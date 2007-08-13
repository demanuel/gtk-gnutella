/* 
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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
 * @ingroup gtk
 * @file
 *
 * GUI filtering functions.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gtk/gui.h"

RCSID("$Id$")

#include "search_cb.h"

#include "gtk/columns.h"
#include "gtk/drag.h"
#include "gtk/gtkcolumnchooser.h"
#include "gtk/misc.h"
#include "gtk/notebooks.h"
#include "gtk/search.h"
#include "gtk/settings.h"
#include "gtk/statusbar.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/gnet_property.h"
#include "if/bridge/ui2c.h"
#include "if/core/sockets.h"

#include "lib/cq.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/utf8.h"
#include "lib/vendors.h"
#include "lib/override.h"		/* Must be the last header included */

/**
 *	When a search string is entered, activate the search button
 */
void
on_entry_search_changed(GtkEditable *editable, gpointer unused_udata)
{
	gchar *s = STRTRACK(gtk_editable_get_chars(editable, 0, -1));
	gchar *normalized;
	gboolean changed;

	(void) unused_udata;

	/* Gimmick: Normalize the input on the fly because Gtk+ currently
	 * renders them differently (for example decomposed) if they're are
	 * not in Normalization Form Canonic (NFC)
	 */
	normalized = utf8_normalize(s, UNI_NORM_GUI);
	changed = normalized != s && 0 != strcmp(s, normalized);
	
	if (changed) {
		gtk_entry_set_text(GTK_ENTRY(editable), normalized);
	}
	if (normalized != s) {
		G_FREE_NULL(normalized);
	}
	if (!changed) {
		g_strstrip(s);
		gtk_widget_set_sensitive(gui_main_window_lookup("button_search"),
			s[0] != '\0');
	}
	G_FREE_NULL(s);

    gui_prop_set_boolean_val(PROP_SEARCHBAR_VISIBLE, TRUE);
}

void
on_tree_view_search_cursor_changed(GtkTreeView *tv, gpointer unused_udata)
{
	GtkTreePath *path = NULL;

	(void) unused_udata;

	gtk_tree_view_get_cursor(tv, &path, NULL);
	if (path) {
		GtkTreeModel *model;
		GtkTreeIter iter;

		model = gtk_tree_view_get_model(tv);
		if (gtk_tree_model_get_iter(model, &iter, path)) {
			gpointer ptr = NULL;

			gtk_tree_model_get(model, &iter, c_sl_sch, &ptr, (-1));
			if (ptr) {
				search_t *sch = ptr;

				search_gui_set_current_search(sch);
				main_gui_notebook_set_page(nb_main_page_search);
			}
		}
		gtk_tree_path_free(path);
	}
}

void
on_tree_view_search_results_click_column(GtkTreeViewColumn *column,
	gpointer udata)
{
	GtkTreeSortable *model;
	search_t *search = udata;
	GtkSortType order;
	gint sort_col;

	/* The default treeview is empty */
	if (!search)
		return;

	model = GTK_TREE_SORTABLE(
				gtk_tree_view_get_model(GTK_TREE_VIEW(column->tree_view)));

	/*
	 * Here we enforce a tri-state sorting. Normally, Gtk+ would only
	 * switch between ascending and descending but never switch back
	 * to the unsorted state.
	 *
	 * 			+--> sort ascending -> sort descending -> unsorted -+
     *      	|                                                   |
     *      	+-----------------------<---------------------------+
     */

	/*
	 * "order" is set to the current sort-order, not the previous one
	 * i.e., Gtk+ has already changed the order
	 */
	g_object_get(G_OBJECT(column), "sort-order", &order, (void *) 0);

	gtk_tree_sortable_get_sort_column_id(model, &sort_col, NULL);

	/* If the user switched to another sort column, reset the sort order. */
	if (search->sort_col != sort_col) {
		guint32 rank = 0;

		search->sort_order = SORT_NONE;
		/*
		 * Iterate over all rows and record their current rank/position so
	 	 * that re-sorting is stable.
		 */
		gtk_tree_model_foreach(GTK_TREE_MODEL(model),
			search_gui_update_rank, &rank);
	}

	search->sort_col = sort_col;

	/* The search has to keep state about the sort order itself because
	 * Gtk+ knows only ASCENDING/DESCENDING but not NONE (unsorted). */
	switch (search->sort_order) {
	case SORT_NONE:
	case SORT_NO_COL:
		search->sort_order = SORT_ASC;
		break;
	case SORT_ASC:
		search->sort_order = SORT_DESC;
		break;
	case SORT_DESC:
		search->sort_order = SORT_NONE;
		break;
	}

	if (SORT_NONE == search->sort_order) {
		/*
		 * Reset the sorting and let the arrow disappear from the
		 * header. Gtk+ actually seems to change the order of the
		 * rows back to the original order (i.e., chronological).
		 */
#if GTK_CHECK_VERSION(2,6,0)
		gtk_tree_sortable_set_sort_column_id(model,
			GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, order);
#endif /* Gtk+ >= 2.6.0 */
		gtk_tree_view_column_set_sort_indicator(column, FALSE);
	} else {
		/*
		 * Enforce the order as decided from the search state. Gtk+
		 * might disagree but it'll do as told.
		 */
		gtk_tree_sortable_set_sort_column_id(model, sort_col,
			SORT_ASC == search->sort_order
				? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
		gtk_tree_view_column_set_sort_indicator(column, TRUE);
	}
	/* Make the column stays clickable. */
	gtk_tree_view_column_set_clickable(column, TRUE);
}

void
search_update_tooltip(GtkTreeView *tv, GtkTreePath *path)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	const record_t *rc;

	g_return_if_fail(tv);

	if (path) {
		model = gtk_tree_view_get_model(tv);
		if (!gtk_tree_model_get_iter(model, &iter, path)) {
			g_warning("gtk_tree_model_get_iter() failed");
			return;
		}

		rc = search_gui_get_record_at_path(tv, path);
	} else {
		rc = NULL;
	}

	if (!rc) {
		GtkWidget *w;

		gtk_tooltips_set_tip(settings_gui_tooltips(), GTK_WIDGET(tv),
			_("Move the cursor over a row to see details."), NULL);
		w = settings_gui_tooltips()->tip_window;
	} else {
		gchar text[4096];

		gm_snprintf(text, sizeof text,
			"%s %s\n"
			"%s %s (%s)\n"
			"%s %.64s\n"
			"%s %s",
			_("Peer:"),
			host_addr_port_to_string(rc->results_set->addr,
				rc->results_set->port),
			_("Country:"),
			iso3166_country_name(rc->results_set->country),
			iso3166_country_cc(rc->results_set->country),
			_("Vendor:"),
			search_gui_get_vendor(rc->results_set),
			_("Size:"),
			short_size(rc->size, show_metric_units()));

		gtk_tooltips_set_tip(settings_gui_tooltips(), GTK_WIDGET(tv),
			text, NULL);
	}
}

char *
search_gui_details_get_text(GtkWidget *widget)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_val_if_fail(widget, NULL);

	if (drag_get_iter(GTK_TREE_VIEW(widget), &model, &iter)) {
		static const GValue zero_value;
		GValue value;

		value = zero_value;
		gtk_tree_model_get_value(model, &iter, 1, &value);
		return g_strdup(g_value_get_string(&value));
	} else {
		return NULL;
	}
}

/* Display XML data from the result if any */
static void
search_set_xml_metadata(const record_t *rc)
{
	GtkTextBuffer *txt;
	gchar *xml_txt;
		
	txt = gtk_text_view_get_buffer(GTK_TEXT_VIEW(
					gui_main_window_lookup("textview_result_info_xml")));
	
	/*
	 * Character set detection usually fails here because XML
	 * is mostly ASCII so that the thresholds are not reached.
	 */
	if (rc->xml) {
		gchar *s = unknown_to_utf8_normalized(rc->xml, UNI_NORM_GUI, NULL);
		xml_txt = search_xml_indent(s);
		if (rc->xml != s) {
			G_FREE_NULL(s);
		}
	} else {
		xml_txt = NULL;
	}
	gtk_text_buffer_set_text(txt, EMPTY_STRING(xml_txt), -1);
	G_FREE_NULL(xml_txt);
}

static GtkTreeView *treeview_search_details;

void
search_gui_clear_details(void)
{
	GtkTreeModel *model;

	g_return_if_fail(treeview_search_details);

	model = gtk_tree_view_get_model(treeview_search_details);
	gtk_list_store_clear(GTK_LIST_STORE(model));
}

void
search_gui_append_detail(const gchar *title, const gchar *value)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_if_fail(treeview_search_details);

	model = gtk_tree_view_get_model(treeview_search_details);
	gtk_list_store_append(GTK_LIST_STORE(model), &iter);
	gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, title, 1, value, (-1));
}

static void
search_gui_refresh_details(const record_t *rc)
{
	if (NULL == treeview_search_details) {
		static const gchar name[] = "treeview_search_details";
		treeview_search_details = GTK_TREE_VIEW(gui_main_window_lookup(name));
	}
	g_return_if_fail(treeview_search_details);

	g_object_freeze_notify(G_OBJECT(treeview_search_details));
	search_gui_set_details(rc);
	g_object_thaw_notify(G_OBJECT(treeview_search_details));
	search_set_xml_metadata(rc);
}

static void
search_update_details(GtkTreeView *tv, GtkTreePath *path)
{
	g_assert(tv);
	g_assert(path);
	
	search_gui_refresh_details(search_gui_get_record_at_path(tv, path));
}

static cevent_t *row_selected_ev;

#define ROW_SELECT_TIMEOUT	100 /* milliseconds */

static void
row_selected_expire(cqueue_t *unused_cq, gpointer unused_udata)
{
	GtkTreePath *path = NULL;
	GtkTreeView *tv;
	search_t *search;

	(void) unused_cq;
	(void) unused_udata;

	row_selected_ev = NULL;
    search = search_gui_get_current_search();

	if (search) {
		tv = GTK_TREE_VIEW(search->tree);
		gtk_tree_view_get_cursor(tv, &path, NULL);
	} else {
		tv = NULL;
		path = NULL;
	}
	if (path) {
		search_update_tooltip(tv, path);
		search_update_details(tv, path);
		gtk_tree_path_free(path);
       	search_gui_refresh_popup();
	} else {
		search_gui_clear_details();
	}
}

/**
 * This function is called when the user selectes a row in the
 * search results pane. Autoselection takes place here.
 */
void
on_tree_view_search_results_select_row(GtkTreeView *unused_tv,
	gpointer unused_udata)
{
	(void) unused_tv;
	(void) unused_udata;

	if (row_selected_ev) {
		cq_resched(callout_queue, row_selected_ev, ROW_SELECT_TIMEOUT);
	} else {
		row_selected_ev = cq_insert(callout_queue, ROW_SELECT_TIMEOUT,
							row_selected_expire, NULL);
	}
}



/***
 *** Search results popup
 ***/

void
on_popup_search_drop_name_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_name_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_name_rule, search->filter);
    g_slist_free(sl);
}

void
on_popup_search_drop_sha1_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_sha1_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_sha1_rule, search->filter);
    g_slist_free(sl);
}

void
on_popup_search_drop_host_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_host_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_host_rule, search->filter);
    g_slist_free(sl);
}

void
on_popup_search_drop_name_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_name_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_name_rule,
        filter_get_global_pre());
    g_slist_free(sl);
}

void
on_popup_search_drop_sha1_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_sha1_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_sha1_rule,
        filter_get_global_pre());
    g_slist_free(sl);
}

void
on_popup_search_drop_host_global_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
    search_t *search;
	GtkTreeSelection *selection;
    GSList *sl;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
    sl = tree_selection_collect_data(selection,
			search_gui_get_record, gui_record_host_eq);
    g_slist_foreach(sl, (GFunc) filter_add_drop_host_rule,
        filter_get_global_pre());
    g_slist_free(sl);
}

void
on_popup_search_config_cols_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	GtkWidget * cc;
    search_t *search;

	(void) unused_menuitem;
	(void) unused_udata;

    search = search_gui_get_current_search();
	g_return_if_fail(search);

	cc = gtk_column_chooser_new(GTK_WIDGET(search->tree));
   	gtk_menu_popup(GTK_MENU(cc), NULL, NULL, NULL, NULL, 1,
		gtk_get_current_event_time());

	/* GtkColumnChooser takes care of cleaning up itself */
}

/**
 * Queue a bitzi query.
 */
void
on_popup_search_metadata_activate(GtkMenuItem *unused_menuitem,
	gpointer unused_udata)
{
	guint32 bitzi_debug;

	(void) unused_menuitem;
	(void) unused_udata;

    gnet_prop_get_guint32_val(PROP_BITZI_DEBUG, &bitzi_debug);
	if (bitzi_debug)
		g_message("on_search_meta_data_active: called");

	search_gui_request_bitzi_data();
}

static void
search_gui_browse_selected_helper(gpointer data, gpointer unused_udata)
{
	const record_t *rc = data;
	guint32 flags = 0;

	(void) unused_udata;
	
	flags |= (rc->results_set->status & ST_FIREWALL) ? SOCK_F_PUSH : 0;
	flags |= (rc->results_set->status & ST_TLS) ? SOCK_F_TLS : 0;
	
	search_gui_new_browse_host(
		rc->results_set->hostname,
		rc->results_set->addr,
		rc->results_set->port,
		rc->results_set->guid,
		rc->results_set->proxies,
		flags);
}

/**
 * Request host browsing for the selected host.
 */
void
search_gui_browse_selected(void)
{
	search_t *search;

	search = search_gui_get_current_search();
	if (search) {
    	GSList *sl;

		sl = tree_selection_collect_data(
				gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree)),
				search_gui_get_record,
				gui_record_host_eq);
		g_slist_foreach(sl, search_gui_browse_selected_helper, NULL);
		g_slist_free(sl);
	}
}

void
on_popup_search_copy_magnet_activate(GtkMenuItem *unused_item,
	gpointer unused_udata)
{
	search_t *search;
	GtkTreeView *tv;
	GtkTreeIter iter;
	GtkTreePath *path = NULL;
	GtkTreeModel *model;

	(void) unused_item;
	(void) unused_udata;

	search = search_gui_get_current_search();
	g_return_if_fail(search);

	tv = GTK_TREE_VIEW(search->tree);
	gtk_tree_view_get_cursor(tv, &path, NULL);
	g_return_if_fail(path);

	model = gtk_tree_view_get_model(tv);
	if (gtk_tree_model_get_iter(model, &iter, path)) {
		gchar *url;

		url = search_gui_get_magnet(search,
					search_gui_get_record(model, &iter));
		clipboard_set_text(GTK_WIDGET(tv), url);
		G_FREE_NULL(url);
	}
	gtk_tree_path_free(path);
}


void
search_gui_callbacks_shutdown(void)
{
	/*
 	 *	Remove delayed callbacks
 	 */
	cq_cancel(callout_queue, &row_selected_ev);
}

/* -*- mode: cc-mode; tab-width:4; -*- */
/* vi: set ts=4 sw=4 cindent: */
