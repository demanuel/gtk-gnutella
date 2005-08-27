/*
 * Copyright (c) 2001-2003, Richard Eckart
 *
 * THIS FILE IS AUTOGENERATED! DO NOT EDIT!
 * This file is generated from gnet_props.ag using autogen.
 * Autogen is available at http://autogen.sourceforge.net/.
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

#ifndef _gnet_property_priv_h_
#define _gnet_property_priv_h_

#include <glib.h>

#include "lib/prop.h"

#ifdef CORE_SOURCES

/*
 * Includes specified by "uses"-statement in .ag file
 */
#include "core/sockets.h"
#include "core/bsched.h"
#include "if/core/nodes.h"


extern const gboolean reading_hostfile;
extern const gboolean reading_ultrafile;
extern const gboolean ancient_version;
extern const gchar   *new_version_str;
extern const guint32  up_connections;
extern const guint32  normal_connections;
extern const guint32  max_connections;
extern const guint32  node_leaf_count;
extern const guint32  node_normal_count;
extern const guint32  node_ultra_count;
extern const guint32  max_downloads;
extern const guint32  max_host_downloads;
extern const guint32  max_uploads;
extern const guint32  max_uploads_ip;
extern const gchar   *local_ip;
extern const guint64  current_ip_stamp;
extern const guint32  average_ip_uptime;
extern const guint64  start_stamp;
extern const guint32  average_servent_uptime;
extern const guint32  listen_port;
extern const gchar   *forced_local_ip;
extern const guint32  connection_speed;
extern const gboolean compute_connection_speed;
extern const guint32  search_max_items;
extern const guint32  ul_usage_min_percentage;
extern const guint32  download_connecting_timeout;
extern const guint32  download_push_sent_timeout;
extern const guint32  download_connected_timeout;
extern const guint32  download_retry_timeout_min;
extern const guint32  download_retry_timeout_max;
extern const guint32  download_max_retries;
extern const guint32  download_retry_timeout_delay;
extern const guint32  download_retry_busy_delay;
extern const guint32  download_retry_refused_delay;
extern const guint32  download_retry_stopped_delay;
extern const guint32  download_overlap_range;
extern const guint32  upload_connecting_timeout;
extern const guint32  upload_connected_timeout;
extern const guint32  search_reissue_timeout;
extern const guint32  ban_ratio_fds;
extern const guint32  ban_max_fds;
extern const guint32  banned_count;
extern const guint32  max_banned_fd;
extern const guint32  incoming_connecting_timeout;
extern const guint32  node_connecting_timeout;
extern const guint32  node_connected_timeout;
extern const guint32  node_sendqueue_size;
extern const guint32  node_tx_flowc_timeout;
extern const guint32  node_rx_flowc_ratio;
extern const guint32  max_ttl;
extern const guint32  my_ttl;
extern const guint32  hard_ttl_limit;
extern const guint32  dbg;
extern const guint32  http_debug;
extern const guint32  download_debug;
extern const guint32  upload_debug;
extern const guint32  lib_debug;
extern const guint32  bitzi_debug;
extern const guint32  gwc_debug;
extern const guint32  url_debug;
extern const guint32  dh_debug;
extern const guint32  dq_debug;
extern const guint32  vmsg_debug;
extern const guint32  query_debug;
extern const guint32  search_debug;
extern const guint32  udp_debug;
extern const guint32  qrp_debug;
extern const guint32  routing_debug;
extern const guint32  ggep_debug;
extern const guint32  pcache_debug;
extern const guint32  hsep_debug;
extern const guint32  tls_debug;
extern const guint32  parq_debug;
extern const guint32  track_props;
extern const gboolean stop_host_get;
extern const gboolean bws_in_enabled;
extern const gboolean bws_out_enabled;
extern const gboolean bws_gin_enabled;
extern const gboolean bws_glin_enabled;
extern const gboolean bws_gout_enabled;
extern const gboolean bws_glout_enabled;
extern const gboolean bw_ul_usage_enabled;
extern const gboolean bw_allow_stealing;
extern const gboolean clear_complete_downloads;
extern const gboolean clear_failed_downloads;
extern const gboolean clear_unavailable_downloads;
extern const gboolean search_remove_downloaded;
extern const gboolean force_local_ip;
extern const gboolean use_netmasks;
extern const gboolean allow_private_network_connection;
extern const gboolean use_ip_tos;
extern const gboolean download_delete_aborted;
extern const gboolean proxy_auth;
extern const gchar   *socks_user;
extern const gchar   *socks_pass;
extern const gchar   *proxy_addr;
extern const gchar   *proxy_hostname;
extern const guint32  proxy_port;
extern const guint32  proxy_protocol;
extern const guint32  network_protocol;
extern const guint32  hosts_in_catcher;
extern const guint32  hosts_in_ultra_catcher;
extern const guint32  hosts_in_bad_catcher;
extern const guint32  max_hosts_cached;
extern const guint32  max_ultra_hosts_cached;
extern const guint32  max_bad_hosts_cached;
extern const guint32  max_high_ttl_msg;
extern const guint32  max_high_ttl_radius;
extern const guint32  bw_http_in;
extern const guint32  bw_http_out;
extern const guint32  bw_gnet_in;
extern const guint32  bw_gnet_out;
extern const guint32  bw_gnet_lin;
extern const guint32  bw_gnet_lout;
extern const guint32  search_queries_forward_size;
extern const guint32  search_queries_kick_size;
extern const guint32  search_answers_forward_size;
extern const guint32  search_answers_kick_size;
extern const guint32  other_messages_kick_size;
extern const guint32  hops_random_factor;
extern const gboolean send_pushes;
extern const guint32  min_dup_msg;
extern const guint32  min_dup_ratio;
extern const gchar   *scan_extensions;
extern const gboolean scan_ignore_symlink_dirs;
extern const gboolean scan_ignore_symlink_regfiles;
extern const gchar   *save_file_path;
extern const gchar   *move_file_path;
extern const gchar   *bad_file_path;
extern const gchar   *shared_dirs_paths;
extern const gchar   *local_netmasks_string;
extern const guint32  total_downloads;
extern const guint32  ul_running;
extern const guint32  ul_registered;
extern const guint32  total_uploads;
extern const gchar    servent_guid[16];
extern const gboolean use_swarming;
extern const gboolean use_aggressive_swarming;
extern const guint32  dl_minchunksize;
extern const guint32  dl_maxchunksize;
extern const gboolean auto_download_identical;
extern const gboolean auto_feed_download_mesh;
extern const gboolean strict_sha1_matching;
extern const gboolean use_fuzzy_matching;
extern const guint32  fuzzy_threshold;
extern const gboolean is_firewalled;
extern const gboolean is_inet_connected;
extern const gboolean is_udp_firewalled;
extern const gboolean recv_solicited_udp;
extern const gboolean gnet_compact_query;
extern const gboolean download_optimistic_start;
extern const gboolean library_rebuilding;
extern const gboolean sha1_rebuilding;
extern const gboolean sha1_verifying;
extern const gboolean file_moving;
extern const gboolean prefer_compressed_gnet;
extern const gboolean online_mode;
extern const gboolean download_require_urn;
extern const gboolean download_require_server_name;
extern const guint32  max_ultrapeers;
extern const guint32  quick_connect_pool_size;
extern const guint32  max_leaves;
extern const guint32  search_handle_ignored_files;
extern const guint32  configured_peermode;
extern const guint32  current_peermode;
extern const guint32  sys_nofile;
extern const guint32  sys_physmem;
extern const guint32  dl_queue_count;
extern const guint32  dl_running_count;
extern const guint32  dl_active_count;
extern const guint32  dl_aqueued_count;
extern const guint32  dl_pqueued_count;
extern const guint32  fi_all_count;
extern const guint32  fi_with_source_count;
extern const guint32  dl_qalive_count;
extern const guint64  dl_byte_count;
extern const guint64  ul_byte_count;
extern const gboolean pfsp_server;
extern const guint32  pfsp_first_chunk;
extern const gboolean fuzzy_filter_dmesh;
extern const guint32  crawler_visit_count;
extern const guint32  udp_crawler_visit_count;
extern const gboolean host_runs_ntp;
extern const gboolean ntp_detected;
extern const guint32  clock_skew;
extern const gboolean node_monitor_unstable_ip;
extern const gboolean node_monitor_unstable_servents;
extern const gboolean dl_remove_file_on_mismatch;
extern const guint32  dl_mismatch_backout;
extern const gchar   *server_hostname;
extern const gboolean give_server_hostname;
extern const guint32  reserve_gtkg_nodes;
extern const guint32  unique_nodes;
extern const guint32  download_rx_size;
extern const guint32  node_rx_size;
extern const guint32  dl_http_latency;
extern const guint64  node_last_ultra_check;
extern const guint64  node_last_ultra_leaf_switch;
extern const gboolean up_req_avg_servent_uptime;
extern const gboolean up_req_avg_ip_uptime;
extern const gboolean up_req_node_uptime;
extern const gboolean up_req_not_firewalled;
extern const gboolean up_req_enough_conn;
extern const gboolean up_req_enough_fd;
extern const gboolean up_req_enough_mem;
extern const gboolean up_req_enough_bw;
extern const guint32  search_queue_size;
extern const guint32  search_queue_spacing;
extern const gboolean enable_shell;
extern const guint32  entry_removal_timeout;
extern const gboolean node_watch_similar_queries;
extern const guint32  node_queries_half_life;
extern const guint32  node_requery_threshold;
extern const guint64  library_rescan_started;
extern const guint64  library_rescan_finished;
extern const guint32  library_rescan_duration;
extern const guint64  qrp_indexing_started;
extern const guint32  qrp_indexing_duration;
extern const guint64  qrp_timestamp;
extern const guint32  qrp_computation_time;
extern const guint64  qrp_patch_timestamp;
extern const guint32  qrp_patch_computation_time;
extern const guint32  qrp_generation;
extern const guint32  qrp_slots;
extern const guint32  qrp_slots_filled;
extern const guint32  qrp_fill_ratio;
extern const guint32  qrp_conflict_ratio;
extern const guint32  qrp_hashed_keywords;
extern const guint32  qrp_patch_raw_length;
extern const guint32  qrp_patch_length;
extern const guint32  qrp_patch_comp_ratio;
extern const gchar   *ancient_version_force;
extern const guint32  ancient_version_left_days;
extern const gboolean file_descriptor_shortage;
extern const gboolean file_descriptor_runout;
extern const gboolean enable_g2_support;
extern const gboolean convert_spaces;
extern const gboolean convert_evil_chars;
extern const gboolean convert_old_filenames;
extern const gboolean tls_enforce;
extern const gboolean gnet_deflate_enabled;
extern const gboolean enable_udp;
extern const gboolean process_oob_queries;
extern const gboolean send_oob_queries;
extern const gboolean proxy_oob_queries;
extern const gboolean uploads_stalling;
extern const gboolean allow_auto_requeries;
extern const gboolean use_global_hostiles_txt;
extern const gboolean use_so_linger;
extern const gboolean browse_host_enabled;
extern const guint32  html_browse_count;
extern const guint32  qhits_browse_count;


prop_set_t *gnet_prop_init(void);
void gnet_prop_shutdown(void);

#endif /* CORE_SOURCES */

#endif /* _gnet_property_priv_h_ */

