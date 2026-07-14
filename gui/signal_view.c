/**
 * @file signal_view.c
 * @brief Signal Analysis tab — live DBC-decoded individual-signal table.
 *
 * @details
 * Implements a Vector-CANalyzer-style signal view: a CAN database (DBC) is
 * loaded, every signal it defines becomes a row, and as frames arrive each
 * matching signal's raw and physical values update live.  The decoding itself
 * lives in the GTK-independent `driver/dbc.c`; this file owns the active
 * database (@ref s_db), builds the `GtkTreeView`, and bridges incoming
 * @ref can_msg_t frames to row updates on the GTK main thread.
 *
 * Layout of one Signal Analysis page:
 * @verbatim
 *   GtkBox (vertical)
 *   ├─ toolbar  (Load DBC… / Reload / Clear  +  loaded-file label)
 *   └─ scrolled GtkTreeView   (Message | ID | Signal | Raw | Value | Unit | …)
 * @endverbatim
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"
#include "../inc/dbc.h"

/** @brief Resolve a bundled asset path (defined in main_window.c). */
extern const char *gui_find_asset(const char *name);

/** @brief The active CAN database, or NULL when none is loaded. */
static dbc_db_t *s_db = NULL;

/**
 * @brief Column indices of the Signal Analysis `GtkListStore`.
 */
enum {
    SCOL_MSG = 0,   /**< Message name.                                        */
    SCOL_ID,        /**< Formatted CAN identifier.                            */
    SCOL_SIGNAL,    /**< Signal name.                                         */
    SCOL_RAW,       /**< Raw (unscaled) value.                                */
    SCOL_VALUE,     /**< Physical value.                                      */
    SCOL_UNIT,      /**< Engineering unit.                                    */
    SCOL_RANGE,     /**< Documented min..max.                                 */
    SCOL_COUNT,     /**< Update hit count.                                    */
    SCOL_FG,        /**< Row foreground (NULL until first update).            */
    SCOL_MSGID,     /**< Hidden: raw CAN id for matching.                     */
    SCOL_EXT,       /**< Hidden: extended-ID flag for matching.              */
    SCOL_SIGIDX,    /**< Hidden: signal index within its message.            */
    SCOL_NUM        /**< Model width.                                         */
};

/**
 * @brief Format a physical/raw double compactly (trim trailing zeros).
 * @param buf Output. @param sz Size. @param v Value.
 */
static void fmt_double(char *buf, size_t sz, double v)
{
    snprintf(buf, sz, "%.4f", v);
    /* Trim trailing zeros and a dangling decimal point. */
    char *dot = strchr(buf, '.');
    if (!dot)
        return;
    char *end = buf + strlen(buf) - 1;
    while (end > dot && *end == '0')
        *end-- = '\0';
    if (end == dot)
        *end = '\0';
}

/* ------------------------------------------------------------------ */
/* Table (re)population                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Rebuild the signal table from the active database (@ref s_db).
 */
static void sig_table_rebuild(void)
{
    if (!g_gui.sig_store)
        return;

    gtk_list_store_clear(g_gui.sig_store);
    if (!s_db)
        return;

    for (size_t mi = 0; mi < s_db->message_count; mi++) {
        const dbc_message_t *m = &s_db->messages[mi];
        char id_buf[16];
        gui_format_id(id_buf, sizeof(id_buf), m->id, m->is_extended);

        for (uint16_t si = 0; si < m->signal_count; si++) {
            const dbc_signal_t *s = &m->signals[si];
            char range[80], minb[32], maxb[32];
            fmt_double(minb, sizeof(minb), s->min);
            fmt_double(maxb, sizeof(maxb), s->max);
            snprintf(range, sizeof(range), "%s .. %s", minb, maxb);

            GtkTreeIter it;
            gtk_list_store_append(g_gui.sig_store, &it);
            gtk_list_store_set(g_gui.sig_store, &it,
                SCOL_MSG,    m->name,
                SCOL_ID,     id_buf,
                SCOL_SIGNAL, s->name,
                SCOL_RAW,    "—",
                SCOL_VALUE,  "—",
                SCOL_UNIT,   s->unit,
                SCOL_RANGE,  range,
                SCOL_COUNT,  0u,
                SCOL_FG,     NULL,
                SCOL_MSGID,  m->id,
                SCOL_EXT,    (gboolean)m->is_extended,
                SCOL_SIGIDX, (guint)si,
                -1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Database load / clear                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Refresh the "loaded database" status label.
 */
static void sig_update_dbc_label(void)
{
    if (!g_gui.lbl_dbc)
        return;
    char markup[640];
    if (s_db) {
        const char *base = strrchr(s_db->path, '/');
        base = base ? base + 1 : s_db->path;
        snprintf(markup, sizeof(markup),
                 "<b>Database:</b> %s  (%zu messages, %zu signals)",
                 base, s_db->message_count, s_db->signal_count);
    } else {
        snprintf(markup, sizeof(markup),
                 "<b>Database:</b> <i>none loaded — use “Load DBC…”.</i>");
    }
    gtk_label_set_markup(GTK_LABEL(g_gui.lbl_dbc), markup);
}

gboolean gui_signal_load_dbc(const char *path)
{
    if (!path || !*path)
        return FALSE;

    char err[256] = {0};
    dbc_db_t *db = dbc_load_file(path, err, sizeof(err));
    if (!db) {
        gui_status_message("DBC load failed: %s", err);
        return FALSE;
    }

    if (s_db)
        dbc_free(s_db);
    s_db = db;

    sig_table_rebuild();
    gui_plot_set_database(s_db);
    gui_math_set_database(s_db);
    sig_update_dbc_label();
    gui_status_message("Loaded database %s (%zu messages, %zu signals).",
                       db->path, db->message_count, db->signal_count);
    return TRUE;
}

void gui_signal_clear_dbc(void)
{
    if (s_db) {
        dbc_free(s_db);
        s_db = NULL;
    }
    sig_table_rebuild();
    gui_plot_set_database(NULL);
    gui_math_set_database(NULL);
    sig_update_dbc_label();
    gui_status_message("Database unloaded.");
}

void gui_signal_load_default_dbc(void)
{
    const char *p = gui_find_asset("demo.dbc");
    if (p && access(p, R_OK) == 0)
        gui_signal_load_dbc(p);
    else
        sig_update_dbc_label();
}

const char *gui_signal_get_dbc_path(void)
{
    return s_db && s_db->path[0] ? s_db->path : NULL;
}

/* ------------------------------------------------------------------ */
/* Live decode                                                          */
/* ------------------------------------------------------------------ */

void gui_signal_decode_message(const can_msg_t *msg)
{
    if (!s_db || !g_gui.sig_store || !msg || msg->is_error)
        return;

    const dbc_message_t *m = dbc_find_message(s_db, msg->id, msg->is_extended);
    if (!m)
        return;

    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_gui.sig_store), &it))
        return;

    do {
        guint    row_id  = 0, sig_idx = 0, cnt = 0;
        gboolean row_ext = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(g_gui.sig_store), &it,
                           SCOL_MSGID,  &row_id,
                           SCOL_EXT,    &row_ext,
                           SCOL_SIGIDX, &sig_idx,
                           SCOL_COUNT,  &cnt,
                           -1);

        if (row_id != m->id ||
            row_ext != (gboolean)m->is_extended ||
            sig_idx >= m->signal_count)
            continue;

        const dbc_signal_t *s = &m->signals[sig_idx];
        int64_t  sraw = 0;
        uint64_t raw  = dbc_extract_raw(msg->data, msg->dlc, s);
        double   phys = dbc_decode_physical(s, raw, &sraw);

        /* Feed the time-graph viewer with this decoded sample. */
        gui_plot_add_sample(m->id, m->is_extended, (int)sig_idx, phys);
        gui_math_add_sample(m->id, m->is_extended, (int)sig_idx, phys);

        char raw_buf[32], val_buf[40];
        snprintf(raw_buf, sizeof(raw_buf), "%lld", (long long)sraw);
        fmt_double(val_buf, sizeof(val_buf), phys);

        gtk_list_store_set(g_gui.sig_store, &it,
            SCOL_RAW,   raw_buf,
            SCOL_VALUE, val_buf,
            SCOL_COUNT, cnt + 1,
            SCOL_FG,    msg->direction == CAN_DIR_TX ? "blue" : "black",
            -1);
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_gui.sig_store), &it));
}

/* ------------------------------------------------------------------ */
/* Toolbar callbacks                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief "Load DBC…" — choose and load a database file.
 */
static void on_load_dbc(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Load CAN Database (DBC)",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);

    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ff, "*.dbc");
    gtk_file_filter_set_name(ff, "CAN database (*.dbc)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            gui_signal_load_dbc(fn);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
}

/**
 * @brief "Reload" — re-read the active database from disk.
 */
static void on_reload_dbc(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_db && s_db->path[0]) {
        char path[512];
        strncpy(path, s_db->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        gui_signal_load_dbc(path);
    } else {
        gui_signal_load_default_dbc();
    }
}

/**
 * @brief "Clear" — unload the active database.
 */
static void on_clear_dbc(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_signal_clear_dbc();
}

/* ------------------------------------------------------------------ */
/* View construction                                                   */
/* ------------------------------------------------------------------ */

GtkWidget *gui_create_signal_view(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);

    /* --- Toolbar row --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    GtkWidget *load_btn = gtk_button_new_with_label("Load DBC…");
    g_signal_connect(load_btn, "clicked", G_CALLBACK(on_load_dbc), NULL);
    gtk_box_pack_start(GTK_BOX(bar), load_btn, FALSE, FALSE, 0);

    GtkWidget *reload_btn = gtk_button_new_with_label("Reload");
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_reload_dbc), NULL);
    gtk_box_pack_start(GTK_BOX(bar), reload_btn, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_dbc), NULL);
    gtk_box_pack_start(GTK_BOX(bar), clear_btn, FALSE, FALSE, 0);

    g_gui.lbl_dbc = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_dbc), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(g_gui.lbl_dbc), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(bar), g_gui.lbl_dbc, TRUE, TRUE, 8);

    gtk_box_pack_start(GTK_BOX(box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* --- Signal table --- */
    GtkListStore *store = gtk_list_store_new(
        SCOL_NUM,
        G_TYPE_STRING,   /* MSG    */
        G_TYPE_STRING,   /* ID     */
        G_TYPE_STRING,   /* SIGNAL */
        G_TYPE_STRING,   /* RAW    */
        G_TYPE_STRING,   /* VALUE  */
        G_TYPE_STRING,   /* UNIT   */
        G_TYPE_STRING,   /* RANGE  */
        G_TYPE_UINT,     /* COUNT  */
        G_TYPE_STRING,   /* FG     */
        G_TYPE_UINT,     /* MSGID  */
        G_TYPE_BOOLEAN,  /* EXT    */
        G_TYPE_UINT      /* SIGIDX */
    );
    g_gui.sig_store = store;

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    g_gui.sig_view = tree;
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(tree),
                                 GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);

    struct { int col; const char *title; int width; gboolean expand; } cols[] = {
        { SCOL_MSG,    "Message",  150, FALSE },
        { SCOL_ID,     "ID",        90, FALSE },
        { SCOL_SIGNAL, "Signal",   170, FALSE },
        { SCOL_RAW,    "Raw",       90, FALSE },
        { SCOL_VALUE,  "Value",    110, TRUE  },
        { SCOL_UNIT,   "Unit",      70, FALSE },
        { SCOL_RANGE,  "Range",    150, FALSE },
        { SCOL_COUNT,  "Count",     70, FALSE },
    };
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        GtkCellRenderer   *rend = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes(
            cols[i].title, rend,
            "text", cols[i].col, "foreground", SCOL_FG, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_min_width(col, cols[i].width);
        gtk_tree_view_column_set_expand(col, cols[i].expand);
        gtk_tree_view_column_set_sort_column_id(col, cols[i].col);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    sig_update_dbc_label();
    return box;
}
