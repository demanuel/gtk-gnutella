/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Vidar Madsen & Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Structure for storing meta-information about files being
 * downloaded.
 *
 * @author Vidar Madsen
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$")

#include "fileinfo.h"
#include "file_object.h"
#include "sockets.h"
#include "downloads.h"
#include "uploads.h"
#include "hosts.h"
#include "routing.h"
#include "routing.h"
#include "gmsg.h"
#include "bsched.h"
#include "huge.h"
#include "dmesh.h"
#include "search.h"
#include "guid.h"
#include "share.h"
#include "settings.h"
#include "nodes.h"
#include "namesize.h"
#include "http.h"					/* For http_range_t */
#include "gdht.h"

#include "lib/atoms.h"
#include "lib/ascii.h"
#include "lib/base32.h"
#include "lib/endian.h"
#include "lib/file.h"
#include "lib/halloc.h"
#include "lib/header.h"
#include "lib/idtable.h"
#include "lib/magnet.h"
#include "lib/tigertree.h"
#include "lib/tm.h"
#include "lib/url.h"
#include "lib/utf8.h"
#include "lib/walloc.h"
#include "lib/glib-missing.h"

#include "if/dht/dht.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"			/* Must be the last header included */

#define FI_MIN_CHUNK_SPLIT	512		/**< Smallest chunk we can split */
/**< Max field length we accept to save */
#define FI_MAX_FIELD_LEN	(TTH_RAW_SIZE * TTH_MAX_LEAVES)
#define FI_DHT_PERIOD		1200		/**< Requery period for DHT: 20 min */
#define FI_DHT_SOURCE_DELAY	300			/**< Penalty per known source */
#define FI_DHT_QUEUED_DELAY	150			/**< Penalty per queued source */
#define FI_DHT_RECV_DELAY	600			/**< Penalty per active source */
#define FI_DHT_RECV_THRESH	5			/**< No query if that many active */

enum dl_file_chunk_magic {
	DL_FILE_CHUNK_MAGIC = 0x563b483dU
};

struct dl_file_chunk {
	enum dl_file_chunk_magic magic;
	filesize_t from;				/**< Range offset start (byte included) */
	filesize_t to;					/**< Range offset end (byte EXCLUDED) */
	struct download *download;		/**< Download that "reserved" the range */
	enum dl_chunk_status status;	/**< Status of range */
};

/*
 * File information is uniquely describing an output file in the download
 * directory.  There is a many-to-one relationship between downloads and
 * file information (fileinfo or fi for short): several downloads can point
 * to the same fi, in which case they write data to the SAME file.
 *
 * Files are uniquely indexed by their SHA1 hash.  If two files bear the
 * same SHA1, then they MUST be identical, whatever their reported size.
 * We assume the largest entry is the right one.  Servers should always report
 * the full filesize in hits, but some broken servers will not, hence the
 * possible divergence in file size.
 *
 * When we don't have a SHA1 to identify a file, we use the tuple (name, size)
 * to uniquely identify a file.  The name alone is not enough, since it is
 * conceivable that two equal names could have different sizes, because
 * they are just underlying different files.
 *
 * To lookup for possible aliases, we also keep track of all our fi structs
 * by size in a table indexed solely by filesize and listing all the currently
 * recorded fi structs for that size.
 *
 * The `fi_by_sha1' hash table keeps track of the SHA1 -> fi association.
 * The `fi_by_namesize' hash table keeps track of items by (name, size).
 * The `fi_by_outname' table keeps track of the "output name" -> fi link.
 * The `fi_by_guid' hash table keeps track of the GUID -> fi association.
 */

static GHashTable *fi_by_sha1;
static GHashTable *fi_by_namesize;
static GHashTable *fi_by_outname;
static GHashTable *fi_by_guid;

static const char file_info_file[] = "fileinfo";
static const char file_info_what[] = "the fileinfo database";
static gboolean fileinfo_dirty = FALSE;
static gboolean can_swarm = FALSE;		/**< Set by file_info_retrieve() */

#define	FILE_INFO_MAGIC32 0xD1BB1ED0U
#define	FILE_INFO_MAGIC64 0X91E63640U

typedef guint32 fi_magic_t;

#define FILE_INFO_VERSION	6

enum dl_file_info_field {
	FILE_INFO_FIELD_NAME = 1,	/**< No longer used in 32-bit version >= 3 */
	FILE_INFO_FIELD_ALIAS,
	FILE_INFO_FIELD_SHA1,
	FILE_INFO_FIELD_CHUNK,
	FILE_INFO_FIELD_END,		/**< Marks end of field section */
	FILE_INFO_FIELD_CHA1,
	FILE_INFO_FIELD_GUID,
	FILE_INFO_FIELD_TTH,
	FILE_INFO_FIELD_TIGERTREE,
	/* Add new fields here, never change ordering for backward compatibility */

	NUM_FILE_INFO_FIELDS
};

#define FI_STORE_DELAY		60	/**< Max delay (secs) for flushing fileinfo */
#define FI_TRAILER_INT		6	/**< Amount of guint32 in the trailer */

/**
 * The swarming trailer is built within a memory buffer first, to avoid having
 * to issue mutliple write() system calls.	We can't use stdio's buffering
 * since we can sometime reuse the download's file descriptor.
 */
static struct {
	char *arena;			/**< Base arena */
	char *wptr;			/**< Write pointer */
	const char *rptr;		/**< Read pointer */
	const char *end;		/**< First byte off arena */
	size_t size;			/**< Current size of arena */
} tbuf;

#define TBUF_SIZE			512		/**< Initial trailing buffer size */
#define TBUF_GROW_BITS		9		/**< Growing chunks */

#define TBUF_GROW			((size_t) 1 << TBUF_GROW_BITS)
#define TBUF_GROW_MASK		(TBUF_GROW - 1)

static inline size_t
round_grow(size_t x)
{
	return (x + TBUF_GROW_MASK) & ~TBUF_GROW_MASK;
}

/*
 * Low level trailer buffer read/write macros.
 */

static void
tbuf_check(void)
{
	if (tbuf.arena) {
		g_assert(NULL != tbuf.end);
		g_assert(tbuf.size > 0);
		g_assert(&tbuf.arena[tbuf.size] == tbuf.end);
		if (tbuf.rptr) {
			g_assert((size_t) tbuf.rptr >= (size_t) tbuf.arena);
			g_assert((size_t) tbuf.rptr <= (size_t) tbuf.end);
		}
		if (tbuf.wptr) {
			g_assert((size_t) tbuf.wptr >= (size_t) tbuf.arena);
			g_assert((size_t) tbuf.wptr <= (size_t) tbuf.end);
		}
	} else {
		g_assert(NULL == tbuf.end);
		g_assert(NULL == tbuf.rptr);
		g_assert(NULL == tbuf.wptr);
		g_assert(0 == tbuf.size);
	}
}

/**
 * Make sure there is enough room in the buffer for `x' more bytes.
 * If `writing' is TRUE, we update the write pointer.
 */
static void
tbuf_extend(size_t x, gboolean writing)
{
	size_t new_size = round_grow(x + tbuf.size);
	size_t offset;

	tbuf_check();

	offset = (writing && tbuf.wptr) ? (tbuf.wptr - tbuf.arena) : 0;
	g_assert(offset <= tbuf.size);

	tbuf.arena = g_realloc(tbuf.arena, new_size);
	tbuf.end = &tbuf.arena[new_size];
	tbuf.size = new_size;
	tbuf.wptr = writing ? &tbuf.arena[offset] : NULL;
	tbuf.rptr = writing ? NULL : tbuf.arena;
}

static inline void
TBUF_INIT_READ(size_t size)
{
	tbuf_check();

	if (NULL == tbuf.arena || (size_t) (tbuf.end - tbuf.arena) < size) {
		tbuf_extend(size, FALSE);
	}
	tbuf.rptr = tbuf.arena;
	tbuf.wptr = NULL;
}

static inline void
TBUF_INIT_WRITE(void)
{
	tbuf_check();

	if (NULL == tbuf.arena) {
		tbuf_extend(TBUF_SIZE, TRUE);
	}
	tbuf.rptr = NULL;
	tbuf.wptr = tbuf.arena;
}

static inline size_t
TBUF_WRITTEN_LEN(void)
{
	tbuf_check();

	return tbuf.wptr - tbuf.arena;
}

static inline void
TBUF_CHECK(size_t size)
{
	tbuf_check();

	if (NULL == tbuf.arena || (size_t) (tbuf.end - tbuf.wptr) < size)
		tbuf_extend(size, TRUE);
}

static WARN_UNUSED_RESULT gboolean
TBUF_GETCHAR(guint8 *x)
{
	tbuf_check();
	
	if ((size_t) (tbuf.end - tbuf.rptr) >= sizeof *x) {
		*x = *tbuf.rptr;
		tbuf.rptr += sizeof *x;
		return TRUE;
	} else {
		return FALSE;
	}
}

static WARN_UNUSED_RESULT gboolean
TBUF_GET_UINT32(guint32 *x)
{
	tbuf_check();

	if ((size_t) (tbuf.end - tbuf.rptr) >= sizeof *x) {
		memcpy(x, tbuf.rptr, sizeof *x);
		tbuf.rptr += sizeof *x;
		return TRUE;
	} else {
		return FALSE;
	}
}

static WARN_UNUSED_RESULT gboolean
TBUF_READ(char *x, size_t size)
{
	tbuf_check();

	if ((size_t) (tbuf.end - tbuf.rptr) >= size) {
		memcpy(x, tbuf.rptr, size);
		tbuf.rptr += size;
		return TRUE;
	} else {
		return FALSE;
	}
}

static void
TBUF_PUT_CHAR(guint8 x)
{
	TBUF_CHECK(sizeof x);
	*tbuf.wptr = x;
	tbuf.wptr++;
}

static void
TBUF_PUT_UINT32(guint32 x)
{
	TBUF_CHECK(sizeof x);
	memcpy(tbuf.wptr, &x, sizeof x);
	tbuf.wptr += sizeof x;
}

static void
TBUF_WRITE(const char *data, size_t size)
{
	TBUF_CHECK(size);
	memcpy(tbuf.wptr, data, size);
	tbuf.wptr += size;
}

static inline void
file_info_checksum(guint32 *checksum, gconstpointer data, size_t len)
{
	const guchar *p = data;
	while (len--)
		*checksum = (*checksum << 1) ^ (*checksum >> 31) ^ *p++;
}

/*
 * High-level write macros.
 */

static void
WRITE_CHAR(guint8 val, guint32 *checksum)
{
	TBUF_PUT_CHAR(val);
	file_info_checksum(checksum, &val, sizeof val);
}

static void
WRITE_UINT32(guint32 val, guint32 *checksum)
{
	val = htonl(val);
	TBUF_PUT_UINT32(val);
	file_info_checksum(checksum, &val, sizeof val);
}

static void
WRITE_STR(const char *data, size_t size, guint32 *checksum)
{
	TBUF_WRITE(data, size);
	file_info_checksum(checksum, data, size);
}

/*
 * High-level read macros.
 */

static WARN_UNUSED_RESULT gboolean
READ_CHAR(guint8 *val, guint32 *checksum)
{
	if (TBUF_GETCHAR(val)) {
		file_info_checksum(checksum, val, sizeof *val);
		return TRUE;
	} else {
		return FALSE;
	}
}

static WARN_UNUSED_RESULT gboolean
READ_UINT32(guint32 *val_ptr, guint32 *checksum)
{
	guint32 val;
	
	if (TBUF_GET_UINT32(&val)) {
		*val_ptr = ntohl(val);
		file_info_checksum(checksum, &val, sizeof val);
		return TRUE;
	} else {
		return FALSE;
	}
}

static WARN_UNUSED_RESULT gboolean
READ_STR(char *data, size_t size, guint32 *checksum)
{
	if (TBUF_READ(data, size)) {
		file_info_checksum(checksum, data, size);
		return TRUE;
	} else {
		return FALSE;
	}
}

/*
 * Addition of a variable-size trailer field.
 */

static void
FIELD_ADD(enum dl_file_info_field id, size_t n, gconstpointer data,
	guint32 *checksum)
{
	WRITE_UINT32(id, checksum);
	WRITE_UINT32(n, checksum);
	WRITE_STR(data, n, checksum);
}

/**
 * The trailer fields of the fileinfo trailer.
 */

struct trailer {
	guint64 filesize;		/**< Real file size */
	guint32 generation;		/**< Generation number */
	guint32 length;			/**< Total trailer length */
	guint32 checksum;		/**< Trailer checksum */
	fi_magic_t magic;		/**< Magic number */
};

static fileinfo_t *file_info_retrieve_binary(const char *pathname);
static void fi_free(fileinfo_t *fi);
static void fi_update_seen_on_network(gnet_src_t srcid);
static const char *file_info_new_outname(const char *dir, const char *name);
static gboolean looks_like_urn(const char *filename);

static idtable_t *fi_handle_map;
static idtable_t *src_handle_map;

static event_t *fi_events[EV_FI_EVENTS];
static event_t *src_events[EV_SRC_EVENTS];

struct download *
src_get_download(gnet_src_t src_handle)
{
	return idtable_get_value(src_handle_map, src_handle);
}

static inline fileinfo_t *
file_info_find_by_handle(gnet_fi_t n)
{
	return idtable_get_value(fi_handle_map, n);
}

static inline gnet_fi_t
file_info_request_handle(fileinfo_t *fi)
{
	return idtable_new_id(fi_handle_map, fi);
}

static void
fi_event_trigger(fileinfo_t *fi, gnet_fi_ev_t id)
{
	file_info_check(fi);
	g_assert(UNSIGNED(id) < EV_FI_EVENTS);
	event_trigger(fi_events[id], T_NORMAL(fi_listener_t, (fi->fi_handle)));
}

static void
file_info_drop_handle(fileinfo_t *fi, const char *reason)
{
	file_info_check(fi);

	file_info_upload_stop(fi, reason);
	fi_event_trigger(fi, EV_FI_REMOVED);
	idtable_free_id(fi_handle_map, fi->fi_handle);
}

/**
 * Checks the kind of trailer. The trailer must be initialized.
 *
 * @return TRUE if the trailer is the 64-bit version, FALSE if it's 32-bit.
 */
static inline gboolean
trailer_is_64bit(const struct trailer *tb)
{
	switch (tb->magic) {
	case FILE_INFO_MAGIC32: return FALSE;
	case FILE_INFO_MAGIC64: return TRUE;
	}

	g_assert_not_reached();
	return FALSE;
}

/**
 * Write trailer buffer at current position on `fd', whose name is `name'.
 */
static void
tbuf_write(const struct file_object *fo, filesize_t offset)
{
	size_t size = TBUF_WRITTEN_LEN();
	ssize_t ret;

	g_assert(fo);
	g_assert(size > 0);
	g_assert(size <= tbuf.size);

	ret = file_object_pwrite(fo, tbuf.arena, size, offset);
	if ((ssize_t) -1 == ret || (size_t) ret != size) {
		const char *error;

		error = (ssize_t) -1 == ret ? g_strerror(errno) : "Unknown error";
		g_warning("error while flushing trailer info for \"%s\": %s",
			file_object_get_pathname(fo), error);
	}
}

/**
 * Read trailer buffer at current position from `fd'.
 *
 * @returns -1 on error.
 */
static ssize_t
tbuf_read(int fd, size_t len)
{
	g_assert(fd >= 0);

	TBUF_INIT_READ(len);

	return read(fd, tbuf.arena, len);
}

static struct dl_file_chunk *
dl_file_chunk_alloc(void)
{
	static const struct dl_file_chunk zero_fc;
	struct dl_file_chunk *fc;
   
	fc = walloc(sizeof *fc);
	*fc = zero_fc;
	fc->magic = DL_FILE_CHUNK_MAGIC;
	return fc;
}

static void
dl_file_chunk_check(const struct dl_file_chunk *fc)
{
	g_assert(fc);
	g_assert(DL_FILE_CHUNK_MAGIC == fc->magic);
}

static void
dl_file_chunk_free(struct dl_file_chunk **fc_ptr)
{
	g_assert(fc_ptr);
	if (*fc_ptr) {
		struct dl_file_chunk *fc = *fc_ptr;

		fc->magic = 0;
		wfree(fc, sizeof *fc);
		*fc_ptr = NULL;
	}
}

/**
 * Given a fileinfo GUID, return the fileinfo_t associated with it, or NULL
 * if it does not exist.
 */
fileinfo_t *
file_info_by_guid(const struct guid *guid)
{
	return g_hash_table_lookup(fi_by_guid, guid);
}

/**
 * Checks the chunklist of fi.
 *
 * @param fi		the fileinfo struct to check.
 * @param assertion	no document
 *
 * @return TRUE if chunklist is consistent, FALSE otherwise.
 */
static gboolean
file_info_check_chunklist(const fileinfo_t *fi, gboolean assertion)
{
	GSList *sl;
	filesize_t last = 0;

	/*
	 * This routine ends up being a CPU hog when all the asserts using it
	 * are run.  Do that only when debugging.
	 */

	if (assertion && !GNET_PROPERTY(fileinfo_debug))
		return TRUE;

	file_info_check(fi);

	for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);
		if (last != fc->from || fc->from >= fc->to)
			return FALSE;

		last = fc->to;
		if (!fi->file_size_known || 0 == fi->size)
			continue;
		
		if (fc->from >= fi->size || fc->to > fi->size)
			return FALSE;
	}

	return TRUE;
}

/**
 * Store a binary record of the file metainformation at the end of the
 * supplied file descriptor, opened for writing.
 *
 * When `force' is false, we don't store unless FI_STORE_DELAY seconds
 * have elapsed since last flush to disk.
 */
static void
file_info_fd_store_binary(fileinfo_t *fi, const struct file_object *fo)
{
	const GSList *fclist, *sl;
	guint32 checksum = 0;
	guint32 length;

	g_assert(fo);

	TBUF_INIT_WRITE();
	WRITE_UINT32(FILE_INFO_VERSION, &checksum);

	/*
	 * Emit leading binary fields.
	 */

	WRITE_UINT32(fi->created, &checksum);	/* Introduced at: version 4 */
	WRITE_UINT32(fi->ntime, &checksum);			/* version 4 */
	WRITE_CHAR(fi->file_size_known, &checksum);	/* Introduced at: version 5 */

	/*
	 * Emit variable-length fields.
	 */

	FIELD_ADD(FILE_INFO_FIELD_GUID, GUID_RAW_SIZE, fi->guid, &checksum);

	if (fi->tth)
		FIELD_ADD(FILE_INFO_FIELD_TTH, TTH_RAW_SIZE, fi->tth, &checksum);

	if (fi->tigertree.leaves) {
		gconstpointer data;
		size_t size;
		
		STATIC_ASSERT(TTH_RAW_SIZE == sizeof(struct tth));
		data = fi->tigertree.leaves;
		size = fi->tigertree.num_leaves * TTH_RAW_SIZE;
		FIELD_ADD(FILE_INFO_FIELD_TIGERTREE, size, data, &checksum);
	}

	if (fi->sha1)
		FIELD_ADD(FILE_INFO_FIELD_SHA1, SHA1_RAW_SIZE, fi->sha1, &checksum);

	if (fi->cha1)
		FIELD_ADD(FILE_INFO_FIELD_CHA1, SHA1_RAW_SIZE, fi->cha1, &checksum);

	for (sl = fi->alias; NULL != sl; sl = g_slist_next(sl)) {
		size_t len = strlen(sl->data);		/* Do not store the trailing NUL */
		g_assert(len <= INT_MAX);
		if (len < FI_MAX_FIELD_LEN)
			FIELD_ADD(FILE_INFO_FIELD_ALIAS, len, sl->data, &checksum);
	}

	g_assert(file_info_check_chunklist(fi, TRUE));
	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;
		guint32 from_hi, to_hi;
		guint32 chunk[5];

		dl_file_chunk_check(fc);
		from_hi = (guint64) fc->from >> 32;
		to_hi = (guint64) fc->to >> 32;

		chunk[0] = htonl(from_hi),
		chunk[1] = htonl((guint32) fc->from),
		chunk[2] = htonl(to_hi),
		chunk[3] = htonl((guint32) fc->to),
		chunk[4] = htonl(fc->status);
		FIELD_ADD(FILE_INFO_FIELD_CHUNK, sizeof chunk, chunk, &checksum);
	}

	fi->generation++;

	WRITE_UINT32(FILE_INFO_FIELD_END, &checksum);

	STATIC_ASSERT((guint64) -1 >= (filesize_t) -1);
	WRITE_UINT32((guint64) fi->size >> 32, &checksum);
	WRITE_UINT32(fi->size, &checksum);
	WRITE_UINT32(fi->generation, &checksum);

	length = TBUF_WRITTEN_LEN() + 3 * sizeof(guint32);

	WRITE_UINT32(length, &checksum);				/* Total trailer size */
	WRITE_UINT32(checksum, &checksum);
	WRITE_UINT32(FILE_INFO_MAGIC64, &checksum);

	/* Flush buffer at current position */
	tbuf_write(fo, fi->size);

	if (0 != ftruncate(file_object_get_fd(fo), fi->size + length))
		g_warning("file_info_fd_store_binary(): truncate() failed: %s",
			g_strerror(errno));

	fi->dirty = FALSE;
	fileinfo_dirty = TRUE;
}

/**
 * Store a binary record of the file metainformation at the end of the
 * output file, if it exists.
 */
void
file_info_store_binary(fileinfo_t *fi, gboolean force)
{
	struct file_object *fo;

	g_assert(!(fi->flags & (FI_F_TRANSIENT | FI_F_SEEDING)));

	/*
	 * Don't flush unless required or some delay occurred since last flush.
	 */

	fi->stamp = tm_time();
	if (!force && delta_time(fi->stamp, fi->last_flush) < FI_STORE_DELAY)
		return;
	fi->last_flush = fi->stamp;

	/*
	 * We don't create the file if it does not already exist.  That way,
	 * a file is only created when at least one byte of data is downloaded,
	 * since then we'll go directly to file_info_fd_store_binary().
	 */

	fo = file_object_open(fi->pathname, O_WRONLY);
	if (!fo) {
		int fd = file_open_missing(fi->pathname, O_WRONLY);
		if (fd >= 0) {
			fo = file_object_new(fd, fi->pathname, O_WRONLY);
		}
	}
	if (fo) {
		file_info_fd_store_binary(fi, fo);
		file_object_release(&fo);
	}
}

void
file_info_got_tth(fileinfo_t *fi, const struct tth *tth)
{
	file_info_check(fi);
	
	g_return_if_fail(tth);
	g_return_if_fail(NULL == fi->tth);
	fi->tth = atom_tth_get(tth);
}

static void
fi_tigertree_free(fileinfo_t *fi)
{
	file_info_check(fi);
	g_assert((NULL != fi->tigertree.leaves) ^ (0 == fi->tigertree.num_leaves));

	if (fi->tigertree.leaves) {
		wfree(fi->tigertree.leaves,
				fi->tigertree.num_leaves * sizeof fi->tigertree.leaves[0]);
		fi->tigertree.slice_size = 0;
		fi->tigertree.num_leaves = 0;
		fi->tigertree.leaves = NULL;
	}
}

void
file_info_got_tigertree(fileinfo_t *fi,
	const struct tth *leaves, size_t num_leaves)
{
	filesize_t num_blocks;

	file_info_check(fi);
	
	g_return_if_fail(leaves);
	g_return_if_fail(num_leaves > 0);
	g_return_if_fail(fi->tigertree.num_leaves < num_leaves);
	g_return_if_fail(fi->file_size_known);

	fi_tigertree_free(fi);
	fi->tigertree.leaves = wcopy(leaves, num_leaves * sizeof leaves[0]);
	fi->tigertree.num_leaves = num_leaves;

	fi->tigertree.slice_size = TTH_BLOCKSIZE;
	num_blocks = tt_block_count(fi->size);
	while (num_blocks > fi->tigertree.num_leaves) {
		num_blocks = (num_blocks + 1) / 2;
		fi->tigertree.slice_size *= 2;
	}
	fi->dirty = TRUE;
}

/**
 * Record that the fileinfo trailer has been stripped.
 */
void
file_info_mark_stripped(fileinfo_t *fi)
{
	file_info_check(fi);
	g_return_if_fail(!(FI_F_STRIPPED & fi->flags));

	fi->flags |= FI_F_STRIPPED;
}

static void
file_info_strip_trailer(fileinfo_t *fi, const char *pathname)
{
	file_info_check(fi);
	g_assert(!((FI_F_TRANSIENT | FI_F_SEEDING | FI_F_STRIPPED) & fi->flags));
	
	fi_tigertree_free(fi);

	if (-1 == truncate(pathname, fi->size)) {
		if (ENOENT == errno) {
			file_info_mark_stripped(fi);
		}
		g_warning("could not chop fileinfo trailer off \"%s\": %s",
			pathname, g_strerror(errno));
	} else {
		file_info_mark_stripped(fi);
	}
}

/**
 * Strips the file metainfo trailer off a file.
 */
void
file_info_strip_binary(fileinfo_t *fi)
{
	file_info_strip_trailer(fi, fi->pathname);
}

/**
 * Strips the file metainfo trailer off specified file.
 */
void
file_info_strip_binary_from_file(fileinfo_t *fi, const char *pathname)
{
	fileinfo_t *dfi;

	g_assert(is_absolute_path(pathname));
	g_assert(!(fi->flags & (FI_F_TRANSIENT | FI_F_SEEDING | FI_F_STRIPPED)));

	/*
	 * Before truncating the file, we must be really sure it is reasonnably
	 * matching the fileinfo structure we have for it: retrieve the binary
	 * trailer, and check size / completion.
	 */

	dfi = file_info_retrieve_binary(pathname);

	if (NULL == dfi) {
		g_warning("could not chop fileinfo trailer off \"%s\": file does "
			"not seem to have a valid trailer", pathname);
		return;
	}

	if (dfi->size != fi->size || dfi->done != fi->done) {
		char buf[64];

		concat_strings(buf, sizeof buf,
			uint64_to_string(dfi->done), "/",
			uint64_to_string2(dfi->size), (void *) 0);
		g_warning("could not chop fileinfo trailer off \"%s\": file was "
			"different than expected (%s bytes done instead of %s/%s)",
			pathname, buf,
			uint64_to_string(fi->done), uint64_to_string2(fi->size));
	} else {
		file_info_strip_trailer(fi, pathname);
	}
	fi_free(dfi);
}

/**
 * Frees the chunklist and all its elements of a fileinfo struct. Note that
 * the consistency of the list isn't checked to explicitely allow freeing
 * inconsistent chunklists.
 *
 * @param fi the fileinfo struct.
 */
static void
file_info_chunklist_free(fileinfo_t *fi)
{
	GSList *sl;

	file_info_check(fi);

	for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		struct dl_file_chunk *fc = sl->data;
		dl_file_chunk_free(&fc);
	}
	g_slist_free(fi->chunklist);
	fi->chunklist = NULL;
}

/**
 * Free a `file_info' structure.
 */
static void
fi_free(fileinfo_t *fi)
{
	file_info_check(fi);
	g_assert(!fi->hashed);
	g_assert(NULL == fi->sf);

#if 0
	/* This does not seem to be a bug; see file_info_remove_source(). */
	if (fi->sha1) {
		g_assert(fi != g_hash_table_lookup(fi_by_sha1, fi->sha1));
	}
#endif

	/*
	 * Stop all uploads occurring for this file.
	 */

	if (fi->chunklist) {
		g_assert(file_info_check_chunklist(fi, TRUE));
		file_info_chunklist_free(fi);
	}
	if (fi->alias) {
		GSList *sl;

		for (sl = fi->alias; NULL != sl; sl = g_slist_next(sl)) {
			const char *s = sl->data;
			atom_str_free_null(&s);
		}
		g_slist_free(fi->alias);
		fi->alias = NULL;
	}
	if (fi->seen_on_network) {
		fi_free_ranges(fi->seen_on_network);
	}
	fi_tigertree_free(fi);

	atom_guid_free_null(&fi->guid);
	atom_str_free_null(&fi->pathname);
	atom_tth_free_null(&fi->tth);
	atom_sha1_free_null(&fi->sha1);
	atom_sha1_free_null(&fi->cha1);

	fi->magic = 0;
	wfree(fi, sizeof *fi);
}

static void
file_info_hash_insert_name_size(fileinfo_t *fi)
{
	namesize_t nsk;
	GSList *sl;

	file_info_check(fi);
	g_assert(fi->file_size_known);

	if (FI_F_TRANSIENT & fi->flags)
		return;

	/*
	 * The (name, size) tuples also point to a list of entries, one for
	 * each of the name aliases.  Ideally, we'd want only one, but there
	 * can be name conflicts.  This does not matter unless they disabled
	 * strict SHA1 matching...  but that is a dangerous move.
	 */

	nsk.size = fi->size;

	for (sl = fi->alias; NULL != sl; sl = g_slist_next(sl)) {
		GSList *slist;
		
		nsk.name = sl->data;
		slist = g_hash_table_lookup(fi_by_namesize, &nsk);

		if (NULL != slist) {
			slist = g_slist_append(slist, fi);
		} else {
			namesize_t *ns = namesize_make(nsk.name, nsk.size);
			slist = g_slist_append(slist, fi);
			gm_hash_table_insert_const(fi_by_namesize, ns, slist);
		}
	}
}

/**
 * Resize fileinfo to be `size' bytes, by adding empty chunk at the tail.
 */
static void
fi_resize(fileinfo_t *fi, filesize_t size)
{
	struct dl_file_chunk *fc;

	file_info_check(fi);
	g_assert(fi->size < size);
	g_assert(!fi->hashed);

	fc = dl_file_chunk_alloc();
	fc->from = fi->size;
	fc->to = size;
	fc->status = DL_CHUNK_EMPTY;
	fi->chunklist = g_slist_append(fi->chunklist, fc);

	/*
	 * Don't remove/re-insert `fi' from hash tables: when this routine is
	 * called, `fi' is no longer "hashed", or has never been "hashed".
	 */

	fi->size = size;

	g_assert(file_info_check_chunklist(fi, TRUE));
}

/**
 * Add `name' as an alias for `fi' if not already known.
 * If `record' is TRUE, also record new alias entry in `fi_by_namesize'.
 */
static void
fi_alias(fileinfo_t *fi, const char *name, gboolean record)
{
	namesize_t *ns;
	GSList *list;

	file_info_check(fi);
	g_assert(!record || fi->hashed);	/* record => fi->hashed */

	/*
	 * The fastest way to know if this alias exists is to lookup the
	 * fi_by_namesize table, since all the aliases are inserted into
	 * that table.
	 */
	
	ns = namesize_make(name, fi->size);
	list = g_hash_table_lookup(fi_by_namesize, ns);
	if (NULL != list && NULL != g_slist_find(list, fi)) {
		/* Alias already known */
	} else if (looks_like_urn(name)) {
		/* This is often caused by (URN entries in) the dmesh */
	} else {

		/*
		 * Insert new alias for `fi'.
		 */

		fi->alias = g_slist_append(fi->alias,
						deconstify_gchar(atom_str_get(name)));

		if (record) {
			if (NULL != list)
				list = g_slist_append(list, fi);
			else {
				list = g_slist_append(list, fi);
				gm_hash_table_insert_const(fi_by_namesize, ns, list);
				ns = NULL; /* Prevent freeing */
			}
		}
	}
	if (ns)
		namesize_free(ns);
}

/**
 * Extract fixed trailer at the end of the file `name', already opened as `fd'.
 * The supplied trailer buffer `tb' is filled.
 *
 * @returns TRUE if the trailer is "validated", FALSE otherwise.
 */
static gboolean
file_info_get_trailer(int fd, struct trailer *tb, struct stat *sb,
	const char *name)
{
	ssize_t r;
	fi_magic_t magic;
	guint32 tr[FI_TRAILER_INT];
	struct stat buf;
	off_t offset;
	guint64 filesize_hi;
	size_t i = 0;

	g_assert(fd >= 0);
	g_assert(tb);

	if (-1 == fstat(fd, &buf)) {
		g_warning("error fstat()ing \"%s\": %s", name, g_strerror(errno));
		return FALSE;
	}

	if (sb) {
		*sb = buf;
	}

	if (!S_ISREG(buf.st_mode)) {
		g_warning("Not a regular file: \"%s\"", name);
		return FALSE;
	}

	if (buf.st_size < (off_t) sizeof tr)
		return FALSE;

	/*
	 * Don't use SEEK_END with "-sizeof(tr)" to avoid problems when off_t is
	 * defined as an 8-byte wide quantity.  Since we have the file size
	 * already, better use SEEK_SET.
	 *		--RAM, 02/02/2003 after a bug report from Christian Biere
	 */

	offset = buf.st_size - sizeof tr;		/* Start of trailer */

	/* No wrapper because this is a native off_t value. */
	if (offset != lseek(fd, offset, SEEK_SET)) {
		g_warning("file_info_get_trailer(): "
			"error seek()ing in file \"%s\": %s", name, g_strerror(errno));
		return FALSE;
	}

	r = read(fd, tr, sizeof tr);
	if ((ssize_t) -1 == r) {
		g_warning("file_info_get_trailer(): "
			"error reading trailer in  \"%s\": %s", name, g_strerror(errno));
		return FALSE;
	}


	/*
	 * Don't continue if the number of bytes read is smaller than
	 * the minimum number of bytes needed.
	 *		-- JA 12/02/2004
	 */
	if (r < 0 || (size_t) r < sizeof tr)
		return FALSE;

	filesize_hi = 0;
	magic = ntohl(tr[5]);
	switch (magic) {
	case FILE_INFO_MAGIC64:
		filesize_hi	= ((guint64) ((guint32) ntohl(tr[0]))) << 32;
		/* FALLTHROUGH */
	case FILE_INFO_MAGIC32:
		tb->filesize = filesize_hi | ((guint32) ntohl(tr[1]));
		i = 2;
		break;
	}
	if (2 != i) {
		return FALSE;
	}

	for (/* NOTHING */; i < G_N_ELEMENTS(tr); i++) {
		guint32 v = ntohl(tr[i]);

		switch (i) {
		case 2: tb->generation	= v; break;
		case 3: tb->length		= v; break;
		case 4: tb->checksum	= v; break;
		case 5: tb->magic 		= v; break;
		default:
			g_assert_not_reached();
		}
	}

	g_assert(FILE_INFO_MAGIC32 == tb->magic || FILE_INFO_MAGIC64 == tb->magic);

	/*
	 * Now, sanity checks...  We must make sure this is a valid trailer.
	 */

	if ((guint64) buf.st_size != tb->filesize + tb->length) {
		return FALSE;
	}

	return TRUE;
}

/**
 * Check whether file has a trailer.
 *
 * @return	0 if the file has no trailer
 *			1 if the file has a trailer
 *			-1 on error.
 */
int
file_info_has_trailer(const char *path)
{
	struct trailer trailer;
	int fd;
	gboolean valid;

	fd = file_open_missing(path, O_RDONLY);
	if (fd < 0)
		return -1;

	valid = file_info_get_trailer(fd, &trailer, NULL, path);
	close(fd);

	return valid ? 1 : 0;
}

fileinfo_t *
file_info_by_sha1(const struct sha1 *sha1)
{
	g_return_val_if_fail(sha1, NULL);
	g_return_val_if_fail(fi_by_sha1, NULL);
	return g_hash_table_lookup(fi_by_sha1, sha1);
}

/**
 * Lookup our existing fileinfo structs to see if we can spot one
 * referencing the supplied file `name' and `size', as well as the
 * optional `sha1' hash.
 *
 * @returns the fileinfo structure if found, NULL otherwise.
 */
static fileinfo_t *
file_info_lookup(const char *name, filesize_t size, const struct sha1 *sha1)
{
	fileinfo_t *fi;
	GSList *list;

	/*
	 * If we have a SHA1, this is our unique key.
	 */

	if (sha1) {
		fi = g_hash_table_lookup(fi_by_sha1, sha1);
		if (fi) {
			file_info_check(fi);
			return fi;
		}

		/*
		 * No need to continue if strict SHA1 matching is enabled.
		 * If the entry is not found in the `fi_by_sha1' table, then
		 * nothing can be found for this SHA1.
		 */

		if (GNET_PROPERTY(strict_sha1_matching))
			return NULL;
	}

	if (0 == size)
		return NULL;


	/*
	 * Look for a matching (name, size) tuple.
	 */
	{
		struct namesize nsk;

		nsk.name = deconstify_gchar(name);
		nsk.size = size;

		list = g_hash_table_lookup(fi_by_namesize, &nsk);
		g_assert(!gm_slist_is_looping(list));
		g_assert(!g_slist_find(list, NULL));
	}

	if (NULL != list && NULL == g_slist_next(list)) {
		fi = list->data;
		file_info_check(fi);

		/* FIXME: FILE_SIZE_KNOWN: Should we provide another lookup?
		 *	-- JA 2004-07-21
		 */
		if (fi->file_size_known)
			g_assert(fi->size == size);
		return fi;
	}
	return NULL;
}

/**
 * Given a fileinfo structure, look for any other known duplicate.
 *
 * @returns the duplicate found, or NULL if no duplicate was found.
 */
static fileinfo_t *
file_info_lookup_dup(fileinfo_t *fi)
{
	fileinfo_t *dfi;

	file_info_check(fi);
	g_assert(fi->pathname);

	dfi = g_hash_table_lookup(fi_by_outname, fi->pathname);
	if (dfi) {
		file_info_check(dfi);
		return dfi;
	}

	/*
	 * If `fi' has a SHA1, find any other entry bearing the same SHA1.
	 */

	if (fi->sha1) {
		dfi = g_hash_table_lookup(fi_by_sha1, fi->sha1);
		if (dfi) {
			file_info_check(dfi);
			return dfi;
		}
	}

	/*
	 * The file ID must also be unique.
	 */

	g_assert(fi->guid);
	dfi = g_hash_table_lookup(fi_by_guid, fi->guid);
	if (dfi) {
		file_info_check(dfi);
		return dfi;
	}
	return NULL;
}

/**
 * Check whether filename looks like an URN.
 */
static gboolean
looks_like_urn(const char *filename)
{
	const char *p, *q;
	guint i;

	/* Check for the following pattern:
	 *
	 * (urn.)?(sha1|bitprint).[a-zA-Z0-9]{SHA1_BASE32_SIZE,}
	 */
	
	p = is_strcaseprefix(filename, "urn");
	/* Skip a single character after the prefix */
	if (p) {
	   	if ('\0' == *p++)
			return FALSE;
	} else {
		p = filename;
	}
	
	q = is_strcaseprefix(p, "sha1");
	if (!q)
		q = is_strcaseprefix(p, "bitprint");

	/* Skip a single character after the prefix */
	if (!q || '\0' == *q++)
		return FALSE;

	i = 0;
	while (i < SHA1_BASE32_SIZE && is_ascii_alnum(q[i]))
		i++;

	return i < SHA1_BASE32_SIZE ? FALSE : TRUE;
}

/**
 * Determines a human-readable filename for the file, using heuristics to
 * skip what looks like an URN.
 *
 * @returns a pointer to the information in the fileinfo, but this must be
 * duplicated should it be perused later.
 */
const char *
file_info_readable_filename(const fileinfo_t *fi)
{
	const GSList *sl;
	const char *filename;

	file_info_check(fi);

	filename = filepath_basename(fi->pathname);
	if (looks_like_urn(filename)) {
		for (sl = fi->alias; sl; sl = g_slist_next(sl)) {
			const char *name = sl->data;
			if (!looks_like_urn(name))
				return name;
		}
	}

	return filename;
}

/**
 * Look whether we have a partially downloaded file bearing the given SHA1.
 * If we do, return a "shared_file" structure suitable for uploading the
 * parts of the file we have (will happen only when PFSP-server is enabled).
 *
 * @return NULL if don't have any download with this SHA1, otherwise return
 * a "shared_file" structure suitable for uploading the parts of the file
 * we have (which will happen only when PFSP-server is enabled).
 */
shared_file_t *
file_info_shared_sha1(const struct sha1 *sha1)
{
	fileinfo_t *fi;

	fi = g_hash_table_lookup(fi_by_sha1, sha1);
	if (fi) {
		file_info_check(fi);

		/*
		 * Completed file (with SHA-1 verified) are always shared, regardless
		 * of their size.
		 *
		 * Partial files below the minimum filesize are not shared, since
		 * their SHA-1 is not yet validated and we don't partially validate
		 * chunks based on the TTH.
		 */

		if (FI_F_SEEDING & fi->flags)
			goto share;

		if (fi->done > 0 && fi->size >= GNET_PROPERTY(pfsp_minimum_filesize))
			goto share;
	}
	return NULL;

share:
	/*
	 * Build shared_file entry if not already present.
	 */

	g_assert(NULL != fi);
	g_assert(NULL != fi->sha1);

	if (fi->sf) {
		shared_file_check(fi->sf);
	} else {
		shared_file_from_fileinfo(fi);
		file_info_changed(fi);
	}
	return fi->sf;
}

/**
 * Allocate random GUID to use as the file ID.
 *
 * @return a GUID atom, refcount incremented already.
 */
static const guid_t *
fi_random_guid_atom(void)
{
	struct guid guid;
	size_t i;

	/*
	 * Paranoid, in case the random number generator is broken.
	 */

	for (i = 0; i < 100; i++) {
		guid_random_fill(&guid);

		if (NULL == g_hash_table_lookup(fi_by_guid, &guid))
			return atom_guid_get(&guid);
	}

	g_error("no luck with random number generator");
	return NULL;
}

/**
 * Ensure potentially old fileinfo structure is brought up-to-date by
 * inferring or allocating missing fields.
 *
 * @return TRUE if an upgrade was necessary.
 */
static gboolean
fi_upgrade_older_version(fileinfo_t *fi)
{
	gboolean upgraded = FALSE;

	file_info_check(fi);

	/*
	 * Ensure proper timestamps for creation and update times.
	 */

	if (0 == fi->created) {
		fi->created = tm_time();
		upgraded = TRUE;
	}

	if (0 == fi->ntime) {
		fi->ntime = fi->created;
		upgraded = TRUE;
	}

	/*
	 * Enforce "size != 0 => file_size_known".
	 */

	if (fi->size && !fi->file_size_known) {
		fi->file_size_known = TRUE;
		upgraded = TRUE;
	}

	/*
	 * Versions before 2005-08-27 lacked the GUID in the fileinfo.
	 */

	if (NULL == fi->guid) {
		fi->guid = fi_random_guid_atom();
		upgraded = TRUE;
	}

	return upgraded;
}

static void
fi_tigertree_check(fileinfo_t *fi)
{
	if (fi->tigertree.leaves) {
		unsigned depth;
		struct tth root;

		if (NULL == fi->tth) {
			g_warning("Trailer contains tigertree but no root hash");
			goto discard;
		}

		depth = tt_depth(fi->tigertree.num_leaves);

		if (
			fi->file_size_known &&
			fi->tigertree.num_leaves != tt_node_count_at_depth(fi->size, depth)
		) {
			g_warning("Trailer contains tigertree with invalid leaf count");
			goto discard;
		}

		STATIC_ASSERT(TTH_RAW_SIZE == sizeof(struct tth));
		root = tt_root_hash(fi->tigertree.leaves, fi->tigertree.num_leaves);
		if (!tth_eq(&root, fi->tth)) {
			g_warning("Trailer contains tigertree with non-matching root hash");
			goto discard;
		}
	}
	return;

discard:
	fi_tigertree_free(fi);
}

/**
 * Reads the file metainfo from the trailer of a file, if it exists.
 *
 * @returns a pointer to the info structure if found, and NULL otherwise.
 */
static fileinfo_t *
file_info_retrieve_binary(const char *pathname)
{
	guint32 tmpchunk[5];
	guint32 tmpguint;
	guint32 checksum = 0;
	fileinfo_t *fi = NULL;
	enum dl_file_info_field field;
	char tmp[FI_MAX_FIELD_LEN + 1];	/* +1 for trailing NUL on strings */
	const char *reason;
	int fd;
	guint32 version;
	struct trailer trailer;
	struct stat sb;
	gboolean t64;
	GSList *chunklist = NULL;

#define BAILOUT(x)			\
G_STMT_START {				\
	reason = (x);			\
	goto bailout;			\
	/* NOTREACHED */		\
} G_STMT_END

	g_assert(NULL != pathname);
	g_assert(is_absolute_path(pathname));

	fd = file_open_missing(pathname, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}

	if (!file_info_get_trailer(fd, &trailer, &sb, pathname)) {
		BAILOUT("could not find trailer");
		/* NOT REACHED */
	}
	t64 = trailer_is_64bit(&trailer);

	{
		gboolean ret;
		
		if (trailer.filesize > (filesize_t) -1) {
			errno = ERANGE;
			ret = -1;
		} else {
			ret = seek_to_filepos(fd, trailer.filesize);
		}
		if (0 != ret) {
			g_warning("seek to position %s within \"%s\" failed: %s",
				uint64_to_string(trailer.filesize),
				pathname, g_strerror(errno));
			goto eof;
		}
	}

	/*
	 * Now read the whole trailer in memory.
	 */

	if (-1 == tbuf_read(fd, trailer.length)) {
		g_warning("file_info_retrieve_binary(): "
			"unable to read whole trailer %s bytes) from \"%s\": %s",
			uint64_to_string(trailer.filesize), pathname, g_strerror(errno));
		goto eof;
	}

	/* Check version */
	if (!READ_UINT32(&version, &checksum))
		goto eof;
	if ((t64 && version > FILE_INFO_VERSION) || (!t64 && version > 5)) {
		g_warning("file_info_retrieve_binary(): strange version; %u", version);
		goto eof;
	}

	fi = walloc0(sizeof *fi);

	fi->magic = FI_MAGIC;
	fi->pathname = atom_str_get(pathname);
	fi->size = trailer.filesize;
	fi->generation = trailer.generation;
	fi->file_size_known = fi->use_swarming = 1;		/* Must assume swarming */
	fi->refcount = 0;
	fi->seen_on_network = NULL;
	fi->modified = sb.st_mtime;

	/*
	 * Read leading binary fields.
	 */

	if (version >= 4) {
		guint32 val;

		if (!READ_UINT32(&val, &checksum))
			goto eof;
		fi->created = val;
		if (!READ_UINT32(&val, &checksum))
			goto eof;
		fi->ntime = val;
	}

	if (version >= 5) {
		guint8 c;
		if (!READ_CHAR(&c, &checksum))
			goto eof;
		fi->file_size_known = 0 != c;
	}

	/*
	 * Read variable-length fields.
	 */

	for (;;) {
		tmpguint = FILE_INFO_FIELD_END; /* in case read() fails. */
		if (!READ_UINT32(&tmpguint, &checksum))		/* Read a field ID */
			goto eof;
		if (FILE_INFO_FIELD_END == tmpguint)
			break;
		field = tmpguint;

		if (!READ_UINT32(&tmpguint, &checksum))	/* Read field data length */
			goto eof;

		if (0 == tmpguint) {
			gm_snprintf(tmp, sizeof tmp, "field #%d has zero size", field);
			BAILOUT(tmp);
			/* NOT REACHED */
		}

		if (tmpguint > FI_MAX_FIELD_LEN) {
			gm_snprintf(tmp, sizeof tmp,
				"field #%d is too large (%u bytes) ", field, (guint) tmpguint);
			BAILOUT(tmp);
			/* NOT REACHED */
		}

		g_assert(tmpguint < sizeof tmp);

		if (!READ_STR(tmp, tmpguint, &checksum))
			goto eof;
		tmp[tmpguint] = '\0';				/* Did not store trailing NUL */

		switch (field) {
		case FILE_INFO_FIELD_NAME:
			/*
			 * Starting with version 3, the file name is added as an alias.
			 * We don't really need to carry the filename in the file itself!
			 */
			if (version >= 3)
				g_warning("found NAME field in fileinfo v%u for \"%s\"",
					version, pathname);
			else
				fi_alias(fi, tmp, FALSE);	/* Pre-v3 lacked NAME in ALIA */
			break;
		case FILE_INFO_FIELD_ALIAS:
			fi_alias(fi, tmp, FALSE);
			break;
		case FILE_INFO_FIELD_GUID:
			if (GUID_RAW_SIZE == tmpguint)
				fi->guid = atom_guid_get(cast_to_guid_ptr_const(tmp));
			else
				g_warning("bad length %d for GUID in fileinfo v%u for \"%s\"",
					tmpguint, version, pathname);
			break;
		case FILE_INFO_FIELD_TTH:
			if (TTH_RAW_SIZE == tmpguint) {
				struct tth tth;
				memcpy(tth.data, tmp, TTH_RAW_SIZE);
				file_info_got_tth(fi, &tth);
			} else {
				g_warning("bad length %d for TTH in fileinfo v%u for \"%s\"",
					tmpguint, version, pathname);
			}
			break;
		case FILE_INFO_FIELD_TIGERTREE:
			if (tmpguint > 0 && 0 == (tmpguint % TTH_RAW_SIZE)) {
				const struct tth *leaves;
				
				STATIC_ASSERT(TTH_RAW_SIZE == sizeof(struct tth));
				leaves = (const struct tth *) &tmp[0];
				file_info_got_tigertree(fi, leaves, tmpguint / TTH_RAW_SIZE);
			} else {
				g_warning("bad length %d for TIGERTREE in fileinfo v%u "
					"for \"%s\"",
					tmpguint, version, pathname);
			}
			break;
		case FILE_INFO_FIELD_SHA1:
			if (SHA1_RAW_SIZE == tmpguint) {
				struct sha1 sha1;
				memcpy(sha1.data, tmp, SHA1_RAW_SIZE);
				fi->sha1 = atom_sha1_get(&sha1);
			} else
				g_warning("bad length %d for SHA1 in fileinfo v%u for \"%s\"",
					tmpguint, version, pathname);
			break;
		case FILE_INFO_FIELD_CHA1:
			if (SHA1_RAW_SIZE == tmpguint) {
				struct sha1 sha1;
				memcpy(sha1.data, tmp, SHA1_RAW_SIZE);
				fi->cha1 = atom_sha1_get(&sha1);
			} else
				g_warning("bad length %d for CHA1 in fileinfo v%u for \"%s\"",
					tmpguint, version, pathname);
			break;
		case FILE_INFO_FIELD_CHUNK:
			{
				struct dl_file_chunk *fc;

				memcpy(tmpchunk, tmp, sizeof tmpchunk);
				fc = dl_file_chunk_alloc();

				if (!t64) {
					g_assert(version < 6);

					/*
			 	 	 * In version 1, fields were written in native form.
			 	 	 * Starting with version 2, they are written in network
					 * order.
			 	 	 */

			   		if (1 == version) {
						fc->from = tmpchunk[0];
						fc->to = tmpchunk[1];
						fc->status = tmpchunk[2];
					} else {
						fc->from = ntohl(tmpchunk[0]);
						fc->to = ntohl(tmpchunk[1]);
						fc->status = ntohl(tmpchunk[2]);
					}
				} else {
					guint64 hi, lo;

					g_assert(version >= 6);
					hi = ntohl(tmpchunk[0]);
					lo = ntohl(tmpchunk[1]);
					fc->from = (hi << 32) | lo;
					hi = ntohl(tmpchunk[2]);
					lo = ntohl(tmpchunk[3]);
					fc->to = (hi << 32) | lo;
					fc->status = ntohl(tmpchunk[4]);
				}

				if (DL_CHUNK_BUSY == fc->status)
					fc->status = DL_CHUNK_EMPTY;

				/* Prepend now and reverse later for better efficiency */
				chunklist = g_slist_prepend(chunklist, fc);
			}
			break;
		default:
			g_warning("file_info_retrieve_binary(): "
				"unhandled field ID %u (%d bytes long)", field, tmpguint);
			break;
		}
	}

	fi->chunklist = g_slist_reverse(chunklist);
	if (!file_info_check_chunklist(fi, FALSE)) {
		file_info_chunklist_free(fi);
		BAILOUT("File contains inconsistent chunk list");
		/* NOT REACHED */
	}

	/*
	 * Pre-v4 (32-bit) trailers lacked the created and ntime fields.
	 * Pre-v5 (32-bit) trailers lacked the fskn (file size known) indication.
	 */

	if (version < 4)
		fi->ntime = fi->created = tm_time();

	if (version < 5)
		fi->file_size_known = TRUE;

	fi_upgrade_older_version(fi);

	/*
	 * If the fileinfo appendix was coherent sofar, we must have reached
	 * the fixed-size trailer that we already parsed eariler.  However,
	 * in case there was an application crash (kill -9) in the middle of
	 * a write(), or a machine crash, some data can be non-consistent.
	 *
	 * Read back the trailer fileds before the checksum to get an accurate
	 * checksum recomputation, but don't assert that what we read matches
	 * the trailer we already parsed.
	 */

	/* file size */
	if (t64) {
		/* Upper 32 bits since version 6 */
		if (!READ_UINT32(&tmpguint, &checksum))
			goto eof;
	}
	if (!READ_UINT32(&tmpguint, &checksum))		/* Lower bits */
		goto eof;

	if (!READ_UINT32(&tmpguint, &checksum))		/* generation number */
		goto eof;
	if (!READ_UINT32(&tmpguint, &checksum))		/* trailer length */
		goto eof;

	if (checksum != trailer.checksum) {
		BAILOUT("checksum mismatch");
		/* NOT REACHED */
	}

	close(fd);
	fd = -1;

	fi_tigertree_check(fi);
	file_info_merge_adjacent(fi);	/* Update fi->done */

	if (GNET_PROPERTY(fileinfo_debug) > 3)
		g_message("FILEINFO: "
			"good trailer info (v%u, %s bytes) in \"%s\"",
			version, uint64_to_string(trailer.length), pathname);

	return fi;

bailout:

	g_warning("file_info_retrieve_binary(): %s in \"%s\"", reason, pathname);

eof:
	if (fi) {
		fi_free(fi);
		fi = NULL;
	}
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
	return NULL;
#undef BAILOUT
}

/**
 * Stores a file info record to the config_dir/fileinfo file, and
 * appends it to the output file in question if needed.
 */
static void
file_info_store_one(FILE *f, fileinfo_t *fi)
{
	const GSList *sl;
	char *path;

	file_info_check(fi);

	if (fi->flags & (FI_F_TRANSIENT | FI_F_SEEDING | FI_F_STRIPPED))
		return;

	if (fi->use_swarming && fi->dirty) {
		file_info_store_binary(fi, FALSE);
	}

	/*
	 * Keep entries for incomplete or not even started downloads so that the
	 * download is started/resumed as soon as a search gains a source.
	 */

	if (0 == fi->refcount && fi->done == fi->size) {
		struct stat st;

		if (-1 == stat(fi->pathname, &st)) {
			return; 	/* Skip: not referenced, and file no longer exists */
		}
	}

	path = filepath_directory(fi->pathname);
	fprintf(f,
		"# refcount %u\n"
		"NAME %s\n"
		"PATH %s\n"
		"GUID %s\n"
		"GENR %u\n",
		fi->refcount,
		filepath_basename(fi->pathname),
		path,
		guid_hex_str(fi->guid),
		fi->generation);
	G_FREE_NULL(path);

	for (sl = fi->alias; NULL != sl; sl = g_slist_next(sl)) {
		const char *alias = sl->data;

		g_assert(NULL != alias);
		if (looks_like_urn(alias)) {
			g_warning("skipping fileinfo alias which looks like a urn: "
				"\"%s\" (filename=\"%s\")",
				alias, filepath_basename(fi->pathname));
		} else
			fprintf(f, "ALIA %s\n", alias);
	}

	if (fi->sha1)
		fprintf(f, "SHA1 %s\n", sha1_base32(fi->sha1));
	if (fi->tth)
		fprintf(f, "TTH %s\n", tth_base32(fi->tth));
	if (fi->cha1)
		fprintf(f, "CHA1 %s\n", sha1_base32(fi->cha1));

	fprintf(f, "SIZE %s\n", uint64_to_string(fi->size));
	fprintf(f, "FSKN %u\n", fi->file_size_known ? 1 : 0);
	fprintf(f, "PAUS %u\n", (FI_F_PAUSED & fi->flags) ? 1 : 0);
	fprintf(f, "DONE %s\n", uint64_to_string(fi->done));
	fprintf(f, "TIME %s\n", uint64_to_string(fi->stamp));
	fprintf(f, "CTIM %s\n", uint64_to_string(fi->created));
	fprintf(f, "NTIM %s\n", uint64_to_string(fi->ntime));
	fprintf(f, "SWRM %u\n", fi->use_swarming ? 1 : 0);

	g_assert(file_info_check_chunklist(fi, TRUE));
	for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);
		fprintf(f, "CHNK %s %s %u\n",
			uint64_to_string(fc->from), uint64_to_string2(fc->to),
			(guint) fc->status);
	}
	fprintf(f, "\n");
}

/**
 * Callback for hash table iterator. Used by file_info_store().
 */
static void
file_info_store_list(gpointer key, gpointer value, gpointer user_data)
{
	fileinfo_t *fi;

	fi = value;
	file_info_check(fi);
	g_assert(key == fi->pathname);
	file_info_store_one(user_data, fi);
}

/**
 * Stores the list of output files and their metainfo to the
 * configdir/fileinfo database.
 */
void
file_info_store(void)
{
	FILE *f;
	file_path_t fp;

	file_path_set(&fp, settings_config_dir(), file_info_file);
	f = file_config_open_write(file_info_what, &fp);

	if (!f)
		return;

	file_config_preamble(f, "Fileinfo database");

	fputs(
		"#\n"
		"# Format is:\n"
		"#	NAME <file name>\n"
		"#	PATH <path>\n"
		"#	GUID <file ID>\n"
		"#	GENR <generation number>\n"
		"#	ALIA <alias file name>\n"
		"#	SIZE <size>\n"
		"#	FSKN <boolean; file_size_known>\n"
		"#	PAUS <boolean; paused>\n"
		"#	SHA1 <server sha1>\n"
		"#	TTH  <server tth>\n"
		"#	CHA1 <computed sha1> [when done only]\n"
		"#	DONE <bytes done>\n"
		"#	TIME <last update stamp>\n"
		"#	CTIM <entry creation time>\n"
		"#	NTIM <time when new source was seen>\n"
		"#	SWRM <boolean; use_swarming>\n"
		"#	CHNK <start> <end+1> <0=hole, 1=busy, 2=done>\n"
		"#	<blank line>\n"
		"#\n\n",
		f
	);

	g_hash_table_foreach(fi_by_outname, file_info_store_list, f);

	file_config_close(f, &fp);
	fileinfo_dirty = FALSE;
}

/**
 * Store global file information cache if dirty.
 */
void
file_info_store_if_dirty(void)
{
	if (fileinfo_dirty)
		file_info_store();
}

/*
 * Notify interested parties that file info is being removed and free
 * its handle.  Used mainly during final cleanup.
 */
static void
fi_dispose(fileinfo_t *fi)
{
	file_info_check(fi);

	file_info_drop_handle(fi, "Shutting down");

	/*
	 * Note that normally all fileinfo structures should have been collected
	 * during the freeing of downloads, so if we come here with a non-zero
	 * refcount, something is wrong with our memory management.
	 *
	 * (refcount of zero is possible if we have a fileinfo entry but no
	 * download attached to that fileinfo)
	 */

	if (fi->refcount)
		g_warning("fi_dispose() refcount = %u for \"%s\"",
			fi->refcount, fi->pathname);

	fi->hashed = FALSE;
	fi_free(fi);
}

/**
 * Callback for hash table iterator. Used by file_info_close().
 */
static void
file_info_free_sha1_kv(gpointer key, gpointer val, gpointer unused_x)
{
	const struct sha1 *sha1 = key;
	const fileinfo_t *fi = val;

	(void) unused_x;
	file_info_check(fi);
	g_assert(sha1 == fi->sha1);		/* SHA1 shared with fi's, don't free */

	/* fi structure in value not freed, shared with other hash tables */
}

/**
 * Callback for hash table iterator. Used by file_info_close().
 */
static void
file_info_free_namesize_kv(gpointer key, gpointer val, gpointer unused_x)
{
	namesize_t *ns = key;
	GSList *list = val;

	(void) unused_x;
	namesize_free(ns);
	g_slist_free(list);

	/* fi structure in value not freed, shared with other hash tables */
}

/**
 * Callback for hash table iterator. Used by file_info_close().
 */
static void
file_info_free_guid_kv(gpointer key, gpointer val, gpointer unused_x)
{
	const struct guid *guid = key;
	fileinfo_t *fi = val;

	(void) unused_x;
	file_info_check(fi);
	g_assert(guid == fi->guid);		/* GUID shared with fi's, don't free */

	/*
	 * fi structure in value not freed, shared with other hash tables
	 * However, transient file info are only in this hash, so free them!
	 */

	if (fi->flags & FI_F_TRANSIENT)
		fi_dispose(fi);
}

/**
 * Callback for hash table iterator. Used by file_info_close().
 */
static void
file_info_free_outname_kv(gpointer key, gpointer val, gpointer unused_x)
{
	const char *name = key;
	fileinfo_t *fi = val;

	(void) unused_x;
	file_info_check(fi);

	/* name shared with fi's, don't free */
	g_assert(name == fi->pathname);

	/*
	 * This table is the last one to be freed, and it is also guaranteed to
	 * contain ALL fileinfo, and only ONCE, by definition.  Thus freeing
	 * happens here.
	 */

	fi_dispose(fi);
}

/**
 * Signals that some information in the fileinfo has changed, warranting
 * a display update in the GUI.
 */
void
file_info_changed(fileinfo_t *fi)
{
	file_info_check(fi);
	g_return_if_fail(fi->hashed);

	fi_event_trigger(fi, EV_FI_STATUS_CHANGED);
}

static void
src_event_trigger(struct download *d, gnet_src_ev_t id)
{
	fileinfo_t *fi;

	download_check(d);
	g_assert(d->src_handle_valid);

	fi = d->file_info;
	file_info_check(fi);

	g_assert(UNSIGNED(id) < EV_SRC_EVENTS);
	event_trigger(src_events[id], T_NORMAL(src_listener_t, (d->src_handle)));
}

void
fi_src_status_changed(struct download *d)
{
	src_event_trigger(d, EV_SRC_STATUS_CHANGED);
}

void
fi_src_info_changed(struct download *d)
{
	src_event_trigger(d, EV_SRC_INFO_CHANGED);
}

void
fi_src_ranges_changed(struct download *d)
{
	src_event_trigger(d, EV_SRC_RANGES_CHANGED);
}

/**
 * Pre-close some file_info information.
 * This should be separate from file_info_close so that we can avoid circular
 * dependencies with other close routines, in this case with download_close.
 */
void
file_info_close_pre(void)
{
	src_remove_listener(fi_update_seen_on_network, EV_SRC_RANGES_CHANGED);
}

/**
 * Close and free all file_info structs in the list.
 */
void
file_info_close(void)
{
	unsigned i;

	/*
	 * Freeing callbacks expect that the freeing of the `fi_by_outname'
	 * table will free the referenced `fi' (since that table MUST contain
	 * all the known `fi' structs by definition).
	 */

	g_hash_table_foreach(fi_by_sha1, file_info_free_sha1_kv, NULL);
	g_hash_table_foreach(fi_by_namesize, file_info_free_namesize_kv, NULL);
	g_hash_table_foreach(fi_by_guid, file_info_free_guid_kv, NULL);
	g_hash_table_foreach(fi_by_outname, file_info_free_outname_kv, NULL);

	g_assert(0 == idtable_ids(src_handle_map));
	idtable_destroy(src_handle_map);

	for (i = 0; i < G_N_ELEMENTS(src_events); i++) {
		event_destroy(src_events[i]);
	}

	/*
	 * The hash tables may still not be completely empty, but the referenced
	 * file_info structs are all freed.
	 *      --Richard, 9/3/2003
	 */

	g_assert(0 == idtable_ids(fi_handle_map));
	idtable_destroy(fi_handle_map);

	for (i = 0; i < G_N_ELEMENTS(fi_events); i++) {
		event_destroy(fi_events[i]);
	}
	g_hash_table_destroy(fi_by_sha1);
	g_hash_table_destroy(fi_by_namesize);
	g_hash_table_destroy(fi_by_guid);
	g_hash_table_destroy(fi_by_outname);

	G_FREE_NULL(tbuf.arena);
}

/**
 * Inserts a file_info struct into the hash tables.
 */
static void
file_info_hash_insert(fileinfo_t *fi)
{
	const fileinfo_t *xfi;

	file_info_check(fi);
	g_assert(!fi->hashed);
	g_assert(fi->guid);

	if (GNET_PROPERTY(fileinfo_debug) > 4)
		g_message("FILEINFO insert 0x%p \"%s\" "
			"(%s/%s bytes done) sha1=%s",
			cast_to_gconstpointer(fi), fi->pathname,
			uint64_to_string(fi->done), uint64_to_string2(fi->size),
			fi->sha1 ? sha1_base32(fi->sha1) : "none");

	/*
	 * Transient fileinfo is only recorded in the GUID hash table.
	 */

	if (fi->flags & FI_F_TRANSIENT)
		goto transient;

	/*
	 * If an entry already exists in the `fi_by_outname' table, then it
	 * is for THIS fileinfo.  Otherwise, there's a structural assertion
	 * that has been broken somewhere!
	 *		--RAM, 01/09/2002
	 */

	xfi = g_hash_table_lookup(fi_by_outname, fi->pathname);
	if (xfi) {
		file_info_check(xfi);
		g_assert(xfi == fi);
	} else { 
		gm_hash_table_insert_const(fi_by_outname, fi->pathname, fi);
	}

	/*
	 * Likewise, there can be only ONE entry per given SHA1, but the SHA1
	 * may not be already present at this time, so the entry is optional.
	 * If it exists, it must be unique though.
	 *		--RAM, 01/09/2002
	 */

	if (fi->sha1) {
		xfi = g_hash_table_lookup(fi_by_sha1, fi->sha1);

		if (NULL != xfi && xfi != fi)		/* See comment above */
			g_error("xfi = 0x%lx, fi = 0x%lx", (gulong) xfi, (gulong) fi);

		if (NULL == xfi)
			gm_hash_table_insert_const(fi_by_sha1, fi->sha1, fi);
	}

	if (fi->file_size_known) {
		file_info_hash_insert_name_size(fi);
	}

transient:
	/*
	 * Obviously, GUID entries must be unique as well.
	 */

	xfi = g_hash_table_lookup(fi_by_guid, fi->guid);

	if (NULL != xfi && xfi != fi)		/* See comment above */
		g_error("xfi = 0x%lx, fi = 0x%lx", (gulong) xfi, (gulong) fi);

	if (NULL == xfi)
		gm_hash_table_insert_const(fi_by_guid, fi->guid, fi);

	/*
	 * Notify interested parties, update counters.
	 */

	fi->hashed = TRUE;
    fi->fi_handle = file_info_request_handle(fi);

	gnet_prop_incr_guint32(PROP_FI_ALL_COUNT);

    fi_event_trigger(fi, EV_FI_ADDED);
}

/**
 * Remove fileinfo data from all the hash tables.
 */
static void
file_info_hash_remove(fileinfo_t *fi)
{
	const fileinfo_t *xfi;
	namesize_t nsk;
	gboolean found;

	file_info_check(fi);
	g_assert(fi->hashed);
	g_assert(fi->guid);

	if (GNET_PROPERTY(fileinfo_debug) > 4) {
		g_message("FILEINFO remove 0x%lx \"%s\" "
			"(%s/%s bytes done) sha1=%s\n",
			(gulong) fi, fi->pathname,
			uint64_to_string(fi->done), uint64_to_string2(fi->size),
			fi->sha1 ? sha1_base32(fi->sha1) : "none");
	}

	file_info_drop_handle(fi, "Discarding file info");

	g_assert(GNET_PROPERTY(fi_all_count) > 0);
	gnet_prop_decr_guint32(PROP_FI_ALL_COUNT);

	/*
	 * Transient fileinfo is only recorded in the GUID hash table.
	 */

	if (fi->flags & FI_F_TRANSIENT)
		goto transient;

	/*
	 * Remove from plain hash tables: by output name, by SHA1 and by GUID.
	 */

	xfi = g_hash_table_lookup(fi_by_outname, fi->pathname);
	if (xfi) {
		file_info_check(xfi);
		g_assert(xfi == fi);
		g_hash_table_remove(fi_by_outname, fi->pathname);
	}

	if (fi->sha1)
		g_hash_table_remove(fi_by_sha1, fi->sha1);

	if (fi->file_size_known) {
		GSList *sl, *head;

		/*
		 * Remove all the aliases from the (name, size) table.
		 */

		nsk.size = fi->size;

		for (sl = fi->alias; NULL != sl; sl = g_slist_next(sl)) {
			namesize_t *ns;
			GSList *slist;
			gpointer key, value;

			nsk.name = sl->data;

			found = g_hash_table_lookup_extended(fi_by_namesize, &nsk,
						&key, &value);

			ns = key;
			slist = value;
			g_assert(found);
			g_assert(NULL != slist);
			g_assert(ns->size == fi->size);

			head = slist;
			slist = g_slist_remove(slist, fi);

			if (NULL == slist) {
				g_hash_table_remove(fi_by_namesize, ns);
				namesize_free(ns);
			} else if (head != slist) {
				gm_hash_table_insert_const(fi_by_namesize, ns, slist);
				/* Head changed */
			}
		}
	}

transient:
	g_hash_table_remove(fi_by_guid, fi->guid);

	fi->hashed = FALSE;
}

/**
 * Stop all sharing occuring for this fileinfo.
 */
void
file_info_upload_stop(fileinfo_t *fi, const char *reason)
{
	file_info_check(fi);

	if (fi->sf) {
		upload_stop_all(fi, reason);
		shared_file_unref(&fi->sf);
		fi->flags &= ~FI_F_SEEDING;
		file_info_changed(fi);
		fileinfo_dirty = TRUE;
	}
}

void
file_info_resume(fileinfo_t *fi)
{
	file_info_check(fi);

	if (FI_F_PAUSED & fi->flags) {
		fi->flags &= ~FI_F_PAUSED;
		file_info_changed(fi);
	}
}

void
file_info_pause(fileinfo_t *fi)
{
	file_info_check(fi);

	if (!(FI_F_PAUSED & fi->flags)) {
		fi->flags |= FI_F_PAUSED;
		file_info_changed(fi);
		fileinfo_dirty = TRUE;
	}
}

/**
 * Unlink file from disk.
 */
void
file_info_unlink(fileinfo_t *fi)
{
	file_info_check(fi);

	/*
	 * If this fileinfo was partially shared, make sure all uploads currently
	 * requesting it are terminated.
	 */

	file_info_upload_stop(fi, "Partial file removed");

	if (fi->flags & (FI_F_TRANSIENT|FI_F_SEEDING|FI_F_STRIPPED|FI_F_UNLINKED))
		return;

	/*
	 * Only try to unlink partials because completed files are
	 * already moved or renamed and this could in theory match
	 * the filename of another download started afterwards which 
	 * means the wrong file would be removed.
	 */
	if (FILE_INFO_COMPLETE(fi))
		return;

	if (-1 == unlink(fi->pathname)) {
		/*
		 * File might not exist on disk yet if nothing was downloaded.
		 */

		if (fi->done)
			g_warning("cannot unlink \"%s\": %s",
				fi->pathname, g_strerror(errno));
	} else {
		g_warning("unlinked \"%s\" (%s/%s bytes or %u%% done, %s SHA1%s%s)",
			fi->pathname,
			uint64_to_string(fi->done), uint64_to_string2(fi->size),
			(unsigned) (fi->done * 100U / (fi->size == 0 ? 1 : fi->size)),
			fi->sha1 ? "with" : "no",
			fi->sha1 ? ": " : "",
			fi->sha1 ? sha1_base32(fi->sha1) : "");
	}
	fi->flags |= FI_F_UNLINKED;
}

/**
 * Reparent all downloads using `from' as a fileinfo, so they use `to' now.
 */
static void
file_info_reparent_all(fileinfo_t *from, fileinfo_t *to)
{
	file_info_check(from);
	file_info_check(to);
	g_assert(0 == from->done);
	g_assert(0 != strcmp(from->pathname, to->pathname));

	file_info_unlink(from);
	download_info_change_all(from, to);

	/*
	 * We can dispose of the old `from' as all downloads using it are now gone.
	 */

	g_assert(0 == from->refcount);
	g_assert(0 == from->lifecount);

	file_info_hash_remove(from);
	fi_free(from);
}

/**
 * Called when we discover the SHA1 of a running download.
 * Make sure there is no other entry already bearing that SHA1, and record
 * the information.
 *
 * @returns TRUE if OK, FALSE if a duplicate record with the same SHA1 exists.
 */
gboolean
file_info_got_sha1(fileinfo_t *fi, const struct sha1 *sha1)
{
	fileinfo_t *xfi;

	file_info_check(fi);
	g_assert(sha1);
	g_assert(NULL == fi->sha1);

	xfi = g_hash_table_lookup(fi_by_sha1, sha1);

	if (NULL == xfi) {
		fi->sha1 = atom_sha1_get(sha1);
		gm_hash_table_insert_const(fi_by_sha1, fi->sha1, fi);
		return TRUE;
	}

	/*
	 * Found another entry with the same SHA1.
	 *
	 * If either download has not started yet, we can keep the active one
	 * and reparent the other.  Otherwise, we have to abort the current
	 * download, which will be done when we return FALSE.
	 *
	 * XXX we could abort the download with less data downloaded already,
	 * XXX or we could reconciliate the chunks from both files, but this
	 * XXX will cost I/Os and cannot be done easily in our current
	 * XXX mono-threaded model.
	 * XXX		--RAM, 05/09/2002
	 */

	if (GNET_PROPERTY(fileinfo_debug) > 3) {
		char buf[64];

		concat_strings(buf, sizeof buf,
			uint64_to_string(xfi->done), "/",
			uint64_to_string2(xfi->size), (void *) 0);
		g_message("CONFLICT found same SHA1 %s in \"%s\" "
			"(%s bytes done) and \"%s\" (%s/%s bytes done)\n",
			sha1_base32(sha1), xfi->pathname, buf, fi->pathname,
			uint64_to_string(fi->done), uint64_to_string2(fi->size));
	}

	if (fi->done && xfi->done) {
		char buf[64];

		concat_strings(buf, sizeof buf,
			uint64_to_string(xfi->done), "/",
			uint64_to_string2(xfi->size), (void *) 0);
		g_warning("found same SHA1 %s in \"%s\" (%s bytes done) and \"%s\" "
			"(%s/%s bytes done) -- aborting last one",
			sha1_base32(sha1), xfi->pathname, buf, fi->pathname,
			uint64_to_string(fi->done), uint64_to_string2(fi->size));
		return FALSE;
	}

	if (fi->done) {
		g_assert(0 == xfi->done);
		fi->sha1 = atom_sha1_get(sha1);
		file_info_reparent_all(xfi, fi);	/* All `xfi' replaced by `fi' */
		gm_hash_table_insert_const(fi_by_sha1, fi->sha1, fi);
	} else {
		g_assert(0 == fi->done);
		file_info_reparent_all(fi, xfi);	/* All `fi' replaced by `xfi' */
	}

	return TRUE;
}

/**
 * Extract GUID from GUID line in the ASCII "fileinfo" summary file
 * and return NULL if none or invalid, the GUID atom otherwise.
 */
static const struct guid *
extract_guid(const char *s)
{
	struct guid guid;

	if (strlen(s) < GUID_HEX_SIZE)
		return NULL;

	if (!hex_to_guid(s, &guid))
		return NULL;

	return atom_guid_get(&guid);
}

/**
 * Extract sha1 from SHA1/CHA1 line in the ASCII "fileinfo" summary file
 * and return NULL if none or invalid, the SHA1 atom otherwise.
 */
static const struct sha1 *
extract_sha1(const char *s)
{
	struct sha1 sha1;

	if (strlen(s) < SHA1_BASE32_SIZE)
		return NULL;

	if (SHA1_RAW_SIZE != base32_decode(sha1.data, sizeof sha1.data,
							s, SHA1_BASE32_SIZE))
		return NULL;

	return atom_sha1_get(&sha1);
}

static const struct tth *
extract_tth(const char *s)
{
	struct tth tth;

	if (strlen(s) < TTH_BASE32_SIZE)
		return NULL;

	if (TTH_RAW_SIZE != base32_decode(tth.data, sizeof tth.data,
							s, TTH_BASE32_SIZE))
		return NULL;

	return atom_tth_get(&tth);
}

typedef enum {
	FI_TAG_UNKNOWN = 0,
	FI_TAG_ALIA,
	FI_TAG_CHA1,
	FI_TAG_CHNK,
	FI_TAG_CTIM,
	FI_TAG_DONE,
	FI_TAG_FSKN,
	FI_TAG_GENR,
	FI_TAG_GUID,
	FI_TAG_NAME,
	FI_TAG_NTIM,
	FI_TAG_PATH,
	FI_TAG_PAUS,
	FI_TAG_SHA1,
	FI_TAG_SIZE,
	FI_TAG_SWRM,
	FI_TAG_TIME,
	FI_TAG_TTH,

	NUM_FI_TAGS
} fi_tag_t;

static const struct fi_tag {
	fi_tag_t	tag;
	const char *str;
} fi_tag_map[] = {
	/* Must be sorted alphabetically for dichotomic search */

	{ FI_TAG_ALIA,	"ALIA" },
	{ FI_TAG_CHA1,	"CHA1" },
	{ FI_TAG_CHNK,	"CHNK" },
	{ FI_TAG_CTIM,	"CTIM" },
	{ FI_TAG_DONE,	"DONE" },
	{ FI_TAG_FSKN,	"FSKN" },
	{ FI_TAG_GENR,	"GENR" },
	{ FI_TAG_GUID,	"GUID" },
	{ FI_TAG_NAME,	"NAME" },
	{ FI_TAG_NTIM,	"NTIM" },
	{ FI_TAG_PATH,	"PATH" },
	{ FI_TAG_PAUS,	"PAUS" },
	{ FI_TAG_SHA1, 	"SHA1" },
	{ FI_TAG_SIZE, 	"SIZE" },
	{ FI_TAG_SWRM, 	"SWRM" },
	{ FI_TAG_TIME, 	"TIME" },
	{ FI_TAG_TTH, 	"TTH" },

	/* Above line intentionally left blank (for "!}sort" on vi) */
};

/**
 * Transform fileinfo tag string into tag constant.
 * For instance, "TIME" would yield FI_TAG_TIME.
 * An unknown tag yieldd FI_TAG_UNKNOWN.
 */
static fi_tag_t
file_info_string_to_tag(const char *s)
{
	STATIC_ASSERT(G_N_ELEMENTS(fi_tag_map) == (NUM_FI_TAGS - 1));

#define GET_KEY(i) (fi_tag_map[(i)].str)
#define FOUND(i) G_STMT_START { \
	return fi_tag_map[(i)].tag; \
	/* NOTREACHED */ \
} G_STMT_END

	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(const char *, s, G_N_ELEMENTS(fi_tag_map), strcmp,
		GET_KEY, FOUND);

#undef FOUND
#undef GET_KEY
	return FI_TAG_UNKNOWN;
}

/**
 * Reset CHUNK info: everything will have to be downloaded again
 */
static void
fi_reset_chunks(fileinfo_t *fi)
{
	file_info_check(fi);
	if (fi->file_size_known) {
		struct dl_file_chunk *fc;

		fc = dl_file_chunk_alloc();
		fc->from = 0;
		fc->to = fi->size;
		fc->status = DL_CHUNK_EMPTY;
		fi->chunklist = g_slist_append(fi->chunklist, fc);
	}

	fi->generation = 0;		/* Restarting from scratch... */
	fi->done = 0;
	atom_sha1_free_null(&fi->cha1);
}

/**
 * Copy CHUNK info from binary trailer `trailer' into `fi'.
 */
static void
fi_copy_chunks(fileinfo_t *fi, fileinfo_t *trailer)
{
	GSList *sl;

	file_info_check(fi);
	file_info_check(trailer);
	g_assert(NULL == fi->chunklist);
	g_assert(file_info_check_chunklist(trailer, TRUE));

	fi->generation = trailer->generation;
	if (trailer->cha1)
		fi->cha1 = atom_sha1_get(trailer->cha1);

	for (sl = trailer->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		g_assert(fc);
		g_assert(fc->from <= fc->to);
		g_assert(sl != trailer->chunklist || 0 == fc->from);
		/* Prepend now and reverse later for better efficiency */
		fi->chunklist = g_slist_prepend(fi->chunklist, wcopy(fc, sizeof *fc));
	}

	fi->chunklist = g_slist_reverse(fi->chunklist);

	file_info_merge_adjacent(fi); /* Recalculates also fi->done */
}

/**
 * Loads the fileinfo database from disk, and saves a copy in fileinfo.orig.
 */
void
file_info_retrieve(void)
{
	FILE *f;
	char line[1024];
	fileinfo_t *fi = NULL;
	gboolean empty = TRUE;
	gboolean last_was_truncated = FALSE;
	file_path_t fp;
	const char *old_filename = NULL;	/* In case we must rename the file */
	const char *path = NULL;
	const char *filename = NULL;

	/*
	 * We have a complex interaction here: each time a new entry within the
	 * download mesh is added, file_info_try_to_swarm_with() will be
	 * called.	Moreover, the download mesh is initialized before us.
	 *
	 * However, we cannot enqueue a download before the download module is
	 * initialized. And we know it is initialized now because download_init()
	 * calls us!
	 *
	 *		--RAM, 20/08/2002
	 */

	can_swarm = TRUE;			/* Allows file_info_try_to_swarm_with() */

	file_path_set(&fp, settings_config_dir(), file_info_file);
	f = file_config_open_read("fileinfo database", &fp, 1);
	if (!f)
		return;

	while (fgets(line, sizeof line, f)) {
		size_t len;
		int error;
		gboolean truncated = FALSE, damaged;
		const char *ep;
		char *value;
		guint64 v;

		if ('#' == *line) continue;

		/*
		 * The following semi-complex logic attempts to determine whether
		 * we filled the whole line buffer without reaching the end of the
		 * physical line.
		 *
		 * When truncation occurs, we skip every following "line" we'd get
		 * up to the point where we no longer need to truncate, at which time
		 * we'll be re-synchronized on the real end of the line.
		 */

		len = strlen(line);
		if (sizeof line - 1 == len)
			truncated = '\n' != line[sizeof line - 2];

		if (last_was_truncated) {
			last_was_truncated = truncated;
			g_warning("ignoring fileinfo line after truncation: '%s'", line);
			continue;
		} else if (truncated) {
			last_was_truncated = TRUE;
			g_warning("ignoring too long fileinfo line: '%s'", line);
			continue;
		}

		/*
		 * Remove trailing "\n" from line, then parse it.
		 * Reaching an empty line means the end of the fileinfo description.
		 */

		str_chomp(line, len);

		if ('\0' == *line && fi) {
			fileinfo_t *dfi;
			gboolean upgraded;
			gboolean reload_chunks = FALSE;

			if (filename && path) {
				char *pathname = make_pathname(path, filename);
				fi->pathname = atom_str_get(pathname);
				G_FREE_NULL(pathname);
			} else {
				/* There's an incomplete fileinfo record */
				goto reset;
			}
			atom_str_free_null(&filename);
			atom_str_free_null(&path);

			/*
			 * There can't be duplicates!
			 */

			dfi = g_hash_table_lookup(fi_by_outname, fi->pathname);
			if (NULL != dfi) {
				g_warning("discarding DUPLICATE fileinfo entry for \"%s\"",
					filepath_basename(fi->pathname));
				goto reset;
			}

			if (0 == fi->size) {
				fi->file_size_known = FALSE;
			}

			/*
			 * If we deserialized an older version, bring it up to date.
			 */

			upgraded = fi_upgrade_older_version(fi);

			/*
			 * Allow reconstruction of missing information: if no CHNK
			 * entry was found for the file, fake one, all empty, and reset
			 * DONE and GENR to 0.
			 *
			 * If for instance the partition where temporary files are held
			 * is lost, a single "grep -v ^CHNK fileinfo > fileinfo.new"
			 * will be enough to restart without losing the collected
			 * files.
			 *
			 *		--RAM, 31/12/2003
			 */

			if (NULL == fi->chunklist) {
				if (fi->file_size_known)
					g_warning("no CHNK info for \"%s\"", fi->pathname);
				fi_reset_chunks(fi);
				reload_chunks = TRUE;	/* Will try to grab from trailer */
			} else if (!file_info_check_chunklist(fi, FALSE)) {
				if (fi->file_size_known)
					g_warning("invalid set of CHNK info for \"%s\"",
						fi->pathname);
				fi_reset_chunks(fi);
				reload_chunks = TRUE;	/* Will try to grab from trailer */
			}

			g_assert(file_info_check_chunklist(fi, TRUE));

			file_info_merge_adjacent(fi); /* Recalculates also fi->done */

			/*
			 * If `old_filename' is not NULL, then we need to rename
			 * the file bearing that name into the new (sanitized)
			 * name, making sure there is no filename conflict.
			 */

			if (NULL != old_filename) {
				const char *new_pathname;
				char *old_path;
				gboolean renamed = TRUE;

				old_path = filepath_directory(fi->pathname);
				new_pathname = file_info_new_outname(old_path,
									filepath_basename(fi->pathname));
				G_FREE_NULL(old_path);
				if (NULL == new_pathname)
					goto reset;

				/*
				 * If fi->done == 0, the file might not exist on disk.
				 */

				if (-1 == rename(fi->pathname, new_pathname) && 0 != fi->done)
					renamed = FALSE;

				if (renamed) {
					g_warning("renamed \"%s\" into sanitized \"%s\"",
						fi->pathname, new_pathname);
					atom_str_change(&fi->pathname, new_pathname);
				} else {
					g_warning("cannot rename \"%s\" into \"%s\": %s",
						fi->pathname, new_pathname, g_strerror(errno));
				}
				atom_str_free_null(&new_pathname);
			}

			/*
			 * Check file trailer information.	The main file is only written
			 * infrequently and the file's trailer can have more up-to-date
			 * information.
			 */

			dfi = file_info_retrieve_binary(fi->pathname);

			/*
			 * If we resetted the CHNK list above, grab those from the
			 * trailer: that cannot be worse than having to download
			 * everything again...  If there was no valid trailer, all the
			 * data are lost and the whole file will need to be grabbed again.
			 */

			if (dfi != NULL && reload_chunks) {
				fi_copy_chunks(fi, dfi);
				if (fi->chunklist) g_message(
					"recovered %lu downloaded bytes from trailer of \"%s\"",
						(gulong) fi->done, fi->pathname);
			} else if (reload_chunks)
				g_warning("lost all CHNK info for \"%s\" -- downloading again",
					fi->pathname);

			g_assert(file_info_check_chunklist(fi, TRUE));

			/*
			 * Special treatment for the GUID: if not present, it will be
			 * added during retrieval, but it will be different for the
			 * one in the fileinfo DB and the one on disk.  Set `upgraded'
			 * to signal that, so that we resync the metainfo below.
			 */

			if (dfi && dfi->guid != fi->guid)		/* They're atoms... */
				upgraded = TRUE;

			/*
			 * NOTE: The tigertree data is only stored in the trailer, not
			 * in the common "fileinfo" file. Therefore, it MUST be fetched
			 * from "dfi".
			 */
			if (dfi && dfi->tigertree.leaves && NULL == fi->tigertree.leaves) {
				file_info_got_tigertree(fi,
					dfi->tigertree.leaves, dfi->tigertree.num_leaves);
			}

			if (dfi) {
				fi->modified = dfi->modified;
			}

			if (NULL == dfi) {
				if (is_regular(fi->pathname)) {
					g_warning("got metainfo in fileinfo cache, "
						"but none in \"%s\"", fi->pathname);
					upgraded = FALSE;			/* No need to flush twice */
					file_info_store_binary(fi, TRUE);	/* Create metainfo */
				} else {
					file_info_merge_adjacent(fi);		/* Compute fi->done */
					if (fi->done > 0) {
						g_warning("discarding cached metainfo for \"%s\": "
							"file had %s bytes downloaded "
							"but is now gone!", fi->pathname,
							uint64_to_string(fi->done));
						goto reset;
					}
				}
			} else if (dfi->generation > fi->generation) {
				g_warning("found more recent metainfo in \"%s\"", fi->pathname);
				fi_free(fi);
				fi = dfi;
			} else if (dfi->generation < fi->generation) {
				g_warning("found OUTDATED metainfo in \"%s\"", fi->pathname);
				fi_free(dfi);
				dfi = NULL;
				upgraded = FALSE;				/* No need to flush twice */
				file_info_store_binary(fi, TRUE);/* Resync metainfo */
			} else {
				g_assert(dfi->generation == fi->generation);
				fi_free(dfi);
				dfi = NULL;
			}

			/*
			 * Check whether entry is not another's duplicate.
			 */

			dfi = file_info_lookup_dup(fi);

			if (NULL != dfi) {
				g_warning("found DUPLICATE entry for \"%s\" "
					"(%s bytes) with \"%s\" (%s bytes)",
					fi->pathname, uint64_to_string(fi->size),
					dfi->pathname, uint64_to_string2(dfi->size));
				goto reset;
			}

			/*
			 * If we had to upgrade the fileinfo, make sure we resync
			 * the metadata on disk as well.
			 */

			if (upgraded) {
				g_warning("flushing upgraded metainfo in \"%s\"", fi->pathname);
				file_info_store_binary(fi, TRUE);		/* Resync metainfo */
			}

			file_info_merge_adjacent(fi);
			file_info_hash_insert(fi);

			/*
			 * We could not add the aliases immediately because the file
			 * is formatted with ALIA coming before SIZE.  To let fi_alias()
			 * detect conflicting entries, we need to have a valid fi->size.
			 * And since the `fi' is hashed, we can detect duplicates in
			 * the `aliases' list itself as an added bonus.
			 */

			if (fi->alias) {
				GSList *aliases, *sl;

				/* For efficiency each alias has been prepended to
				 * the list. To preserve the order between sessions,
				 * the original list order is restored here. */
				aliases = g_slist_reverse(fi->alias);
				fi->alias = NULL;
				for (sl = aliases; NULL != sl; sl = g_slist_next(sl)) {
					const char *s = sl->data;
					fi_alias(fi, s, TRUE);
					atom_str_free_null(&s);
				}
				g_slist_free(aliases);
				aliases = NULL;
			}

			empty = FALSE;
			fi = NULL;
			continue;
		}

		if (!fi) {
			fi = walloc0(sizeof *fi);
			fi->magic = FI_MAGIC;
			fi->refcount = 0;
			fi->seen_on_network = NULL;
			fi->file_size_known = TRUE;		/* Unless stated otherwise below */
			old_filename = NULL;
		}

		value = strchr(line, ' ');
		if (!value) {
			if (*line)
				g_warning("ignoring fileinfo line: \"%s\"", line);
			continue;
		}
		*value++ = '\0'; /* Skip space and point to value */
		if ('\0' == value[0]) {
			g_warning("empty value in fileinfo line: \"%s %s\"", line, value);
			continue;
		}

		damaged = FALSE;
		switch (file_info_string_to_tag(line)) {
		case FI_TAG_NAME:
			if (GNET_PROPERTY(convert_old_filenames)) {
				char *s;
				char *b;

				b = s = gm_sanitize_filename(value,
						GNET_PROPERTY(convert_spaces),
						GNET_PROPERTY(convert_evil_chars));

				if (GNET_PROPERTY(beautify_filenames))
					b = gm_beautify_filename(s);

				filename = atom_str_get(b);
				if (s != value) {

					if (0 != strcmp(s, value)) {
						g_warning("fileinfo database contained an "
						"unsanitized filename: \"%s\" -> \"%s\"", value, s);

						/*
						 * Record old filename, before sanitization.
						 * We'll have to rename that file later, when we
						 * have parsed the whole fileinfo.
						 */

						old_filename = atom_str_get(value);
					}
				}

				if (b != s)		G_FREE_NULL(b);
				if (value != s) G_FREE_NULL(s);
			} else {
				filename = atom_str_get(value);
			}
			break;
		case FI_TAG_PATH:
			/* FIXME: Check the pathname more thoroughly */
			damaged = !is_absolute_path(value);
			path = damaged ? NULL : atom_str_get(value);
			break;
		case FI_TAG_PAUS:
			v = parse_uint32(value, &ep, 10, &error);
			damaged = error || '\0' != *ep || v > 1;
			fi->flags |= v ? FI_F_PAUSED : 0;
			break;
		case FI_TAG_ALIA:
			if (looks_like_urn(value)) {
				g_warning("skipping alias which looks like a urn in "
					"fileinfo database: \"%s\" (pathname=\"%s\")", value,
					NULL_STRING(fi->pathname));
			} else {
				char *s;
				char *b;

				b = s = gm_sanitize_filename(value, FALSE, FALSE);

				if (GNET_PROPERTY(beautify_filenames))
					b = gm_beautify_filename(s);

				/* The alias is only temporarily added to fi->alias, the list
				 * of aliases has to be re-constructed with fi_alias()
			   	 * when the fileinfo record is finished. It's merely done
				 * this way to simplify discarding incomplete/invalid records
				 * utilizing fi_free().
				 * The list should be reversed once it's complete.
				 */
				fi->alias = g_slist_prepend(fi->alias,
								deconstify_gchar(atom_str_get(b)));
				if (s != value) {
					if (strcmp(s, value)) {
						g_warning("fileinfo database contained an "
							"unsanitized alias: \"%s\" -> \"%s\"", value, s);
					}
				}
				if (b != s)		G_FREE_NULL(b);
				if (s != value)	G_FREE_NULL(s);
			}
			break;
		case FI_TAG_GENR:
			v = parse_uint32(value, &ep, 10, &error);
			damaged = error || '\0' != *ep || v > (guint32) INT_MAX;
			fi->generation = v;
			break;
		case FI_TAG_SIZE:
			v = parse_uint64(value, &ep, 10, &error);
			damaged = error
				|| '\0' != *ep
				|| v >= ((guint64) 1UL << 63)
				|| (!fi->file_size_known && 0 == v);
			fi->size = v;
			break;
		case FI_TAG_FSKN:
			v = parse_uint32(value, &ep, 10, &error);
			damaged = error
				|| '\0' != *ep
				|| v > 1
				|| (0 == fi->size && 0 != v);
			fi->file_size_known = v != 0;
			break;
		case FI_TAG_TIME:
			v = parse_uint64(value, &ep, 10, &error);
			damaged = error || '\0' != *ep;
			fi->stamp = v;
			break;
		case FI_TAG_CTIM:
			v = parse_uint64(value, &ep, 10, &error);
			damaged = error || '\0' != *ep;
			fi->created = v;
			break;
		case FI_TAG_NTIM:
			v = parse_uint64(value, &ep, 10, &error);
			damaged = error || '\0' != *ep;
			fi->ntime = v;
			break;
		case FI_TAG_DONE:
			v = parse_uint64(value, &ep, 10, &error);
			damaged = error || '\0' != *ep || v >= ((guint64) 1UL << 63);
			fi->done = v;
			break;
		case FI_TAG_SWRM:
			v = parse_uint32(value, &ep, 10, &error);
			damaged = error || '\0' != *ep || v > 1;
			fi->use_swarming = v;
			break;
		case FI_TAG_GUID:
			fi->guid = extract_guid(value);
			damaged = NULL == fi->guid;
			break;
		case FI_TAG_TTH:
			fi->tth = extract_tth(value);
			damaged = NULL == fi->tth;
			break;
		case FI_TAG_SHA1:
			fi->sha1 = extract_sha1(value);
			damaged = NULL == fi->sha1;
			break;
		case FI_TAG_CHA1:
			fi->cha1 = extract_sha1(value);
			damaged = NULL == fi->cha1;
			break;
		case FI_TAG_CHNK:
			{
				filesize_t from, to;
				guint32 status;

				from = v = parse_uint64(value, &ep, 10, &error);
				damaged = error
					|| *ep != ' '
					|| v >= ((guint64) 1UL << 63)
					|| from > fi->size;

				if (!damaged) {
					const char *s = &ep[1];

					to = v = parse_uint64(s, &ep, 10, &error);
					damaged = error
						|| ' ' != *ep
						|| v >= ((guint64) 1UL << 63)
						|| v <= from
						|| to > fi->size;
				} else {
					to = 0;	/* For stupid compilers */
				}
				if (!damaged) {
					const char *s = &ep[1];

					status = v = parse_uint64(s, &ep, 10, &error);
					damaged = error || '\0' != *ep || v > 2U;
				} else {
					status = 0;	/* For stupid compilers */
				}
				if (!damaged) {
					struct dl_file_chunk *fc, *prev;

					fc = dl_file_chunk_alloc();
					fc->from = from;
					fc->to = to;
					if (DL_CHUNK_BUSY == status)
						status = DL_CHUNK_EMPTY;
					fc->status = status;
					prev = fi->chunklist
						? g_slist_last(fi->chunklist)->data : NULL;
					if (fc->from != (prev ? prev->to : 0)) {
						g_warning("Chunklist is inconsistent (fi->size=%s)",
							uint64_to_string(fi->size));
						damaged = TRUE;
					} else {
						fi->chunklist = g_slist_append(fi->chunklist, fc);
					}
				}
			}
			break;
		case FI_TAG_UNKNOWN:
			if (*line)
				g_warning("ignoring fileinfo line: \"%s %s\"", line, value);
			break;
		case NUM_FI_TAGS:
			g_assert_not_reached();
		}

		if (damaged)
			g_warning("damaged entry in fileinfo line: \"%s %s\"", line, value);
		continue;

	reset:
		if (NULL == fi->pathname) {
			fi->pathname = atom_str_get("/non-existent");
		}
		fi_free(fi);
		fi = NULL;
		atom_str_free_null(&filename);
		atom_str_free_null(&path);
	}

	if (fi) {
		if (NULL == fi->pathname) {
			fi->pathname = atom_str_get("/non-existent");
		}
		fi_free(fi);
		fi = NULL;
		if (!empty)
			g_warning("file info repository was truncated!");
	}
	atom_str_free_null(&filename);
	atom_str_free_null(&path);

	fclose(f);
}

static gboolean
file_info_name_is_uniq(const char *pathname)
{
	return NULL == g_hash_table_lookup(fi_by_outname, pathname) &&
	   	file_does_not_exist(pathname);
}

char *
file_info_unique_filename(const char *path, const char *file,
	const char *ext)
{
	return unique_filename(path, file, ext, file_info_name_is_uniq);
}
	
/**
 * Allocate unique output name for file `name', stored in `dir'.
 *
 * @returns The full pathname (string atom).
 */
static const char *
file_info_new_outname(const char *dir, const char *name)
{
	char *uniq = NULL;
	const char *filename = name;
	char *b;
	char *s;

	g_return_val_if_fail(dir, NULL);
	g_return_val_if_fail(name, NULL);
	g_return_val_if_fail(is_absolute_path(dir), NULL);

	b = s = gm_sanitize_filename(name,
			GNET_PROPERTY(convert_spaces),
			GNET_PROPERTY(convert_evil_chars));

	if (name != s)
		filename = s;

	if (GNET_PROPERTY(beautify_filenames))
		filename = b = gm_beautify_filename(s);

	if ('\0' == filename[0]) {
		/* Don't allow empty names */
		filename = "noname";
	}

	/*
	 * If `filename' (sanitized form) is not taken yet, it will do.
	 */

	uniq = file_info_unique_filename(dir, filename, "");
	if (b != s)		G_FREE_NULL(b);
	if (name != s)	G_FREE_NULL(s);

	if (uniq) {
		const char *pathname;

		pathname = atom_str_get(uniq);
		G_FREE_NULL(uniq);
		g_assert(NULL == g_hash_table_lookup(fi_by_outname, pathname));
		return pathname;
	} else {
		return NULL;
	}
}

/**
 * Create a fileinfo structure from existing file with no swarming trailer.
 * The given `size' argument reflect the final size of the (complete) file.
 * The `sha1' is the known SHA1 for the file (NULL if unknown).
 */
static fileinfo_t *
file_info_create(const char *file, const char *path, filesize_t size,
	const struct sha1 *sha1, gboolean file_size_known)
{
	const char *pathname;
	fileinfo_t *fi;
	struct stat st;

	pathname = file_info_new_outname(path, file);
	g_return_val_if_fail(pathname, NULL);

	fi = walloc0(sizeof *fi);
	fi->magic = FI_MAGIC;

	/* Get unique file name */
	fi->pathname = pathname;

	/* Get unique ID */
	fi->guid = fi_random_guid_atom();

	if (sha1)
		fi->sha1 = atom_sha1_get(sha1);
	fi->size = 0;	/* Will be updated by fi_resize() */
	fi->file_size_known = file_size_known;
	fi->done = 0;
	fi->use_swarming = GNET_PROPERTY(use_swarming) && file_size_known;
	fi->created = tm_time();
	fi->modified = fi->created;
	fi->seen_on_network = NULL;

	if (-1 != stat(fi->pathname, &st) && S_ISREG(st.st_mode)) {
		struct dl_file_chunk *fc;

		g_warning("file_info_create(): "
			"assuming file \"%s\" is complete up to %s bytes",
			fi->pathname, uint64_to_string(st.st_size));

		fc = dl_file_chunk_alloc();
		fc->from = 0;
		fi->size = fc->to = st.st_size;
		fc->status = DL_CHUNK_DONE;
		fi->modified = st.st_mtime;
		fi->chunklist = g_slist_append(fi->chunklist, fc);
		fi->dirty = TRUE;
	}

	if (size > fi->size)
		fi_resize(fi, size);

	g_assert(fi->file_size_known || !fi->use_swarming);

	return fi;
}

/**
 * Create a transient fileinfo structure.
 */
fileinfo_t *
file_info_get_transient(const char *name)
{
	fileinfo_t *fi;
	char *path;

	fi = walloc0(sizeof *fi);
	fi->magic = FI_MAGIC;

	path = make_pathname("/non-existent", name);
	fi->pathname = atom_str_get(path);
	G_FREE_NULL(path);

	/* Get unique ID */
	fi->guid = fi_random_guid_atom();

	fi->size = 0;	/* Will be updated by fi_resize() */
	fi->file_size_known = FALSE;
	fi->done = 0;
	fi->use_swarming = FALSE;
	fi->created = tm_time();
	fi->modified = fi->created;
	fi->seen_on_network = NULL;
	fi->dirty = TRUE;

	fi->flags = FI_F_TRANSIENT;		/* Not persisted to disk */

	file_info_hash_insert(fi);

	return fi;
}

/**
 * Rename dead file we cannot use, either because it bears a duplicate SHA1
 * or because its file trailer bears a duplicate file ID.
 *
 * The file is really dead, so unfortunately we have to strip its fileinfo
 * trailer so that we do not try to reparent it at a later time.
 */
static void
fi_rename_dead(fileinfo_t *fi, const char *pathname)
{
	char *path, *dead;

	file_info_check(fi);

	path = filepath_directory(pathname);
	dead = file_info_unique_filename(path,
				filepath_basename(pathname), ".DEAD");

	if (dead && 0 == rename(pathname, dead)) {
		file_info_strip_trailer(fi, dead);
	} else {
		g_warning("cannot rename \"%s\" as \"%s\": %s",
			pathname, NULL_STRING(dead), g_strerror(errno));
	}
	G_FREE_NULL(dead);
	G_FREE_NULL(path);
}

/**
 * Called to update the fileinfo information with the new path and possibly
 * filename information, once the downloaded file has been moved/renamed.
 * This prepares for possible seeding of the file once it has been completed,
 * to continue "partial-file-sharing" it now that it is fully available...
 */
void
file_info_moved(fileinfo_t *fi, const char *pathname)
{
	const fileinfo_t *xfi;
	
	file_info_check(fi);
	g_assert(pathname);
	g_assert(is_absolute_path(pathname));
	g_assert(!(fi->flags & FI_F_SEEDING));

	if (!fi->hashed)
		return;

	xfi = g_hash_table_lookup(fi_by_outname, fi->pathname);
	if (xfi) {
		file_info_check(xfi);
		g_assert(xfi == fi);
		g_hash_table_remove(fi_by_outname, fi->pathname);
	}
	file_object_revoke(fi->pathname);

	atom_str_change(&fi->pathname, pathname);

	g_assert(NULL == g_hash_table_lookup(fi_by_outname, fi->pathname));
	gm_hash_table_insert_const(fi_by_outname, fi->pathname, fi);

	if (fi->sf) {
		struct stat sb;
	   
		shared_file_set_path(fi->sf, fi->pathname);
		if (
			stat(fi->pathname, &sb) ||
			fi->size + (off_t)0 != sb.st_size + (filesize_t)0
		) {
			sb.st_mtime = 0;
		}
		shared_file_set_modification_time(fi->sf, sb.st_mtime);
	}
	fi_event_trigger(fi, EV_FI_INFO_CHANGED);
	file_info_changed(fi);
	fileinfo_dirty = TRUE;
}

/**
 * @param `file' is the file name on the server.
 * @param `path' no brief description.
 * @param `size' no brief description.
 * @param `sha1' no brief description.
 * @param `file_size_known' no brief description.
 *
 * @returns a pointer to file_info struct that matches the given file
 * name, size and/or SHA1. A new struct will be allocated if necessary.
 */
fileinfo_t *
file_info_get(const char *file, const char *path, filesize_t size,
	const struct sha1 *sha1, gboolean file_size_known)
{
	fileinfo_t *fi;
	const char *pathname;
   	char *to_free = NULL;

	/*
	 * See if we know anything about the file already.
	 */

	fi = file_info_lookup(file, size, sha1);
	if (fi) {
		file_info_check(fi);
		if (sha1 && fi->sha1 && !sha1_eq(sha1, fi->sha1))
			fi = NULL;
	}


	if (fi) {
		/*
		 * Once we have determined the file size with certainety, we do not
		 * allow resizing.  Of course, we can't know which size is the correct
		 * one (the one we had before, or the new reported one).
		 */

		if (size != fi->size) {
			if (fi->file_size_known) {
				g_warning("file \"%s\" (SHA1 %s, %s bytes): "
					"size mismatch: %s bytes",
					fi->pathname, sha1_base32(fi->sha1),
					filesize_to_string(fi->size),
					filesize_to_string2(size));
				return NULL;
			}
		}

		/*
		 * If download size is greater, we need to resize the output file.
		 * This can only happen for a download with a SHA1, because otherwise
		 * we perform a matching on name AND size.
		 */

		if (size > fi->size) {
			g_assert(fi->sha1);
			g_assert(sha1);

			g_warning("file \"%s\" (SHA1 %s) was %s bytes, resizing to %s",
				fi->pathname, sha1_base32(fi->sha1),
				uint64_to_string(fi->size), uint64_to_string2(size));

			file_info_hash_remove(fi);
			fi_resize(fi, size);
			file_info_hash_insert(fi);
		}

		fi_alias(fi, file, TRUE);	/* Add alias if not conflicting */

		return fi;
	}


	/* First convert the filename to what the GUI used */
	{
		char *s = unknown_to_utf8_normalized(file, UNI_NORM_NETWORK, NULL);
		if (file != s) {
			file = s;
			to_free = s;
		}
	}

	/* Now convert the UTF-8 to what the filesystem wants */
	{
		char *s = utf8_to_filename(file);
		g_assert(s != file);
		G_FREE_NULL(to_free);
		to_free = s;
		file = s;
	}

	/*
	 * Compute new output name.  If the filename is not taken yet, this
	 * will be exactly `file'.  Otherwise, it will be a variant.
	 */

	pathname = file_info_new_outname(path, file);
	if (NULL == pathname)
		goto finish;

	/*
	 * Check whether the file exists and has embedded meta info.
	 * Note that we use the new `outname', not `file'.
	 */

	fi = file_info_retrieve_binary(pathname);
	if (fi) {
		/*
		 * File exists, and is NOT currently in use, otherwise `outname' would
		 * not have been selected as an output name.
		 *
		 * If filename has a SHA1, and either:
		 *
		 * 1. we don't have a SHA1 for the new download (the `sha1' parameter)
		 * 2. we have a SHA1 but it differs
		 *
		 * then the file is "dead": we cannot use it.
		 *
		 * Likewise, if the trailer bears a file ID that conflicts with
		 * one of our currently managed files, we cannot use it.
		 */

		if (NULL != fi->sha1 && (NULL == sha1 || !sha1_eq(sha1, fi->sha1))) {
			g_warning("found DEAD file \"%s\" bearing SHA1 %s",
				pathname, sha1_base32(fi->sha1));

			fi_rename_dead(fi, pathname);
			fi_free(fi);
			fi = NULL;
		} else if (NULL != g_hash_table_lookup(fi_by_guid, fi->guid)) {
			g_warning("found DEAD file \"%s\" with conflicting ID %s",
				pathname, guid_hex_str(fi->guid));

			fi_rename_dead(fi, pathname);
			fi_free(fi);
			fi = NULL;
		} else if (fi->size < size) {
			/*
			 * Existing file is smaller than the total size of this file.
			 * Trust the larger size, because it's the only sane thing to do.
			 * NB: if we have a SHA1, we know it's matching at this point.
			 */

			g_warning("found existing file \"%s\" size=%s, increasing to %s",
				pathname, uint64_to_string(fi->size), uint64_to_string2(size));

			fi_resize(fi, size);
		}
	}

	/*
	 * If we don't have a `fi', then it is a new file.
	 *
	 * Potential problem situations:
	 *
	 *	- File exists, but we have no file_info struct for it.
	 * => Assume the file is complete up to filesize bytes.
	 *
	 *	- File with same name as another, but with a different size.
	 * => We have no way to detect it, sorry.  All new files should have a
	 *    metainfo trailer anyway, so we'll handle it above the next time.
	 */

	if (NULL == fi) {
		fi = file_info_create(filepath_basename(pathname), path,
				size, sha1, file_size_known);

		if (NULL == fi)
			goto finish;

		fi_alias(fi, file, FALSE);
	}

	file_info_hash_insert(fi);

finish:
	atom_str_free_null(&pathname);
	G_FREE_NULL(to_free);

	return fi;
}

/**
 * @returns a pointer to the file info struct if we have a file
 * identical to the given properties in the download queue already,
 * and NULL otherwise.
 */
fileinfo_t *
file_info_has_identical(const struct sha1 *sha1, filesize_t size)
{
	fileinfo_t *fi;

	fi = sha1 ? file_info_by_sha1(sha1) : NULL;
	if (
		fi &&
		(fi->size == size || !fi->file_size_known) &&
		!(fi->flags & (FI_F_TRANSIENT | FI_F_SEEDING | FI_F_STRIPPED))
	) {
		return fi;
	}
	return NULL;
}

/**
 * Set or clear the discard state for a fileinfo.
 */
void
file_info_set_discard(fileinfo_t *fi, gboolean state)
{
	file_info_check(fi);

	if (state)
		fi->flags |= FI_F_DISCARD;
	else
		fi->flags &= ~FI_F_DISCARD;
}

/**
 * Go through the chunk list and merge adjacent chunks that share the
 * same status and download. Keeps the chunk list short and tidy.
 */
void
file_info_merge_adjacent(fileinfo_t *fi)
{
	GSList *fclist;
	gboolean restart;
	filesize_t done;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	do {
		struct dl_file_chunk *fc1, *fc2;

		restart = FALSE;
		done = 0;
		fc2 = NULL;
		for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
			fc1 = fc2;
			fc2 = fclist->data;

			if (fc2->download) {
				download_check(fc2->download);
			}

			if (DL_CHUNK_DONE == fc2->status)
				done += fc2->to - fc2->from;

			if (!fc1 || !fc2)
				continue;

			g_assert(fc1->to == fc2->from);

			if (fc1->status == fc2->status && fc1->download == fc2->download) {
				fc1->to = fc2->to;
				fi->chunklist = g_slist_remove(fi->chunklist, fc2);
				dl_file_chunk_free(&fc2);
				restart = TRUE;
				break;
			}
		}
	} while (restart);

	/*
	 * When file size is unknown, there may be no chunklist.
	 */

	if (fi->chunklist != NULL)
		fi->done = done;

	g_assert(file_info_check_chunklist(fi, TRUE));
}

/**
 * Signals that file size became known suddenly.
 *
 * The download becomes the owner of the "busy" part between what we
 * have done and the end of the file.
 */
void
file_info_size_known(struct download *d, filesize_t size)
{
	fileinfo_t *fi;

	download_check(d);

	fi = d->file_info;
	file_info_check(fi);

	g_assert(!fi->file_size_known);
	g_assert(!fi->use_swarming);
	g_assert(fi->chunklist == NULL);

	/*
	 * Mark everything we have so far as done.
	 */

	if (fi->done) {
		struct dl_file_chunk *fc;

		fc = dl_file_chunk_alloc();
		fc->from = 0;
		fc->to = fi->done;			/* Byte at that offset is excluded */
		fc->status = DL_CHUNK_DONE;

		fi->chunklist = g_slist_prepend(fi->chunklist, fc);
	}

	/*
	 * If the file size is less than the amount we think we have,
	 * then ignore it and mark the whole file as done.
	 */

	if (size > fi->done) {
		struct dl_file_chunk *fc;

		fc = dl_file_chunk_alloc();
		fc->from = fi->done;
		fc->to = size;				/* Byte at that offset is excluded */
		fc->status = DL_CHUNK_BUSY;
		fc->download = d;

		fi->chunklist = g_slist_append(fi->chunklist, fc);
	}

	fi->file_size_known = TRUE;
	fi->use_swarming = TRUE;
	fi->size = size;
	fi->dirty = TRUE;

	if (0 == (FI_F_TRANSIENT & fi->flags)) {
		file_info_hash_insert_name_size(fi);
	}

	g_assert(file_info_check_chunklist(fi, TRUE));

	file_info_changed(fi);
}

/**
 * Marks a chunk of the file with given status.
 * The bytes range from `from' (included) to `to' (excluded).
 *
 * When not marking the chunk as EMPTY, the range is linked to
 * the supplied download `d' so we know who "owns" it currently.
 */
void
file_info_update(struct download *d, filesize_t from, filesize_t to,
		enum dl_chunk_status status)
{
	struct dl_file_chunk *fc, *nfc, *prevfc;
	GSList *fclist;
	fileinfo_t *fi;
	gboolean found = FALSE;
	int n, againcount = 0;
	gboolean need_merging;
	struct download *newval;

	download_check(d);
	fi = d->file_info;
	file_info_check(fi);
	g_assert(fi->refcount > 0);
	g_assert(from < to);

	switch (status) {
	case DL_CHUNK_DONE:
		need_merging = FALSE;
		newval = d;
		goto status_ok;
	case DL_CHUNK_BUSY:
		need_merging = TRUE;
		newval = d;
		g_assert(fi->lifecount > 0);
		goto status_ok;
	case DL_CHUNK_EMPTY:
		need_merging = TRUE;
		newval = NULL;
		goto status_ok;
	}
	g_assert_not_reached();

status_ok:

	/*
	 * If file size is not known yet, the chunk list will be empty.
	 * Simply update the downloaded amount if the chunk is marked as done.
	 */

	if (!fi->file_size_known) {
		g_assert(fi->chunklist == NULL);
		g_assert(!fi->use_swarming);

		if (status == DL_CHUNK_DONE) {
			g_assert(from == fi->done);		/* Downloading continuously */
			fi->done += to - from;
		}

		goto done;
	}

	g_assert(file_info_check_chunklist(fi, TRUE));

	fi->stamp = tm_time();

	if (DL_CHUNK_DONE == status) {
		fi->modified = fi->stamp;
		fi->dirty = TRUE;
	}

again:

	/* I think the algorithm is safe now, but hey... */
	if (++againcount > 10) {
		g_error("Eek! Internal error! "
			"file_info_update(%s, %s, %d) "
			"is looping for \"%s\"! Man battle stations!",
			uint64_to_string(from), uint64_to_string2(to),
			status, d->file_name);
		return;
	}

	/*
	 * Update fi->done, accurately.
	 *
	 * We don't blindly update fi->done with (to - from) when DL_CHUNK_DONE
	 * because we may be writing data to an already "done" chunk, when a
	 * previous chunk bumps into a done one.
	 *		--RAM, 04/11/2002
	 */

	for (
		n = 0, prevfc = NULL, fclist = fi->chunklist;
		fclist;
		n++, prevfc = fc, fclist = g_slist_next(fclist)
	) {
		fc = fclist->data;

		if (fc->to <= from) continue;
		if (fc->from >= to) break;

		if (fc->from == from && fc->to == to) {

			if (prevfc && prevfc->status == status)
				need_merging = TRUE;
			else if (DL_CHUNK_DONE == fc->status)
				need_merging = TRUE;		/* Writing to completed chunk! */

			if (DL_CHUNK_DONE == status)
				fi->done += to - from;
			fc->status = status;
			fc->download = newval;
			found = TRUE;
			g_assert(file_info_check_chunklist(fi, TRUE));
			break;

		} else if (fc->from == from && fc->to < to) {

			if (prevfc && prevfc->status == status)
				need_merging = TRUE;
			else if (DL_CHUNK_DONE == fc->status)
				need_merging = TRUE;		/* Writing to completed chunk! */

			if (DL_CHUNK_DONE == status)
				fi->done += fc->to - from;
			fc->status = status;
			fc->download = newval;
			from = fc->to;
			g_assert(file_info_check_chunklist(fi, TRUE));
			continue;

		} else if ((fc->from == from) && (fc->to > to)) {

			if (DL_CHUNK_DONE == fc->status)
				need_merging = TRUE;		/* Writing to completed chunk! */

			if (DL_CHUNK_DONE == status)
				fi->done += to - from;

			if (
				DL_CHUNK_DONE == status &&
				NULL != prevfc &&
				prevfc->status == status
			) {
				g_assert(prevfc->to == fc->from);
				prevfc->to = to;
				fc->from = to;
				g_assert(file_info_check_chunklist(fi, TRUE));
			} else {
				nfc = dl_file_chunk_alloc();
				nfc->from = to;
				nfc->to = fc->to;
				nfc->status = fc->status;
				nfc->download = fc->download;

				fc->to = to;
				fc->status = status;
				fc->download = newval;
				gm_slist_insert_after(fi->chunklist, fclist, nfc);
				g_assert(file_info_check_chunklist(fi, TRUE));
			}

			found = TRUE;
			break;

		} else if ((fc->from < from) && (fc->to >= to)) {

			/*
			 * New chunk [from, to] lies within ]fc->from, fc->to].
			 */

			if (DL_CHUNK_DONE == fc->status)
				need_merging = TRUE;

			if (DL_CHUNK_DONE == status)
				fi->done += to - from;

			if (fc->to > to) {
				nfc = dl_file_chunk_alloc();
				nfc->from = to;
				nfc->to = fc->to;
				nfc->status = fc->status;
				nfc->download = fc->download;
				gm_slist_insert_after(fi->chunklist, fclist, nfc);
			}

			nfc = dl_file_chunk_alloc();
			nfc->from = from;
			nfc->to = to;
			nfc->status = status;
			nfc->download = newval;
			gm_slist_insert_after(fi->chunklist, fclist, nfc);

			fc->to = from;

			found = TRUE;
			g_assert(file_info_check_chunklist(fi, TRUE));
			break;

		} else if ((fc->from < from) && (fc->to < to)) {

			filesize_t tmp;

			if (DL_CHUNK_DONE == fc->status)
				need_merging = TRUE;

			if (DL_CHUNK_DONE == status)
				fi->done += fc->to - from;

			nfc = dl_file_chunk_alloc();
			nfc->from = from;
			nfc->to = fc->to;
			nfc->status = status;
			nfc->download = newval;
			gm_slist_insert_after(fi->chunklist, fclist, nfc);

			tmp = fc->to;
			fc->to = from;
			from = tmp;
			g_assert(file_info_check_chunklist(fi, TRUE));
			goto again;
		}
	}

	if (!found) {
		/* Should never happen. */
		g_warning("file_info_update(): "
			"(%s) Didn't find matching chunk for <%s-%s> (%u)",
			fi->pathname, uint64_to_string(from),
			uint64_to_string2(to), status);

		for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
			fc = fclist->data;
			g_warning("... %s %s %u", uint64_to_string(fc->from),
				uint64_to_string2(fc->to), fc->status);
		}
	}

	if (need_merging)
		file_info_merge_adjacent(fi);		/* Also updates fi->done */

	g_assert(file_info_check_chunklist(fi, TRUE));

	/*
	 * When status is DL_CHUNK_DONE, we're coming from an "active" download,
	 * i.e. we are writing to it, therefore we can reuse its file descriptor.
	 */

	if (fi->flags & FI_F_TRANSIENT)
		goto done;

	if (fi->dirty) {
		file_info_store_binary(d->file_info, FALSE);
	}

done:
	file_info_changed(fi);
}

/**
 * Go through all chunks that belong to the download,
 * and unmark them as busy.
 *
 * If `lifecount' is TRUE, the download is still counted as being "alive",
 * and this is only used for assertions.
 */
void
file_info_clear_download(struct download *d, gboolean lifecount)
{
	GSList *fclist;
	fileinfo_t *fi;
	int busy;			/**< For assertions only */

	download_check(d);
	fi = d->file_info;
	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	for (fclist = fi->chunklist, busy = 0; fclist; fclist = fclist->next) {
		struct dl_file_chunk *fc = fclist->data;

		dl_file_chunk_check(fc);

		if (DL_CHUNK_BUSY == fc->status)
			busy++;
		if (fc->download == d) {
		    fc->download = NULL;
		    if (DL_CHUNK_BUSY == fc->status)
				fc->status = DL_CHUNK_EMPTY;
		}
	}
	file_info_merge_adjacent(fi);

	g_assert(fi->lifecount >= (lifecount ? busy : (busy - 1)));

	/*
	 * No need to flush data to disk, those are transient
	 * changes. However, we do need to trigger a status change,
	 * because other parts of gtkg, i.e. the visual progress view,
	 * needs to know about them.
	 */
    fi_event_trigger(fi, EV_FI_STATUS_CHANGED_TRANSIENT);
}

/**
 * Reset all chunks to EMPTY, clear computed SHA1 if any.
 */
void
file_info_reset(fileinfo_t *fi)
{
	GSList *list;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	atom_sha1_free_null(&fi->cha1);

	/* File possibly shared */
	file_info_upload_stop(fi, "File info being reset");

	fi->flags &= ~(FI_F_STRIPPED | FI_F_UNLINKED);

restart:
	for (list = fi->chunklist; list; list = g_slist_next(list)) {
		struct dl_file_chunk *fc = list->data;
		struct download *d;

		dl_file_chunk_check(fc);
		d = fc->download;
		if (d) {
			download_check(d);

			if (DOWNLOAD_IS_RUNNING(d)) {
				download_queue(d, "Requeued due to file removal");
				goto restart;	/* Because file_info_clear_download() called */
			}
		}
	}

	for (list = fi->chunklist; list; list = g_slist_next(list)) {
		struct dl_file_chunk *fc = list->data;
		dl_file_chunk_check(fc);
		fc->status = DL_CHUNK_EMPTY;
	}

	file_info_merge_adjacent(fi);
	fileinfo_dirty = TRUE;
}

/**
 * @returns DONE if the range requested is marked as complete,
 * or BUSY if not. Used to determine if we can do overlap
 * checking.
 */
enum dl_chunk_status
file_info_chunk_status(fileinfo_t *fi, filesize_t from, filesize_t to)
{
	const GSList *fclist;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;

		dl_file_chunk_check(fc);

		if (from >= fc->from && to <= fc->to)
			return fc->status;
	}

	/*
	 * Ending up here will normally mean that the tested range falls over
	 * multiple chunks in the list. In that case, chances are that it's
	 * not complete, and that's our assumption...
	 */

	return DL_CHUNK_BUSY;
}

/**
 * @returns the status (EMPTY, BUSY or DONE) of the byte requested.
 * Used to detect if a download is crashing with another.
 */
enum dl_chunk_status
file_info_pos_status(fileinfo_t *fi, filesize_t pos /* XXX,
	filesize_t *start, filesize_t *end */)
{
	const GSList *fclist;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;

		dl_file_chunk_check(fc);
		if (pos >= fc->from && pos < fc->to) {
#if 0
			if (start)
				*start = fc->from;
			if (end)
				*end = fc->to;
#endif
			return fc->status;
		}
	}

	if (pos > fi->size)
		g_warning("file_info_pos_status(): unreachable position %s "
			"in %s-byte file \"%s\"", uint64_to_string(pos),
			uint64_to_string2(fi->size), fi->pathname);

#if 0
	if (start)
		*start = 0;
	if (end)
		*end = 0;
#endif

	return DL_CHUNK_DONE;
}

/**
 * This routine is called each time we start a new download, before
 * making the request to the remote server. If we detect that the
 * file is "gone", then it means the user manually deleted the file.
 * In that case, we need to reset all the chunks and mark the whole
 * thing as being EMPTY.
 * 		--RAM, 21/08/2002.
 */
static void
fi_check_file(fileinfo_t *fi)
{
	struct stat buf;

	file_info_check(fi);
	g_assert(fi->done);			/* Or file will not exist */

	/*
	 * File should exist since fi->done > 0, and it was not completed.
	 */

	if (stat(fi->pathname, &buf) && ENOENT == errno) {
		g_warning("file %s removed, resetting swarming", fi->pathname);
		file_info_reset(fi);
	}
}

/**
 * Count the amount of BUSY chunks attached to a given download.
 */
static int
fi_busy_count(fileinfo_t *fi, struct download *d)
{
	const GSList *sl;
	int count = 0;

	download_check(d);
	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	for (sl = fi->chunklist; sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);
		if (fc->download) {
			download_check(d);
			if (fc->download == d && DL_CHUNK_BUSY == fc->status)
				count++;
		}
	}

	g_assert(fi->lifecount >= count);

	return count;
}

/**
 * Clone fileinfo's chunk list, shifting the origin of the list to a randomly
 * selected offset within the file.
 *
 * If the first chunk is not completed or not at least "pfsp_first_chunk" bytes
 * long, the original list is returned.
 *
 * We also strive to get the latest "pfsp_first_chunk" bytes of the file as
 * well, since some file formats store important information at the tail of
 * the file as well, so we put the latest chunks at the head of the list.
 */
static GSList *
list_clone_shift(fileinfo_t *fi)
{
	filesize_t offset = 0;
	GSList *clone;
	GSList *sl;
	GSList *tail;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	if (GNET_PROPERTY(pfsp_first_chunk) > 0) {
		const struct dl_file_chunk *fc;

		/*
		 * Check whether first chunk is at least "pfsp_first_chunk" bytes
		 * long.  If not, return original chunk list so that we select
		 * the first chunk (if available remotely, naturally, but that will
		 * be checked later by our caller).
		 */
		
		fc = fi->chunklist->data;		/* First chunk */
		dl_file_chunk_check(fc);

		if (
			DL_CHUNK_DONE != fc->status ||
			fc->to < GNET_PROPERTY(pfsp_first_chunk)
		)
			return fi->chunklist;
	}

	if (GNET_PROPERTY(pfsp_last_chunk) > 0) {
		const struct dl_file_chunk *fc;
		const GSList *iter;
		filesize_t last_chunk_offset;

		/*
		 * Scan for the first gap within the last "pfsp_last_chunk" bytes
		 * and set "offset" to the start of it, to download the trailing chunk
		 * if available.
		 */

		last_chunk_offset = fi->size > GNET_PROPERTY(pfsp_last_chunk)
			? fi->size - GNET_PROPERTY(pfsp_last_chunk)
			: 0;

		for (iter = fi->chunklist; NULL != iter; iter = g_slist_next(iter)) {
			fc = iter->data;
			dl_file_chunk_check(fc);

			if (DL_CHUNK_DONE == fc->status)
				continue;

			if (fc->from < last_chunk_offset && fc->to <= last_chunk_offset)
				continue;

			offset = fc->from < last_chunk_offset
				? last_chunk_offset
				: fc->from;
			break;
		}
	}

	/*
	 * Only choose a random offset if the default value of "0" was not
	 * forced to something else above.
	 */

	if (0 == offset) {
		offset = get_random_file_offset(fi->size);

		/*
		 * Aligning blocks is just a convenience here, to make it easier later to
		 * validate the file against the TTH, and also because it is likely to be
		 * slightly more efficient when doing aligned disk I/Os.
		 */
		offset &= ~((filesize_t)128 * 1024 - 1);
	}

	/*
	 * First pass: clone the list starting at the first chunk whose start is
	 * after the offset.
	 */

	clone = NULL;

	for (sl = fi->chunklist; sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);
		if (fc->from >= offset) {
			clone = g_slist_copy(sl);
			break;
		}

		/*
		 * If offset lies within a free chunk, it will get split below.
		 * So exit without cloning anything yet.
		 */

		if (DL_CHUNK_EMPTY == fc->status && fc->to - 1 > offset)
			break;
	}

	/*
	 * If we have not cloned anything, it means we have encountered a big chunk
	 * and the selected offset lies within that chunk.
	 * Be smarter and break-up any free chunk into two at the selected offset.
	 */

	if (NULL == clone) {
		for (sl = fi->chunklist; sl; sl = g_slist_next(sl)) {
			struct dl_file_chunk *fc = sl->data;

			dl_file_chunk_check(fc);
			if (DL_CHUNK_EMPTY == fc->status && fc->to - 1 > offset) {
				struct dl_file_chunk *nfc;

				g_assert(fc->from < offset);	/* Or we'd have cloned above */
				g_assert(fc->download == NULL);	/* Chunk is empty */

				/*
				 * fc was [from, to[.  It becomes [from, offset[.
				 * nfc is [offset, to[ and is inserted after fc.
				 */

				nfc = dl_file_chunk_alloc();
				nfc->from = offset;
				nfc->to = fc->to;
				nfc->status = DL_CHUNK_EMPTY;
				fc->to = nfc->from;

				fi->chunklist = gm_slist_insert_after(fi->chunklist, sl, nfc);
				clone = g_slist_copy(g_slist_next(sl));

				g_assert(clone != NULL);		/* The `nfc' chunk is there */

				break;
			}
		}

		g_assert(file_info_check_chunklist(fi, TRUE));
	}

	/*
	 * If still no luck, never mind.  Use original list.
	 */

	if (clone) {
		struct dl_file_chunk *fc;

		/*
		 * Second pass: append to the `clone' list all the chunks that end
		 * before the "from" of the first item in that list.
		 */

		fc = clone->data;
		dl_file_chunk_check(fc);
		offset = fc->from;			/* Cloning point: start of first chunk */
		tail = g_slist_last(clone);

		for (sl = fi->chunklist; sl; sl = g_slist_next(sl)) {
			fc = sl->data;

			dl_file_chunk_check(fc);
			if (fc->to > offset)		/* Not ">=" or we'd miss one chunk */
				break;					/* We've reached the cloning point */
			g_assert(fc->from < offset);
			clone = gm_slist_insert_after(clone, tail, fc);
			tail = g_slist_next(tail);
		}

		return clone;
	} else {
		return fi->chunklist;
	}
}

/**
 * Compute chunksize to be used for the current request.
 */
static filesize_t
fi_chunksize(fileinfo_t *fi)
{
	filesize_t chunksize;
	int src_count;

	file_info_check(fi);

	/*
	 * Chunk size is estimated based on the amount of potential concurrent
	 * downloads we can face (roughly given by the amount of queued sources
	 * plus the amount of active ones).  We also consider the amount of data
	 * that still needs to be fetched, since sources will compete for that.
	 *
	 * The aim is to reduce the chunksize as we progress, to avoid turning
	 * on aggressive swarming if possible since that forces us to close the
	 * connection to the source (and therefore lose the slot, at best, if
	 * the source is not firewalled) whenever we bump into another active
	 * chunk.
	 *		--RAM, 2005-09-27
	 */

	src_count = fi_alive_count(fi);
	src_count = MAX(1, src_count);
	chunksize = (fi->size - fi->done) / src_count;

	/*
	 * Finally trim the computed value so it falls between the boundaries
	 * they want to enforce.
	 */

	if (chunksize < GNET_PROPERTY(dl_minchunksize))
		chunksize = GNET_PROPERTY(dl_minchunksize);
	if (chunksize > GNET_PROPERTY(dl_maxchunksize))
		chunksize = GNET_PROPERTY(dl_maxchunksize);

	return chunksize;
}

/**
 * Compute how much the source covers the missing chunks we still have which
 * are not busy.  This is expressed as a percentage of those missing chunks.
 */
static double
fi_missing_coverage(struct download *d)
{
	GSList *ranges;
	fileinfo_t *fi;
	filesize_t missing_size = 0;
	filesize_t covered_size = 0;
	GSList *fclist;

	download_check(d);
	fi = d->file_info;
	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));
	g_assert(fi->lifecount > 0);

	/*
	 * See update_available_ranges() to understand why we can still get a
	 * non-zero download_ranges_size() despite download_ranges() returning NULL.
	 *
	 * We asssume the server has the whole file if there are no ranges and
	 * a zero ranges_size, as we have not seen any header indicating that the
	 * file would be partial.
	 */

	ranges = download_ranges(d);
	if (ranges == NULL) {
		filesize_t available = download_ranges_size(d);

		return available ? (available * 1.0) / (fi->size * 1.0) : 1.0;
	}

	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;
		const GSList *sl;

		if (DL_CHUNK_EMPTY != fc->status)
			continue;

		missing_size += fc->to - fc->from;

		/*
		 * Look whether this empty chunk intersects with one of the
		 * available ranges.
		 *
		 * NB: the list of ranges is sorted.  And contrary to fi chunks,
		 * the upper boundary of the range (r->end) is part of the range.
		 */

		for (sl = ranges; sl; sl = g_slist_next(sl)) {
			const http_range_t *r = sl->data;
			filesize_t from, to;

			if (r->start > fc->to)
				break;					/* No further range will intersect */

			if (r->start >= fc->from && r->start < fc->to) {
				from = r->start;
				to = MIN(r->end + 1, fc->to);
				covered_size += to - from;
				continue;
			}

			if (r->end >= fc->from && r->end < fc->to) {
				from = MAX(r->start, fc->from);
				to = r->end + 1;
				covered_size += to - from;
				continue;
			}
		}
	}

	g_assert(covered_size <= missing_size);

	if (missing_size == 0)			/* Weird but... */
		return 1.0;					/* they cover the whole of nothing! */

	return (covered_size * 1.0) / (missing_size * 1.0);
}

/**
 * Find the largest busy chunk.
 *
 * @return largest chunk found in the fileinfo, or NULL if there are no
 * busy chunks.
 */
static const struct dl_file_chunk *
fi_find_largest(const fileinfo_t *fi)
{
	GSList *fclist;
	const struct dl_file_chunk *largest = NULL;

	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;

		dl_file_chunk_check(fc);

		if (DL_CHUNK_BUSY != fc->status)
			continue;

		if (
			largest == NULL ||
			(fc->to - fc->from) > (largest->to - largest->from)
		)
			largest = fc;
	}

	return largest;
}

/**
 * Find the largest busy chunk served by the host with the smallest uploading
 * rate.
 *
 * @return chunk found in the fileinfo, or NULL if there are no busy chunks.
 */
static const struct dl_file_chunk *
fi_find_slowest(const fileinfo_t *fi)
{
	GSList *fclist;
	const struct dl_file_chunk *slowest = NULL;
	guint slowest_speed_avg = MAX_INT_VAL(guint);

	for (fclist = fi->chunklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;
		guint speed_avg;

		dl_file_chunk_check(fc);

		if (DL_CHUNK_BUSY != fc->status)
			continue;

		speed_avg = download_speed_avg(fc->download);

		if (
			slowest == NULL ||
			speed_avg < slowest_speed_avg ||
			(
				speed_avg == slowest_speed_avg &&
				(fc->to - fc->from) > (slowest->to - slowest->from)
			)
		) {
			slowest = fc;
			slowest_speed_avg = speed_avg;
		}
	}

	return slowest;
}

/**
 * Find the spot we could download at the tail of an already active chunk
 * to be aggressively completing the file ASAP.
 *
 * @param d		the download source we want to consider making a request to
 * @param busy	the amount of known busy chunks in the file
 * @param from	where the start of the possible chunk request will be written
 * @param to	where the end of the possible chunk request will be written
 *
 * @return TRUE if we were able to find a candidate, with `from' and `to'
 * being filled with the chunk we could be requesting.
 */
static gboolean
fi_find_aggressive_candidate(
	struct download *d, guint busy, filesize_t *from, filesize_t *to)
{
	fileinfo_t *fi = d->file_info;
	const struct dl_file_chunk *fc;
	int starving;
	filesize_t minchunk;
	gboolean can_be_aggressive = FALSE;
	double missing_coverage;

	/*
	 * Compute minimum chunk size for splitting.  When we're told to
	 * be aggressive and we need to be, we don't really want to honour
	 * the dl_minchunksize setting!
	 *
	 * There are fi->lifecount active downloads (queued or running) for
	 * this file, and `busy' chunks.  The difference is the amount of
	 * starving downloads...
	 */

	starving = fi->lifecount - busy;	/* Starving downloads */
	minchunk = (fi->size - fi->done) / (2 * starving);
	minchunk = MIN(minchunk, GNET_PROPERTY(dl_minchunksize));
	minchunk = MAX(minchunk, FI_MIN_CHUNK_SPLIT);

	fc = fi_find_largest(fi);

	if (fc && fc->to - fc->from < minchunk)
		fc = NULL;

	/*
	 * Do not let a slow uploading server interrupt a chunk served by
	 * a faster server if the time it will take to complete the chunk is
	 * larger than what the currently serving host would perform alone!
	 *
	 * However, if the slower server covers 100% of the missing chunks we
	 * need, whereas the faster server does not have 100% of them, it would
	 * be a shame to lose the connection to this slower server.  So we
	 * take into account the missing chunk coverage rate as well.
	 *
	 * We always interrupt a chunk covered by a non-active download, i.e.
	 * when that chunk is only being requested and is not yet served, as we
	 * don't know whether that request will succeed.
	 */

	missing_coverage = fi_missing_coverage(d);

	if (fc) {
		double longest_missing_coverage = fi_missing_coverage(fc->download);

		download_check(fc->download);

		can_be_aggressive =
			!DOWNLOAD_IS_ACTIVE(fc->download) ||
			missing_coverage > longest_missing_coverage ||
			(
				missing_coverage == longest_missing_coverage &&
				download_speed_avg(d) > download_speed_avg(fc->download)
			);

		if (GNET_PROPERTY(download_debug) > 1)
			g_message("will %s be aggressive for \"%s\" given d/l speed "
				"of %s%u B/s for largest chunk owner and %u B/s for stealer, "
				"and a coverage of missing chunks of %.2f%% and "
				"%.2f%% respectively",
				can_be_aggressive ? "really" : "not",
				fi->pathname,
				download_is_stalled(fc->download) ? "stalling " : "",
				download_speed_avg(fc->download), download_speed_avg(d),
				longest_missing_coverage * 100.0, missing_coverage * 100.0);
	}

	if (!can_be_aggressive && (fc = fi_find_slowest(fi))) {
		/*
		 * We couldn't be aggressive with the largest chunk.
		 * Try to see if we're faster than the slowest serving host and have
		 * a larger coverage of the missing chunks.
		 */

		can_be_aggressive =
			!DOWNLOAD_IS_ACTIVE(fc->download) ||
			missing_coverage >= fi_missing_coverage(fc->download);

		if (can_be_aggressive && GNET_PROPERTY(download_debug) > 1)
			g_message("will instead be aggressive for \"%s\" given d/l speed "
				"of %s%u B/s for slowest chunk owner and %u B/s for stealer, "
				"and a coverage of missing chunks of %.2f%% and "
				"%.2f%% respectively",
				fi->pathname,
				download_is_stalled(fc->download) ? "stalling " : "",
				download_speed_avg(fc->download), download_speed_avg(d),
				fi_missing_coverage(fc->download) * 100.0,
				missing_coverage * 100.0);
	}

	if (!can_be_aggressive)
		return FALSE;

	if (fc->to - fc->from >= 2 * FI_MIN_CHUNK_SPLIT) {
		/* Start in the middle of the selected range */
		*from = (fc->from + fc->to - 1) / 2;
		*to = fc->to;		/* 'to' is NOT in the range */
	} else {
		/* Range too small, grab everything */
		*from = fc->from;
		*to = fc->to;
	}

	if (GNET_PROPERTY(download_debug) > 1)
		g_message("aggressively requesting %s@%s for \"%s\" using %s source",
			filesize_to_string(*to - *from), short_size(*from, FALSE),
			fi->pathname,
			d->ranges != NULL ? "partial" : "complete");

	return TRUE;
}
	
/**
 * Finds a range to download, and stores it in *from and *to.
 * If "aggressive" is off, it will return only ranges that are
 * EMPTY. If on, and no EMPTY ranges are available, it will
 * grab a chunk out of the longest BUSY chunk instead, and
 * "compete" with the download that reserved it.
 */
enum dl_chunk_status
file_info_find_hole(struct download *d, filesize_t *from, filesize_t *to)
{
	GSList *fclist;
	fileinfo_t *fi = d->file_info;
	filesize_t chunksize;
	guint busy = 0;
	GSList *cklist;
	gboolean cloned = FALSE;

	file_info_check(fi);
	g_assert(fi->refcount > 0);
	g_assert(fi->lifecount > 0);
	g_assert(0 == fi_busy_count(fi, d));	/* No reservation for `d' yet */
	g_assert(file_info_check_chunklist(fi, TRUE));

	/*
	 * Ensure the file has not disappeared.
	 */

	if (fi->done) {
		if (fi->done == fi->size)
			return DL_CHUNK_DONE;

		fi_check_file(fi);
	}

	if (fi->size < d->file_size) {
		g_warning("fi->size=%s < d->file_size=%s for \"%s\"",
			uint64_to_string(fi->size), uint64_to_string2(d->file_size),
			fi->pathname);
	}

	g_assert(fi->lifecount > 0);

	/*
	 * If PFSP is enabled and we know of a small amount of sources,
	 * try to request a small chunk the first time, in order to help
	 * the download mesh to propagate: we need to advertise ourselves
	 * to others, so more will come and we get more alt-loc exchanges.
	 *
	 * We do that only the first time we reconnect to a source to force
	 * a rapid exchange of alt-locs in case the amount the other source
	 * knows is more that what can fit in the reply (hoping remote will
	 * perform a random selection among its known set).
	 *
	 *		--RAM, 2005-10-27
	 */

	chunksize = fi_chunksize(fi);

	if (
		GNET_PROPERTY(pfsp_server) && d->served_reqs == 0 &&
		fi_alive_count(fi) <= FI_LOW_SRC_COUNT		/* Not enough sources */
	) {
		/*
		 * If we have enough to share the file, we can reduce the chunksize.
		 * Otherwise, try to get the amount we miss first, to be able
		 * to advertise ourselves as soon as possible.
		 */

		if (fi->size >= GNET_PROPERTY(pfsp_minimum_filesize))
			chunksize = GNET_PROPERTY(dl_minchunksize);
		else {
			filesize_t missing;
		   
			missing = GNET_PROPERTY(pfsp_minimum_filesize) - fi->size;
			chunksize = MAX(chunksize, missing);
			chunksize = MIN(chunksize, GNET_PROPERTY(dl_maxchunksize));
		}
	}

	/*
	 * If PFSP-server is enabled, we can serve partially downloaded files.
	 * Therefore, it is interesting to request chunks in random order, to
	 * avoid everyone having the same chunks should full sources disappear.
	 *		--RAM, 11/10/2003
	 */

	if (GNET_PROPERTY(pfsp_server)) {
		cklist = list_clone_shift(fi);
		if (cklist != fi->chunklist)
			cloned = TRUE;
	} else
		cklist = fi->chunklist;

	for (fclist = cklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;

		dl_file_chunk_check(fc);

		if (DL_CHUNK_EMPTY != fc->status) {
			if (DL_CHUNK_BUSY == fc->status)
				busy++;		/* Will be used by assert below */
			continue;
		}

		*from = fc->from;
		*to = fc->to;
		if ((fc->to - fc->from) > chunksize)
			*to = fc->from + chunksize;

		file_info_update(d, *from, *to, DL_CHUNK_BUSY);
		goto selected;
	}

	g_assert(fi->lifecount > (gint32) busy); /* Or we'd found a chunk before */

	if (GNET_PROPERTY(use_aggressive_swarming)) {
		filesize_t start, end;

		if (fi_find_aggressive_candidate(d, busy, &start, &end)) {
			file_info_update(d, start, end, DL_CHUNK_BUSY);
			*from = start;
			*to = end;
			goto selected;
		}
	}

	/* No holes found. */

	if (cloned)
		g_slist_free(cklist);

	return (fi->done == fi->size) ? DL_CHUNK_DONE : DL_CHUNK_BUSY;

selected:	/* Selected a hole to download */

	g_assert(file_info_check_chunklist(fi, TRUE));

	if (cloned)
		g_slist_free(cklist);

	return DL_CHUNK_EMPTY;
}

/**
 * Find free chunk that also fully belongs to the `ranges' list.  If found,
 * the returned chunk is marked BUSY and linked to the download `d'.
 *
 * @returns TRUE if one was found, with `from' and `to' set, FALSE otherwise.
 *
 * @attention
 * NB: In accordance with other fileinfo semantics, `to' is NOT the last byte
 * of the range but one byte AFTER the end.
 */
gboolean
file_info_find_available_hole(
	struct download *d, GSList *ranges, filesize_t *from, filesize_t *to)
{
	GSList *fclist;
	fileinfo_t *fi;
	filesize_t chunksize;
	GSList *cklist;
	gboolean cloned = FALSE;
	guint busy = 0;

	download_check(d);
	g_assert(ranges);

	fi = d->file_info;
	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	/*
	 * Ensure the file has not disappeared.
	 */

	if (fi->done) {
		if (fi->done == fi->size)
			return FALSE;;

		fi_check_file(fi);
	}

	g_assert(fi->lifecount > 0);

	/*
	 * If PFSP-server is enabled, we can serve partially downloaded files.
	 * Therefore, it is interesting to request chunks in random order, to
	 * avoid everyone having the same chunks should full sources disappear.
	 *		--RAM, 11/10/2003
	 */

	if (GNET_PROPERTY(pfsp_server)) {
		cklist = list_clone_shift(fi);
		if (cklist != fi->chunklist)
			cloned = TRUE;
	} else
		cklist = fi->chunklist;

	for (fclist = cklist; fclist; fclist = g_slist_next(fclist)) {
		const struct dl_file_chunk *fc = fclist->data;
		const GSList *sl;

		if (DL_CHUNK_EMPTY != fc->status) {
			if (DL_CHUNK_BUSY == fc->status)
				busy++;		/* Will be used by aggresive code below */
			continue;
		}

		/*
		 * Look whether this empty chunk intersects with one of the
		 * available ranges.
		 *
		 * NB: the list of ranges is sorted.  And contrary to fi chunks,
		 * the upper boundary of the range (r->end) is part of the range.
		 */

		for (sl = ranges; sl; sl = g_slist_next(sl)) {
			const http_range_t *r = sl->data;

			if (r->start > fc->to)
				break;					/* No further range will intersect */

			if (r->start >= fc->from && r->start < fc->to) {
				*from = r->start;
				*to = MIN(r->end + 1, fc->to);
				goto found;
			}

			if (r->end >= fc->from && r->end < fc->to) {
				*from = MAX(r->start, fc->from);
				*to = r->end + 1;
				goto found;
			}
		}
	}

	if (GNET_PROPERTY(use_aggressive_swarming)) {
		filesize_t start, end;

		if (fi_find_aggressive_candidate(d, busy, &start, &end)) {
			const GSList *sl;

			/*
			 * Look whether this candidate chunk is fully held in the
			 * available remote chunks.
			 *
			 * NB: the list of ranges is sorted.  And contrary to fi chunks,
			 * the upper boundary of the range (r->end) is part of the range.
			 */

			for (sl = ranges; sl; sl = g_slist_next(sl)) {
				const http_range_t *r = sl->data;

				if (r->start > end)
					break;				/* No further range will intersect */

				if (r->start <= start && r->end >= (end - 1)) {
					/* Selected chunk is fully contained in remote range */
					*from = start;
					*to = end;
					goto selected;
				}
			}
		}
	}

	if (cloned)
		g_slist_free(cklist);

	return FALSE;

found:
	chunksize = fi_chunksize(fi);

	if ((*to - *from) > chunksize)
		*to = *from + chunksize;

selected:
	file_info_update(d, *from, *to, DL_CHUNK_BUSY);

	g_assert(file_info_check_chunklist(fi, TRUE));

	if (cloned)
		g_slist_free(cklist);

	return TRUE;
}

/**
 * Called when we add something to the dmesh.
 *
 * Add the corresponding file to the download list if we're swarming
 * on it.
 *
 * @param file_name	the remote file name (as in the GET query).
 * @param idx	the remote file index (as in the GET query).
 * @param addr	the remote servent address.
 * @param port	the remote servent port.
 * @param sha1	the SHA1 of the file.
 */
void
file_info_try_to_swarm_with(
	const char *file_name, const host_addr_t addr, guint16 port,
	const struct sha1 *sha1)
{
	fileinfo_t *fi;

	if (!can_swarm)				/* Downloads not initialized yet */
		return;

	fi = file_info_by_sha1(sha1);
	if (!fi)
		return;

	file_info_check(fi);
	download_auto_new(file_name ? file_name : filepath_basename(fi->pathname),
		fi->size,
		addr,
		port,
		&blank_guid,
		NULL,	/* hostname */
		sha1,
		NULL,	/* TTH */
		tm_time(),
		fi,
		NULL,	/* proxies */
		/* FIXME: TLS? */ 0);
}

/**
 * Scan the given directory for files, looking at those bearing a valid
 * fileinfo trailer, yet which we know nothing about.
 */
void
file_info_scandir(const char *dir)
{
	DIR *d;
	struct dirent *dentry;
	fileinfo_t *fi;
	char *pathname = NULL;

	g_return_if_fail(dir);
	g_return_if_fail(is_absolute_path(dir));

	d = opendir(dir);
	if (NULL == d) {
		g_warning("can't open directory %s: %s", dir, g_strerror(errno));
		return;
	}

	while (NULL != (dentry = readdir(d))) {
		G_FREE_NULL(pathname);

		/**
		 * Skip ".", "..", and hidden files. We don't create any
	   	 * and we also must skip the lock file.
		 */
		if ('.' == dentry->d_name[0])
			continue;

		switch (dir_entry_mode(dentry)) {
		case 0:
		case S_IFREG:
		case S_IFLNK:
			break;
		default:
			continue;
		}

		pathname = make_pathname(dir, dentry->d_name);

		if (!S_ISREG(dir_entry_mode(dentry))) {
			struct stat sb;

			if (-1 == stat(pathname, &sb)) {
				g_warning("cannot stat %s: %s", pathname, g_strerror(errno));
				continue;
			}
			if (!S_ISREG(sb.st_mode))			/* Only regular files */
				continue;
		}

		fi = file_info_retrieve_binary(pathname);
		if (NULL == fi)
			continue;

		if (file_info_lookup_dup(fi)) {
			/* Already know about this */
			fi_free(fi);
			fi = NULL;
			continue;
		}

		/*
		 * We found an entry that we do not know about.
		 */

		file_info_merge_adjacent(fi);		/* Update fi->done */
		file_info_hash_insert(fi);

		g_warning("reactivated orphan entry (%.02f%% done, %s SHA1): %s",
			fi->done * 100.0 / (0 == fi->size ? 1 : fi->size),
			fi->sha1 ? "with" : "no", pathname);
	}

	G_FREE_NULL(pathname);
	closedir(d);
}

/**
 * Callback for hash table iterator. Used by file_info_completed_orphans().
 */
static void
fi_spot_completed_kv(gpointer key, gpointer val, gpointer unused_x)
{
	fileinfo_t *fi = val;

	(void) unused_x;
	file_info_check(fi);

	g_assert(key == fi->pathname); /* name shared with fi's, don't free */

	if (fi->refcount)					/* Attached to a download */
		return;

	/*
	 * If the file is 100% done, fake a new download.
	 *
	 * It will be trapped by download_resume_bg_tasks() and handled
	 * as any complete download.
	 */

	if (FILE_INFO_COMPLETE(fi)) {
		download_orphan_new(filepath_basename(fi->pathname),
			fi->size, fi->sha1, fi);
	}
}

/**
 * Look through all the known fileinfo structures, looking for orphaned
 * files that are complete.
 *
 * A fake download is created for them, so that download_resume_bg_tasks()
 * can pick them up.
 */
void
file_info_spot_completed_orphans(void)
{
	g_hash_table_foreach(fi_by_outname, fi_spot_completed_kv, NULL);
}

void
fi_add_listener(fi_listener_t cb, gnet_fi_ev_t ev,
	frequency_t t, guint32 interval)
{
    g_assert(ev < EV_FI_EVENTS);

    event_add_subscriber(fi_events[ev], (GCallback) cb, t, interval);
}

void
fi_remove_listener(fi_listener_t cb, gnet_fi_ev_t ev)
{
    g_assert(ev < EV_FI_EVENTS);

    event_remove_subscriber(fi_events[ev], (GCallback) cb);
}

void
src_add_listener(src_listener_t cb, gnet_src_ev_t ev,
	frequency_t t, guint32 interval)
{
    g_assert(UNSIGNED(ev) < EV_SRC_EVENTS);

    event_add_subscriber(src_events[ev], (GCallback) cb, t, interval);
}

void
src_remove_listener(src_listener_t cb, gnet_src_ev_t ev)
{
    g_assert(UNSIGNED(ev) < EV_SRC_EVENTS);

    event_remove_subscriber(src_events[ev], (GCallback) cb);
}

/**
 * Get an information structure summarizing the file info.
 * This is used by the GUI to avoid peeking into the file info structure
 * directly: it has its own little pre-digested information to display.
 */
gnet_fi_info_t *
fi_get_info(gnet_fi_t fih)
{
    fileinfo_t *fi;
    gnet_fi_info_t *info;
	const struct sha1 *sha1;

    fi = file_info_find_by_handle(fih);
	file_info_check(fi);

    info = walloc(sizeof *info);

    info->guid = atom_guid_get(fi->guid);
    info->filename = atom_str_get(filepath_basename(fi->pathname));
	sha1 = fi->sha1 ? fi->sha1 : fi->cha1;
    info->sha1 = sha1 ? atom_sha1_get(sha1) : NULL;
    info->tth = fi->tth ? atom_tth_get(fi->tth) : NULL;
    info->fi_handle = fi->fi_handle;
	info->size = fi->size;

	info->tth_slice_size = fi->tigertree.slice_size;
	info->tth_num_leaves = fi->tigertree.num_leaves;
	info->created		 = fi->created;
	info->tth_depth      = tt_depth(fi->tigertree.num_leaves);

    return info;
}

/**
 * Dispose of the info structure.
 */
void
fi_free_info(gnet_fi_info_t *info)
{
    g_assert(NULL != info);

	atom_guid_free_null(&info->guid);
	atom_str_free_null(&info->filename);
	atom_sha1_free_null(&info->sha1);
	atom_tth_free_null(&info->tth);

    wfree(info, sizeof *info);
}

void
fi_increase_uploaded(fileinfo_t *fi, size_t amount)
{
	file_info_check(fi);
	fi->uploaded += amount;	
	file_info_changed(fi);
}

/**
 * Fill in the fileinfo status structure "s" using the fileinfo associated
 * with the fileinfo handle "fih".
 */
void
fi_get_status(gnet_fi_t fih, gnet_fi_status_t *s)
{
    fileinfo_t *fi = file_info_find_by_handle(fih);

	file_info_check(fi);
    g_assert(NULL != s);

    s->recvcount      = fi->recvcount;
    s->refcount       = fi->refcount;
    s->lifecount      = fi->lifecount;
    s->done           = fi->done;
	s->uploaded		  = fi->uploaded;
    s->recv_last_rate = fi->recv_last_rate;
    s->size           = fi->size;
    s->active_queued  = fi->active_queued;
    s->passive_queued = fi->passive_queued;
	s->modified		  =	fi->modified;

	s->paused		  = 0 != (FI_F_PAUSED & fi->flags);
	s->seeding		  = 0 != (FI_F_SEEDING & fi->flags);
	s->finished		  = 0 != FILE_INFO_FINISHED(fi);
	s->complete		  = 0 != FILE_INFO_COMPLETE(fi);
	s->has_sha1 	  = NULL != fi->sha1;
	s->sha1_matched   = s->complete && s->has_sha1 && fi->sha1 == fi->cha1;
	s->verifying	  = s->complete && !s->finished && s->has_sha1 && !fi->cha1;

	s->copied 		  = s->complete ? fi->copied : 0;
	s->sha1_hashed    = s->complete ? fi->cha1_hashed : 0;
}

/**
 * Get a list with information about each chunk and status. Returns a
 * linked list of chunks with just the end byte and the status. The
 * list is fully allocated and the receiver is responsible for freeing
 * up the memory.
 */
GSList *
fi_get_chunks(gnet_fi_t fih)
{
    const fileinfo_t *fi = file_info_find_by_handle(fih);
    const GSList *sl;
	GSList *chunks = NULL;

    file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

    for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
        const struct dl_file_chunk *fc = sl->data;
    	gnet_fi_chunks_t *chunk;

        chunk = walloc(sizeof *chunk);
        chunk->from   = fc->from;
        chunk->to     = fc->to;
        chunk->status = fc->status;
        chunk->old    = TRUE;

		chunks = g_slist_prepend(chunks, chunk);
    }

    return g_slist_reverse(chunks);
}

/**
 * Free chunk list got by calling fi_get_chunks.
 */
void
fi_free_chunks(GSList *chunks)
{
    GSList *sl;

    for (sl = chunks; NULL != sl; sl = g_slist_next(sl)) {
    	gnet_fi_chunks_t *chunk = sl->data;
        wfree(chunk, sizeof *chunk);
    }

    g_slist_free(chunks);
}


/**
 * Get a list of available ranges for this fileinfo handle.
 * The list is fully allocated and the receiver is responsible for
 * freeing up the memory, for example using fi_free_ranges().
 */
GSList *
fi_get_ranges(gnet_fi_t fih)
{
    fileinfo_t *fi = file_info_find_by_handle(fih);
    http_range_t *range = NULL;
	GSList *ranges = NULL;
    const GSList *sl;

    file_info_check(fi);

    for (sl = fi->seen_on_network; NULL != sl; sl = g_slist_next(sl)) {
        const http_range_t *r = sl->data;
        range = walloc(sizeof *range);
        range->start = r->start;
        range->end   = r->end;

		ranges = g_slist_prepend(ranges, range);
	}

    return g_slist_reverse(ranges);
}

void
fi_free_ranges(GSList *ranges)
{
	GSList *sl;

	for (sl = ranges; NULL != sl; sl = g_slist_next(sl)) {
        http_range_t *r = sl->data;
		wfree(r, sizeof *r);
	}

	g_slist_free(ranges);
}



/**
 * @return NULL terminated array of char * pointing to the aliases.
 * You can easily free the returned array with g_strfreev().
 *
 * O(2n) - n: number of aliases
 */
char **
fi_get_aliases(gnet_fi_t fih)
{
    char **a;
    guint len;
    GSList *sl;
    guint n;
    fileinfo_t *fi = file_info_find_by_handle(fih);

    len = g_slist_length(fi->alias);

    a = g_malloc((len + 1) * sizeof a[0]);
    a[len] = NULL; /* terminate with NULL */;

    for (sl = fi->alias, n = 0; NULL != sl; sl = g_slist_next(sl), n++) {
        g_assert(n < len);
        a[n] = g_strdup(sl->data);
    }

    return a;
}

/**
 * Add new download source for the file.
 */
void
file_info_add_new_source(fileinfo_t *fi, struct download *d)
{
	fi->ntime = tm_time();
	file_info_add_source(fi, d);
}

/**
 * Add download source for the file, but preserve original "ntime".
 */
void
file_info_add_source(fileinfo_t *fi, struct download *d)
{
	file_info_check(fi);
	g_assert(NULL == d->file_info);
	g_assert(!d->src_handle_valid);

	fi->refcount++;
	fi->dirty_status = TRUE;
	d->file_info = fi;
	d->src_handle = idtable_new_id(src_handle_map, d);
	d->src_handle_valid = TRUE;
	fi->sources = g_slist_prepend(fi->sources, d);

	if (download_is_alive(d)) {
		g_assert(fi->refcount > fi->lifecount);
		fi->lifecount++;
	}

	if (1 == fi->refcount) {
		g_assert(GNET_PROPERTY(fi_with_source_count)
				< GNET_PROPERTY(fi_all_count));
		gnet_prop_incr_guint32(PROP_FI_WITH_SOURCE_COUNT);
	}

	src_event_trigger(d, EV_SRC_ADDED);
}

/**
 * Removing one source reference from the fileinfo.
 * When no sources reference the fileinfo structure, free it if `discard'
 * is TRUE, or if the fileinfo has been marked with FI_F_DISCARD.
 * This replaces file_info_free()
 */
void
file_info_remove_source(fileinfo_t *fi, struct download *d, gboolean discard)
{
	file_info_check(fi);
	g_assert(NULL != d->file_info);
	g_assert(d->src_handle_valid);
	g_assert(fi->refcount > 0);
	g_assert(fi->refcount >= fi->lifecount);
	g_assert(fi->hashed);

	src_event_trigger(d, EV_SRC_REMOVED);

	idtable_free_id(src_handle_map, d->src_handle);
	d->src_handle_valid = FALSE;

	if (download_is_alive(d)) {
		fi->lifecount--;
	}
	fi->refcount--;
	fi->dirty_status = TRUE;
	d->file_info = NULL;
	fi->sources = g_slist_remove(fi->sources, d);

	/*
	 * We don't free the structure when `discard' is FALSE: keeping the
	 * fileinfo around means it's still in the hash tables, and therefore
	 * that its SHA1, if any, is still around to help us spot duplicates.
	 *
	 * At times however, we really want to discard an unreferenced fileinfo
	 * as soon as this happens.
	 */

	if (0 == fi->refcount) {
		g_assert(GNET_PROPERTY(fi_with_source_count) > 0);
		gnet_prop_decr_guint32(PROP_FI_WITH_SOURCE_COUNT);

		if (discard || (fi->flags & FI_F_DISCARD)) {
			file_info_hash_remove(fi);
			fi_free(fi);
		}
    }
}

/**
 * Get a copy of the sources list for a fileinfo. The items have the
 * "struct download *".
 *
 * @return A copy of the sources list.
 */
GSList *
file_info_get_sources(const fileinfo_t *fi)
{
	file_info_check(fi);

	return g_slist_copy(fi->sources);
}

/**
 * Remove non-referenced fileinfo and reclaim its data structures.
 */
void
file_info_remove(fileinfo_t *fi)
{
	file_info_check(fi);
	g_assert(fi->refcount == 0);

	file_info_hash_remove(fi);
	fi_free(fi);
}

static void
fi_notify_helper(gpointer unused_key, gpointer value, gpointer unused_udata)
{
    fileinfo_t *fi = value;

	(void) unused_key;
	(void) unused_udata;

	file_info_check(fi);
    if (!fi->dirty_status)
        return;

    fi->dirty_status = FALSE;
	file_info_changed(fi);
}

/**
 * Called every second by the main timer.
 */
void
file_info_timer(void)
{
	g_hash_table_foreach(fi_by_outname, fi_notify_helper, NULL);
}

/**
 * Query the DHT for a SHA1 search if needed and appropriate.
 */
static void
fi_dht_query(fileinfo_t *fi)
{
	time_delta_t retry_period = FI_DHT_PERIOD;

	file_info_check(fi);

	if (NULL == fi->sha1 || FILE_INFO_FINISHED(fi))
		return;

	if (FI_F_PAUSED & fi->flags)
		return;

	/*
	 * If the file is already being actively downloaded from "enough"
	 * sources, no queries are needed, the download mesh should be correctly
	 * seeded and sufficient.
	 */

	if (fi->recvcount >= FI_DHT_RECV_THRESH)
		return;

	/*
	 * Even if the file is queued, querying the DHT could be useful.
	 * However, we don't want to requeue as often when we have sources.
	 * An actively queued source counts twice as much as a passive.
	 */

	retry_period = time_delta_add(retry_period,
		FI_DHT_SOURCE_DELAY * (fi->lifecount - fi->recvcount));
	retry_period = time_delta_add(retry_period,
		FI_DHT_QUEUED_DELAY * (2 * fi->active_queued + fi->passive_queued));
	retry_period = time_delta_add(retry_period,
		FI_DHT_RECV_DELAY * fi->recvcount);

	if (
		fi->last_dht_query &&
		delta_time(tm_time(), fi->last_dht_query) < retry_period
	)
		return;

	fi->last_dht_query = tm_time();
	gdht_find_sha1(fi);
}

/**
 * Hash table iterator to launch DHT queries.
 */
static void
fi_dht_check(gpointer unused_key, gpointer value, gpointer unused_udata)
{
    fileinfo_t *fi = value;

	(void) unused_key;
	(void) unused_udata;

	fi_dht_query(fi);
}

/**
 * Initiate a SHA1 query in the DHT immediately, without waiting for periodic
 * monitoring of sourceless fileinfos.
 */
void
file_info_dht_query(const sha1_t *sha1)
{
	fileinfo_t *fi;

	g_assert(sha1);

	if (!dht_bootstrapped())
		return;

	fi = file_info_by_sha1(sha1);
	if (fi)
		fi_dht_query(fi);
}

/**
 * Slower timer called every few minutes (about 6).
 */
void
file_info_slow_timer(void)
{
	if (!dht_bootstrapped())
		return;

	g_hash_table_foreach(fi_by_outname, fi_dht_check, NULL);
}

/**
 * Kill all downloads associated with a fi and remove the fi itself.
 *
 * Will return FALSE if download could not be removed because it was still in
 * use, e.g. when it is being verified.
 * 		-- JA 25/10/03
 */
gboolean
file_info_purge(fileinfo_t *fi)
{
	GSList *sl;
	GSList *csl;
	gboolean do_remove;

	file_info_check(fi);
	g_assert(fi->hashed);

	do_remove = !(fi->flags & FI_F_DISCARD) || NULL == fi->sources;
	csl = g_slist_copy(fi->sources);	/* Clone list, orig can be modified */

	for (sl = csl; NULL != sl; sl = g_slist_next(sl)) {
		struct download *d = sl->data;

		download_abort(d);
		if (!download_remove(d)) {
			g_slist_free(csl);
			return FALSE;
		}
	}

	g_slist_free(csl);

	if (do_remove) {
		/*
	 	* Downloads not freed at this point, this will happen when the
	 	* download_free_removed() is asynchronously called.  However, all
	 	* references to the file info has been cleared, so we can remove it.
	 	*/

		g_assert(0 == fi->refcount);

		file_info_unlink(fi);
		file_info_hash_remove(fi);
		fi_free(fi);
	}

	return TRUE;
}

gboolean
fi_purge(gnet_fi_t fih)
{
	return file_info_purge(file_info_find_by_handle(fih));
}

void
fi_pause(gnet_fi_t fih)
{
	file_info_pause(file_info_find_by_handle(fih));
}

void
fi_resume(gnet_fi_t fih)
{
	file_info_resume(file_info_find_by_handle(fih));
}

gboolean
fi_rename(gnet_fi_t fih, const char *filename)
{
	return file_info_rename(file_info_find_by_handle(fih), filename);
}

/**
 * Emit a single X-Available header, letting them know we hold a partial
 * file and how many bytes exactly, in case they want to prioritize their
 * download requests depending on file completion criteria.
 *
 * @return the size of the generated header.
 */
size_t
file_info_available(const fileinfo_t *fi, char *buf, size_t size)
{
	header_fmt_t *fmt;
	size_t len, rw;

	file_info_check(fi);
	g_assert(size_is_non_negative(size));

	fmt = header_fmt_make("X-Available", " ",
		UINT64_DEC_BUFLEN + sizeof("X-Available: bytes") + 2, size);

	header_fmt_append_value(fmt, "bytes");
	header_fmt_append_value(fmt, uint64_to_string(fi->done));
	header_fmt_end(fmt);

	len = header_fmt_length(fmt);
	g_assert(len < size);
	rw = clamp_strncpy(buf, size, header_fmt_string(fmt), len);
	header_fmt_free(&fmt);

	g_assert(rw < size);	/* No clamping occurred */

	return rw;
}

/**
 * Emit an X-Available-Ranges header listing the ranges within the file that
 * we have on disk and we can share as a PFSP-server.  The header is emitted
 * in `buf', which is `size' bytes long.
 *
 * If there is not enough room to emit all the ranges, emit a random subset
 * of the ranges but include an extra "X-Available" header to let them know
 * how many bytes we really have.
 *
 * @return the size of the generated header.
 */
size_t
file_info_available_ranges(const fileinfo_t *fi, char *buf, size_t size)
{
	const struct dl_file_chunk **fc_ary;
	header_fmt_t *fmt, *fmta = NULL;
	gboolean is_first = TRUE;
	char range[2 * UINT64_DEC_BUFLEN + sizeof(" bytes ")];
	GSList *sl;
	int count;
	int nleft;
	int i;
	size_t rw;
	const char *x_available_ranges = "X-Available-Ranges";

	file_info_check(fi);
	g_assert(size_is_non_negative(size));
	g_assert(file_info_check_chunklist(fi, TRUE));

	fmt = header_fmt_make(x_available_ranges, ", ", size, size);

	for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);
		if (DL_CHUNK_DONE != fc->status)
			continue;

		gm_snprintf(range, sizeof range, "%s%s-%s",
			is_first ? "bytes " : "",
			uint64_to_string(fc->from), uint64_to_string2(fc->to - 1));

		if (!header_fmt_append_value(fmt, range))
			break;
		is_first = FALSE;
	}

	if (NULL == sl)
		goto emit;

	/*
	 * Not everything fitted.  We have to be smarter and include only what
	 * can fit in the size we were given.
	 *
	 * However, to let them know how much file data we really hold, we're also
	 * going to include an extra "X-Available" header specifying how many
	 * bytes we have.
	 */

	header_fmt_free(&fmt);

	{
		size_t len;

		fmta = header_fmt_make("X-Available", " ",
			UINT64_DEC_BUFLEN + sizeof("X-Available: bytes") + 2, size);

		header_fmt_append_value(fmta, "bytes");
		header_fmt_append_value(fmta, uint64_to_string(fi->done));
		header_fmt_end(fmta);

		len = header_fmt_length(fmta);
		len = size > len ? size - len : 0;

		fmt = header_fmt_make(x_available_ranges, ", ", size, len);
	}

	is_first = TRUE;

	/*
	 * See how many chunks we have.
	 */

	for (count = 0, sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;
		dl_file_chunk_check(fc);
		if (DL_CHUNK_DONE == fc->status)
			count++;
	}

	/*
	 * Reference all the "done" chunks in `fc_ary'.
	 */

	g_assert(count > 0);		/* Or there would be nothing to emit */

	fc_ary = halloc(count * sizeof fc_ary[0]);

	for (i = 0, sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;
		dl_file_chunk_check(fc);
		if (DL_CHUNK_DONE == fc->status)
			fc_ary[i++] = fc;
	}

	g_assert(i == count);

	/*
	 * Now select chunks randomly from the set, and emit them if they fit.
	 */

	for (nleft = count; nleft > 0; nleft--) {
		const struct dl_file_chunk *fc;
		int j;

		j = random_value(nleft - 1);
		g_assert(j >= 0 && j < nleft);

		fc = fc_ary[j];
		dl_file_chunk_check(fc);	
		g_assert(DL_CHUNK_DONE == fc->status);

		gm_snprintf(range, sizeof range, "%s%s-%s",
			is_first ? "bytes " : "",
			uint64_to_string(fc->from), uint64_to_string2(fc->to - 1));

		if (header_fmt_append_value(fmt, range))
			is_first = FALSE;

		/*
		 * Shift upper (nleft - j - 1) items down 1 position.
		 */

		if (nleft - 1 != j)
			memmove(&fc_ary[j], &fc_ary[j + 1],
				(nleft - j - 1) * sizeof fc_ary[0]);
	}

	HFREE_NULL(fc_ary);

emit:
	rw = 0;

	if (fmta) {				/* X-Available header is required */
		size_t len = header_fmt_length(fmta);
		g_assert(len + rw < size);
		rw += clamp_strncpy(&buf[rw], size - rw, header_fmt_string(fmta), len);
		header_fmt_free(&fmta);
	}

	if (!is_first) {		/* Something was recorded in X-Available-Ranges */
		size_t len;
		header_fmt_end(fmt);
		len = header_fmt_length(fmt);
		g_assert(len + rw < size);
		rw += clamp_strncpy(&buf[rw], size - rw, header_fmt_string(fmt), len);
	}

	header_fmt_free(&fmt);

	g_assert(rw < size);	/* No clamping occurred */

	return rw;
}

/**
 * Given a request range `start' (included) and `end' (included) for the
 * partially downloaded file represented by `fi', see whether we can
 * satisfy it, even partially, without touching `start' but only only by
 * possibly moving `end' down.
 *
 * @returns TRUE if the request is satisfiable, with `end' possibly adjusted,
 * FALSE is the request cannot be satisfied because `start' is not within
 * an available chunk.
 */
gboolean
file_info_restrict_range(fileinfo_t *fi, filesize_t start, filesize_t *end)
{
	GSList *sl;

	file_info_check(fi);
	g_assert(file_info_check_chunklist(fi, TRUE));

	for (sl = fi->chunklist; NULL != sl; sl = g_slist_next(sl)) {
		const struct dl_file_chunk *fc = sl->data;

		dl_file_chunk_check(fc);	

		if (DL_CHUNK_DONE != fc->status)
			continue;

		if (start < fc->from || start >= fc->to)
			continue;		/* `start' is not in the range */

		/*
		 * We found an available chunk within which `start' falls.
		 * Look whether we can serve their whole request, otherwise
		 * shrink the end.
		 */

		if (*end >= fc->to)
			*end = fc->to - 1;

		return TRUE;
	}

	return FALSE;	/* Sorry, cannot satisfy this request */
}

/**
 * Creates a URL which points to a downloads (e.g. you can move this to a
 * browser and download the file there with this URL).
 *
 * @return A newly allocated string.
 */
char *
file_info_build_magnet(gnet_fi_t handle)
{
	struct magnet_resource *magnet;
	const fileinfo_t *fi;
	const GSList *sl;
	char *url;
	int n;
   
	fi = file_info_find_by_handle(handle);
	g_return_val_if_fail(fi, NULL);
	file_info_check(fi);

	magnet = magnet_resource_new();

	/* The filename used for the magnet must be UTF-8 encoded */
	magnet_set_display_name(magnet,
		lazy_filename_to_utf8_normalized(filepath_basename(fi->pathname),
			UNI_NORM_NETWORK));

	if (fi->sha1) {
		magnet_set_sha1(magnet, fi->sha1);
	}
	if (fi->tth) {
		magnet_set_tth(magnet, fi->tth);
	}
	if (fi->file_size_known && fi->size) {
		magnet_set_filesize(magnet, fi->size);
	}

	n = 0;
	for (sl = fi->sources; NULL != sl && n++ < 20; sl = g_slist_next(sl)) {
		struct download *d = sl->data;
		const char *dl_url;

		download_check(d);
		dl_url = download_build_url(d);
		if (dl_url) {
			magnet_add_source_by_url(magnet, dl_url);
		}
	}

	url = magnet_to_string(magnet);
	magnet_resource_free(&magnet);
	return url;
}

/**
 * Creates a file:// URL which points to the file on the local filesystem.
 * If the file has not been created yet, NULL is returned.
 *
 * @return A newly allocated string or NULL.
 */
char *
file_info_get_file_url(gnet_fi_t handle)
{
	fileinfo_t *fi;
	
	fi = file_info_find_by_handle(handle);
	g_return_val_if_fail(fi, NULL);
	file_info_check(fi);
	
	/* Allow partials but not unstarted files */
	return fi->done > 0 ? url_from_absolute_path(fi->pathname) : NULL;
}

/**
 * Create a ranges list with one item covering the whole file.
 * This may be better placed in http.c, but since it is only
 * used here as a utility function for fi_update_seen_on_network
 * it is now placed here.
 *
 * @param[in] size  File size to be used in range creation
 */
static GSList *
fi_range_for_complete_file(filesize_t size)
{
	http_range_t *range;

	range = walloc(sizeof *range);
	range->start = 0;
	range->end = size - 1;

    return g_slist_append(NULL, range);
}

/**
 * Callback for updates to ranges available on the network.
 *
 * This function gets triggered by an event when new ranges
 * information has become available for a download source.
 * We collect the set of currently available ranges in
 * file_info->seen_on_network. Currently we only fold in new ranges
 * from a download source, but we should also remove sets of ranges when
 * a download source is no longer available.
 *
 * @param[in] srcid  The abstract id of the source that had its ranges updated.
 *
 * @bug
 * FIXME: also remove ranges when a download source is no longer available.
 */
static void
fi_update_seen_on_network(gnet_src_t srcid)
{
	struct download *d;
	GSList *old_list;    /* The previous list of ranges, no longer needed */
	GSList *sl;           /* Temporary pointer to help remove old_list */
	GSList *r = NULL;
	GSList *new_r = NULL;

	d = src_get_download(srcid);
	download_check(d);

	old_list = d->file_info->seen_on_network;

	/*
	 * FIXME: this code is currently only triggered by new HTTP ranges
	 * information becoming available. In addition to that we should perhaps
	 * also include add_source and delete_source. We will miss the latter in
	 * this setup especially.
	 */

	/*
	 * Look at all the download sources for this fileinfo and calculate the
	 * overall ranges info for this file.
	 */
	if (GNET_PROPERTY(fileinfo_debug) > 5)
		g_message("*** Fileinfo: %s\n", d->file_info->pathname);

	for (sl = d->file_info->sources; sl; sl = g_slist_next(sl)) {
		struct download *src = sl->data;
		/*
		 * We only count the ranges of a file if it has replied to a recent
		 * request, and if the download request is not done or in an error
		 * state.
		 */
		if (
			src->flags & DL_F_REPLIED &&
			!(
				GTA_DL_COMPLETED == src->status ||
				GTA_DL_ERROR     == src->status ||
				GTA_DL_ABORTED   == src->status ||
				GTA_DL_REMOVED   == src->status ||
				GTA_DL_DONE      == src->status
			)
		) {
			if (GNET_PROPERTY(fileinfo_debug) > 5)
				g_message("    %s:%d replied (%x, %x), ",
					host_addr_to_string(src->server->key->addr),
					src->server->key->port, src->flags, src->status);

			if (!src->file_info->use_swarming || !(src->flags & DL_F_PARTIAL)) {
				/*
				 * Indicate that the whole file is available.
				 * We could just stop here and assign the complete file range,
   				 * but I'm leaving the code as-is so that we can play with the
 				 * info more, e.g. show different colors for ranges that are
				 * available more.
				 */

				if (GNET_PROPERTY(fileinfo_debug) > 5)
					g_message("  whole file is now available");

				{
					GSList *full_r;

					full_r = fi_range_for_complete_file(d->file_info->size);
					new_r = http_range_merge(r, full_r);
					fi_free_ranges(full_r);
				}
			} else {
				/* Merge in the new ranges */
				if (GNET_PROPERTY(fileinfo_debug) > 5)
					g_message("  ranges %s available",
						http_range_to_string(src->ranges));
				new_r = http_range_merge(r, src->ranges);
			}
			fi_free_ranges(r);
			r = new_r;
		}
	}
	d->file_info->seen_on_network = r;

	if (GNET_PROPERTY(fileinfo_debug) > 5)
		g_message("    final ranges: %s", http_range_to_string(r));

	/*
	 * Remove the old list and free its range elements
	 */
	fi_free_ranges(old_list);

	/*
	 * Trigger a changed ranges event so that others can use the updated info.
	 */
	fi_event_trigger(d->file_info, EV_FI_RANGES_CHANGED);
}

struct file_info_foreach {
	file_info_foreach_cb callback;
	gpointer udata;
};

static void
file_info_foreach_helper(gpointer unused_key, gpointer value, gpointer udata)
{
	struct file_info_foreach *data = udata;
    fileinfo_t *fi = value;

	(void) unused_key;

	file_info_check(fi);
	data->callback(fi->fi_handle, data->udata);
}

void
file_info_foreach(file_info_foreach_cb callback, gpointer udata)
{
	struct file_info_foreach data;
	
	g_return_if_fail(fi_by_guid);
	g_return_if_fail(callback);

	data.callback = callback;
	data.udata = udata;
	g_hash_table_foreach(fi_by_guid, file_info_foreach_helper, &data);
}

const char *
file_info_status_to_string(const gnet_fi_status_t *status)
{
	static char buf[4096];

	g_return_val_if_fail(status, NULL);

    if (status->recvcount) {
		guint32 secs;

		if (status->recv_last_rate) {
			secs = (status->size - status->done) / status->recv_last_rate;
		} else {
			secs = 0;
		}
        gm_snprintf(buf, sizeof buf, _("Downloading (TR: %s)"),
			secs ? short_time(secs) : "-");
		return buf;
    } else if (status->seeding) {
		return _("Seeding");
    } else if (status->verifying) {
		if (status->sha1_hashed > 0) {
			gm_snprintf(buf, sizeof buf,
					"%s %s (%.1f%%)", _("Computing SHA1"),
					short_size(status->sha1_hashed,
						GNET_PROPERTY(display_metric_units)),
					(1.0 * status->sha1_hashed / status->size) * 100.0);
			return buf;
		} else {
			return _("Waiting for SHA1 check");
		}
 	} else if (status->complete) {
		static char msg_sha1[1024], msg_copy[1024];

		msg_sha1[0] = '\0';
		if (status->has_sha1) {
			gm_snprintf(msg_sha1, sizeof msg_sha1, "SHA1 %s",
					status->sha1_matched ? _("OK") : _("failed"));
		}

		msg_copy[0] = '\0';
		if (status->copied > 0 && status->copied < status->size) {
			gm_snprintf(msg_copy, sizeof msg_copy,
				"; %s %s (%.1f%%)", _("Moving"),
				short_size(status->copied,
					GNET_PROPERTY(display_metric_units)),
				(1.0 * status->copied / status->size) * 100.0);
		}

		concat_strings(buf, sizeof buf, _("Finished"),
			'\0' != msg_sha1[0] ? "; " : "", msg_sha1,
			'\0' != msg_copy[0] ? "; " : "", msg_copy,
			(void *) 0);

		return buf;
    } else if (0 == status->lifecount) {
		return _("No sources");
    } else if (status->active_queued || status->passive_queued) {
        gm_snprintf(buf, sizeof buf,
            _("Queued (%u active, %u passive)"),
            status->active_queued, status->passive_queued);
		return buf;
    } else if (status->paused) {
        return _("Paused");
    } else {
        return _("Waiting");
    }
}

/**
 * Change the basename of a filename and rename it on-disk.
 * @return TRUE in case of success, FALSE on error.
 */
gboolean
file_info_rename(fileinfo_t *fi, const char *filename)
{
	gboolean success = FALSE;
	char *pathname;

	file_info_check(fi);
	g_return_val_if_fail(fi->hashed, FALSE);
	g_return_val_if_fail(filename, FALSE);
	g_return_val_if_fail(filepath_basename(filename) == filename, FALSE);

	g_return_val_if_fail(!FILE_INFO_COMPLETE(fi), FALSE);
	g_return_val_if_fail(!(FI_F_TRANSIENT & fi->flags), FALSE);
	g_return_val_if_fail(!(FI_F_SEEDING & fi->flags), FALSE);
	g_return_val_if_fail(!(FI_F_STRIPPED & fi->flags), FALSE);

	{
		char *directory, *name;
	   
		directory = filepath_directory(fi->pathname);
		name = gm_sanitize_filename(filename, FALSE, FALSE);

		if (0 == strcmp(filepath_basename(fi->pathname), name)) {
			pathname = NULL;
			success = TRUE;
		} else {
			pathname = file_info_unique_filename(directory, name, "");
		}
		if (name != filename) {
			G_FREE_NULL(name);
		}
		G_FREE_NULL(directory);
	}
	if (NULL != pathname) {
		struct stat sb;

		if (stat(fi->pathname, &sb)) {
			if (ENOENT == errno) {
				/* Assume file hasn't even been created yet */
				success = TRUE;
			}
		} else if (S_ISREG(sb.st_mode)) {
			if (0 == rename(fi->pathname, pathname)) {
				success = TRUE;
			}
		}
		if (success) {
			file_info_moved(fi, pathname);
		}
		G_FREE_NULL(pathname);
	}
	return success;
}

/**
 * Initialize fileinfo handling.
 */
void
file_info_init(void)
{

#define bs_nop(x)	(x)

	BINARY_ARRAY_SORTED(fi_tag_map, struct fi_tag, str, strcmp, bs_nop);

#undef bs_nop

	fi_by_sha1     = g_hash_table_new(sha1_hash, sha1_eq);
	fi_by_namesize = g_hash_table_new(namesize_hash, namesize_eq);
	fi_by_guid     = g_hash_table_new(guid_hash, guid_eq);
	fi_by_outname  = g_hash_table_new(g_str_hash, g_str_equal);

    fi_handle_map = idtable_new();

    fi_events[EV_FI_ADDED]          = event_new("fi_added");
    fi_events[EV_FI_REMOVED]        = event_new("fi_removed");
    fi_events[EV_FI_INFO_CHANGED]   = event_new("fi_info_changed");
	fi_events[EV_FI_RANGES_CHANGED] = event_new("fi_ranges_changed");
    fi_events[EV_FI_STATUS_CHANGED] = event_new("fi_status_changed");
    fi_events[EV_FI_STATUS_CHANGED_TRANSIENT] =
									  event_new("fi_status_changed_transient");

	src_handle_map = idtable_new();

	src_events[EV_SRC_ADDED]			= event_new("src_added");
	src_events[EV_SRC_REMOVED]			= event_new("src_removed");
	src_events[EV_SRC_INFO_CHANGED]		= event_new("src_info_changed");
	src_events[EV_SRC_STATUS_CHANGED]	= event_new("src_status_changed");
	src_events[EV_SRC_RANGES_CHANGED]	= event_new("src_ranges_changed");
}

/**
 * Finish initialization of fileinfo handling. This post initialization is
 * needed to avoid circular dependencies during the init phase. The listener
 * we set up here is set up in download_init, but that must be called after
 * file_info_init.
 */
void
file_info_init_post(void)
{
	/* subscribe to src events on available range updates */
	src_add_listener(fi_update_seen_on_network, EV_SRC_RANGES_CHANGED,
		FREQ_SECS, 0);
}

/*
 * Local Variables:
 * tab-width:4
 * End:
 * vi: set ts=4 sw=4 cindent:
 */
