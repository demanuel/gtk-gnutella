/*
 * $Id$
 *
 * Copyright (c) 2004, Christian Biere
 *
 * Drop support - no dragging, just dropping.
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

#include "gui.h"

#ifdef USE_GTK2

RCSID("$Id$");

#include "drop.h"
#include "statusbar_gui.h"
#include "downloads.h"
#include "search_gui.h"
#include "override.h"		/* Must be the last header included */

/*
 * Private prototypes;
 */

static gboolean handle_not_implemented(gchar *url);
static gboolean handle_magnet(gchar *url);

/*
 * Private data
 */

static GtkClipboard *cb;

static const struct {
	const char * const proto;
	gboolean (* handler)(gchar *url);	
} proto_handlers[] = {
	{ "http",	handle_not_implemented },
	{ "ftp",	handle_not_implemented },
	{ "magnet",	handle_magnet },
};

/*
 * Private functions
 */

static gboolean handle_not_implemented(gchar *url)
{
	statusbar_gui_warning(10,
			_("Support for this protocol is not yet implemented"));
	return FALSE;
}

static void plus_to_space(gchar *s)
{
	gint c;
	gchar *p;
	
	for (p = s; (c = *p) != '\0'; p++)
		if (c == '+')
			*p = ' ';
}

static gboolean handle_magnet(gchar *url)
{
	gchar *p, *q, *next;
	struct {
		gboolean ready;
		gchar *file;
		guint32 ip;
		guint16 port;
		gchar *hostname;
		gchar *sha1;
	} dl = { FALSE, NULL, 0, 0, NULL, NULL };

	p = strchr(url, ':');
	g_assert(p);
	p++;

	if (*p != '?') {
		g_message("Invalid MAGNET URL");
		return FALSE;
	}
	p++;
	
	for (/* NOTHING */; p; p = next) {
		const gchar *name;
		
		q = strchr(p, '=');
		if (!q || p == q) {
			g_message("Invalid MAGNET URL");
			return FALSE;
		}
		name = p;
		g_assert((ssize_t) (q - p) > 0);
		
		*q++ = '\0';	/* Overwrite '=' and skip to next character */
		next = strchr(q, '&');
		if (next) {
			*next++ = '\0';
			if (*next == '\0')
				next = NULL;
		}
		plus_to_space(q);
		if (!url_unescape(q, TRUE)) {
			g_message("Invalidly encoded MAGNET URL");
			return FALSE;
		}

		/* q points to the value; p is free to use */

		if (0 == strcmp(name, "dn")) {
			/* Descriptive Name */
			dl.file = q;
		} else if (0 == strcmp(name, "xs")) {
			/* eXact Source */
			static const char n2r_query[] = "/uri-res/N2R?";
			gchar *hash;
			gchar digest[SHA1_RAW_SIZE];
			guint32 addr;
			guint16 port;
			gchar *hostname = NULL;
			gchar *ep;

			/* XXX: This should be handled elsewhere e.g., downloads.c in
			 *		a generic way. */
			
			if (dl.ready) {
				/* TODO:
				 *			Alternatives sources should be used
				 */
				g_message("More than one source; skipping");
				continue;
			}
			
			if (0 != strncmp("http://", q, sizeof "http://" - 1)) {
				statusbar_gui_warning(10, _("MAGNET URL contained source URL "
					"for an unsupported protocol"));
				/* Skip this parameter */
				continue;
			}

			p = q + sizeof "http://" - 1;

			if (gchar_to_ip_strict(p, &addr, (const gchar **) &ep)) {
				p = ep;
			} else {
				hostname = p;
				p = strchr(p, ':');
				if (!p)
					p = strchr(p, ':');
				if (!p) {
					g_message("Missing slash in URL");
					continue;
				}
			}

			if (*p == ':') {
				gchar *ep2;
				gint error;
				gulong v;
					
				*p++ = '\0'; /* Terminate hostname */
				v = gm_atoul(p, &ep2, &error);
				if (error || v >= 65535) {
					g_message("TCP port is out of range");
					/* Skip this parameter */
					continue;
				}
				
				port = v;
				p = ep2;	
			} else {
				port = 80;
			}
			
			if (*p != '/') {
				g_message("Expected port followed by '/'");
				/* Skip this parameter */
				continue;
			}
			g_assert(*p == '/');
			
			if (0 != strncmp(p, n2r_query, sizeof n2r_query - 1)) {
				/* TODO:
				 *			Support e.g., "http://example.com/example.txt"
				 */
				g_message("Arbitrary HTTP URLs are not supported yet");
				continue;
			}

			*p = '\0'; /* terminate hostname */
			p += sizeof n2r_query - 1;
			if (0 != strncmp(p, "urn:sha1:", sizeof "urn:sha1:" - 1)) {
				g_message("Expected ``urn:sha1:''");
				continue;
			}
			
			p += sizeof "urn:sha1:" - 1;
			if (strlen(p) != SHA1_BASE32_SIZE) {
				g_message("Expected SHA1");
				continue;
			}
			
			hash = p;
			if (!huge_http_sha1_extract32(hash, digest)) {
				g_message("Bad SHA1 in MAGNET URL");
				continue;
			}

			dl.ip = addr;
			dl.port = port;
			dl.sha1 = digest;
			dl.ready = TRUE;
			dl.hostname = hostname;
			if (!dl.file)
				dl.file = hash;
		} else if (0 == strcmp(name, "xt")) {
			/* eXact Topic search (by urn:sha1) */
			if (0 != strncmp("urn:sha1:", q, sizeof "urn:sha1:" - 1)) {
				statusbar_gui_warning(10, _("MAGNET URL contained exact topic "
					"search other than urn:sha1:"));
				/* Skip this parameter */
				continue;
			}
			search_gui_new_search(q, 0, NULL);
		} else if (0 == strcmp(name, "kt")) {
			/* Keyword Topic search */
			search_gui_new_search(q, 0, NULL);
		} else {
			g_message("Unhandled parameter in MAGNET URL \"%s\"", name);
		}

	}
	
	if (dl.ready) {
		gchar *filename;
		
		g_message("file=\"%s\"", dl.file);

		filename = gm_sanitize_filename(dl.file);
		download_new_unknown_size(filename, URN_INDEX, dl.ip, dl.port, 
			blank_guid, dl.hostname, dl.sha1, time(NULL), FALSE, NULL, NULL);
		if (filename != dl.file)
			G_FREE_NULL(filename);
	}

	return TRUE;
}


static void drag_data_received(GtkWidget *widget, GdkDragContext *dc,
	gint x, gint y, GtkSelectionData *data, guint info, guint stamp,
	gpointer user_data)
{
	gboolean succ = FALSE;

	if (gui_debug > 0)
		g_message("%s: x=%d, y=%d, info=%u, t=%u", __func__, x, y, info, stamp);
	if (data->length > 0 && data->format == 8) {
		guint i;
		gchar *p, *url = (gchar *) data->data;
		size_t len;
		
		if (gui_debug > 0)
			g_message("%s: url=\"%s\"", __func__, url);


		p = strchr(url, ':');
		len = p ? p - url : 0;
		if (!p || (ssize_t) len < 1) {
			statusbar_gui_warning(10, _("Cannot handle the dropped data"));
			goto cleanup;
		}
		
		for (i = 0; i < G_N_ELEMENTS(proto_handlers); i++)
			if (0 == strncmp(proto_handlers[i].proto, url, len)) {
				succ = proto_handlers[i].handler(url);
				break;
			}

		if (i == G_N_ELEMENTS(proto_handlers))
			statusbar_gui_warning(10, _("Protocol is not supported"));
	}

cleanup:
	
	gtk_drag_finish(dc, succ, FALSE, stamp);
}

/*
 * Public functions
 */

void drop_init(void)
{
	static const GtkTargetEntry targets[] = {
		{ "STRING",		0, 23 },
		{ "text/plain", 0, 23 },
	};
	GtkWidget *w = GTK_WIDGET(main_window);
	
	g_return_if_fail(!cb);
	cb = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
	g_return_if_fail(cb);
	
	g_signal_connect(G_OBJECT(w), "drag-data-received",
		G_CALLBACK(drag_data_received), NULL);

	gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, targets,
		G_N_ELEMENTS(targets), GDK_ACTION_COPY | GDK_ACTION_MOVE);
	gtk_drag_dest_set_target_list(w, gtk_target_list_new(targets,
		G_N_ELEMENTS(targets)));
}

void drop_close(void)
{
	/* Nothing ATM */
}
#endif /* USE_GTK2 */

#ifdef USE_GTK1
void drop_init(void)
{
	/* NOT IMPLEMENTED */
}

void drop_close(void)
{
	/* NOT IMPLEMENTED */
}
#endif

/* vi: set ts=4 sw=4 cindent: */
