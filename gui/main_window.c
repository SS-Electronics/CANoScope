/**
 * @file main_window.c
 * @brief Main GTK3 window: menu bar, statistics bar, toolbar, trace, TX panel.
 *
 * @details
 * Constructs and owns the entire main-window UI and most of its behaviour:
 *   - the menu bar (File, Trace, CAN, View, Help),
 *   - the always-visible statistics bar below the menu,
 *   - the toolbar,
 *   - a vertical paned area with the receive trace on top and the inline
 *     transmit panel below,
 *   - the Trace capture menu (Start/Stop/Save as CSV),
 *   - the inline transmit panel with per-row CAN FD DLC up to 64 and dynamic,
 *     wrapping data-byte fields,
 *   - Taksys branding (window icon, footer, About dialog).
 *
 * Window layout:
 * @verbatim
 *   GtkApplicationWindow
 *   └─ GtkBox (vertical)
 *      ├─ GtkMenuBar
 *      ├─ statistics bar  (GtkBox)
 *      ├─ GtkToolbar
 *      ├─ GtkNotebook
 *      │  ├─ "Receive / Transmit" page     (GtkPaned: trace + transmit panel)
 *      │  ├─ "Signal Analysis" page        (DBC-decoded signal table)
 *      │  ├─ "Signal Analysis Viewer" page (multi-signal time graph)
 *      │  ├─ "Math" page                   (two-signal realtime math graphs)
 *      │  └─ "DB Creation" page            (DBC generator/updater)
 *      ├─ GtkStatusbar
 *      └─ footer  (Taksys logo + credits)
 * @endverbatim
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"
#include "../inc/version.h"

/** @brief Trace view factory implemented in message_view.c. @return Scrolled tree view. */
extern GtkWidget *create_trace_view(void);

/* ------------------------------------------------------------------ */
/* Branding / asset loading (Taksys logo)                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Resolve a bundled asset file across dev and installed layouts.
 *
 * Searches, in order: `$CANOSCOPE_ASSET_DIR`, paths relative to the executable
 * (dev `assets/` and installed `../share/canoscope`), and the system share dirs.
 * @param name  Asset filename (e.g. "taksys_logo.png").
 * @return Static path string valid until the next call, or NULL if not found.
 */
const char *gui_find_asset(const char *name)
{
    static char buf[1024];

    const char *env = getenv("CANOSCOPE_ASSET_DIR");
    if (env && *env) {
        snprintf(buf, sizeof(buf), "%s/%s", env, name);
        if (access(buf, R_OK) == 0) return buf;
    }

    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            const char *rel[] = {
                "assets", "../assets", "../share/canoscope", NULL
            };
            for (int i = 0; rel[i]; i++) {
                snprintf(buf, sizeof(buf), "%s/%s/%s", exe, rel[i], name);
                if (access(buf, R_OK) == 0) return buf;
            }
        }
    }

    const char *fixed[] = {
        "assets",
        "/usr/local/share/canoscope",
        "/usr/share/canoscope",
        NULL
    };
    for (int i = 0; fixed[i]; i++) {
        snprintf(buf, sizeof(buf), "%s/%s", fixed[i], name);
        if (access(buf, R_OK) == 0) return buf;
    }
    return NULL;
}

/**
 * @brief Load the Taksys logo scaled into a square of @p size px.
 * @param size  Target edge length in pixels.
 * @return A new GdkPixbuf (caller unrefs) or NULL if unavailable.
 */
static GdkPixbuf *gui_load_logo(int size)
{
    const char *p = gui_find_asset("taksys_logo.png");
    if (!p) return NULL;
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(p, size, size, TRUE, &err);
    if (err) {
        g_error_free(err);
        return NULL;
    }
    return pix;
}

/* Forward-declared here; defined in the transmit-panel section below */
#define TX_PANEL_DATA_COLS CANFD_DATA_MAX   /* up to 64 byte fields for CAN FD */
#define TX_PANEL_MAX_ROWS   16
#define TX_STD_ID_MAX      0x7FFu
#define TX_EXT_ID_MAX      0x1FFFFFFFu
#define TX_BYTE_MAX        0xFFu

/* Grid column layout shared by the header row and every transmit row, so the
 * fixed fields stay aligned and resize by ratio with the window.  The variable
 * number of data-byte fields (1 byte for classic CAN … up to 64 for CAN FD)
 * lives inside a single wrapping GtkFlowBox occupying the TXG_COL_DATA cell. */
enum {
    TXG_COL_NUM = 0,
    TXG_COL_ID,
    TXG_COL_EXT,
    TXG_COL_RTR,
    TXG_COL_DLC,
    TXG_COL_DATA,           /* flow-box of 1..64 data byte entries */
    TXG_COL_INTERVAL,
    TXG_COL_SEND,
    TXG_COL_START,
    TXG_COL_STOP,
    TXG_COL_STATUS,
};

typedef struct {
    GtkWidget *num_lbl;
    GtkWidget *id_entry;
    GtkWidget *ext_check;
    GtkWidget *rtr_check;
    GtkWidget *dlc_spin;
    GtkWidget *data_box;                       /* GtkFlowBox holding the bytes */
    GtkWidget *data_entry[TX_PANEL_DATA_COLS];
    GtkWidget *interval_spin;
    GtkWidget *send_btn;
    GtkWidget *start_btn;
    GtkWidget *stop_btn;
    GtkWidget *status_lbl;
    guint      cyclic_id;
} tx_panel_row_t;

static struct {
    tx_panel_row_t rows[TX_PANEL_MAX_ROWS];
    int            count;
    GtkWidget     *rows_box;
    GtkWidget     *placeholder;
} s_txp;

static tx_panel_row_t *txp_row_from_data(gpointer data);
static void txp_set_status(tx_panel_row_t *row,
                           const char *color,
                           const char *msg);
static void txp_clear_row(tx_panel_row_t *row);
static void txp_clear_all(void);
static void txp_add_row(void);
static void txp_remove_last_row(void);
static void txp_set_error(tx_panel_row_t *row, const char *msg);
static void txp_update_data_fields(tx_panel_row_t *row);
static gboolean txp_build_frame(tx_panel_row_t *row, can_msg_t *msg);
static gboolean txp_queue_frame(int row_index, gboolean cyclic);
static void txp_stop_cyclic_row(tx_panel_row_t *row, const char *status);
static void txp_stop_all_cyclic(void);
static gboolean txp_cyclic_tick(gpointer data);
static void txp_send_once(GtkWidget *w, gpointer d);
static void txp_start_cyclic(GtkWidget *w, gpointer d);
static void txp_stop_cyclic(GtkWidget *w, gpointer d);
static void txp_dlc_changed(GtkSpinButton *btn, gpointer d);
static void txp_rtr_toggled(GtkToggleButton *btn, gpointer d);

/* ------------------------------------------------------------------ */
/* Toolbar / menu callbacks                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Window `delete-event`: stop cyclic TX and disconnect before destroy.
 * @param w Window. @param e Event. @param d Unused. @return FALSE (allow destroy).
 */
static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    /* Stop cyclic timer before disconnect to prevent callbacks on dead widgets */
    txp_stop_all_cyclic();
    app_do_disconnect();
    return FALSE; /* allow default window destruction */
}

/**
 * @brief Connect action — open the settings/connect dialog.
 */
static void on_connect(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_settings_dialog(g_gui.window);
}

/**
 * @brief Disconnect action — stop cyclic TX and disconnect.
 */
static void on_disconnect(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    txp_stop_all_cyclic();
    app_do_disconnect();
}

/**
 * @brief Clear-trace action.
 */
static void on_clear(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_clear_trace();
    gui_status_message("Trace cleared.");
}

/**
 * @brief Trace > Start — begin capturing every Rx/Tx frame in memory.
 *
 * @ref on_trace_stop halts capture (the buffer is retained); @ref on_trace_save
 * writes the captured frames to a CSV file.
 */
static void on_trace_start(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_app.connected) {
        gui_show_error(g_gui.window, "Error",
                       "Not connected – cannot start trace.");
        return;
    }
    if (g_app.tracing) return;

    pthread_mutex_lock(&g_app.trace_mutex);
    g_app.trace_len = 0;      /* discard any previous capture */
    g_app.tracing   = 1;
    pthread_mutex_unlock(&g_app.trace_mutex);

    gtk_widget_set_sensitive(g_gui.toolbar_trace_start, FALSE);
    gtk_widget_set_sensitive(g_gui.toolbar_trace_stop,  TRUE);
    gui_status_message("Trace recording started…");
}

/**
 * @brief Trace > Stop — halt capture (buffer retained for export).
 */
static void on_trace_stop(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_app.tracing) return;

    g_app.tracing = 0;

    gtk_widget_set_sensitive(g_gui.toolbar_trace_start, TRUE);
    gtk_widget_set_sensitive(g_gui.toolbar_trace_stop,  FALSE);
    gui_status_message("Trace stopped – %zu frame(s) captured. "
                       "Use Trace > Save to export CSV.", g_app.trace_len);
}

/**
 * @brief Write one captured frame as a CSV record.
 * @param fp   Open output file.
 * @param msg  Frame to serialise.
 */
static void trace_csv_write_row(FILE *fp, const can_msg_t *msg)
{
    char id_buf[16];
    char data_buf[3 * CANFD_DATA_MAX + 4];

    if (msg->is_extended)
        snprintf(id_buf, sizeof(id_buf), "%08X", msg->id);
    else
        snprintf(id_buf, sizeof(id_buf), "%03X", msg->id);

    if (msg->dlc == 0) {
        snprintf(data_buf, sizeof(data_buf), "-");
    } else {
        size_t pos = 0;
        for (uint8_t i = 0; i < msg->dlc && i < CANFD_DATA_MAX &&
                            pos + 4 < sizeof(data_buf); i++) {
            pos += (size_t)snprintf(data_buf + pos, sizeof(data_buf) - pos,
                                    i ? " %02X" : "%02X", msg->data[i]);
        }
    }

    double ts = (double)msg->timestamp.tv_sec
              + (double)msg->timestamp.tv_nsec / 1e9;

    fprintf(fp, "%llu,%.6f,%s,%s,%s,%u,%s\n",
            (unsigned long long)msg->seq,
            ts,
            msg->direction == CAN_DIR_TX ? "Tx" : "Rx",
            id_buf,
            gui_msg_type_str(msg),
            msg->dlc,
            data_buf);
}

/**
 * @brief Trace > Save as CSV — export the captured buffer to a file.
 */
static void on_trace_save(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    if (g_app.trace_len == 0) {
        gui_show_error(g_gui.window, "Save Trace",
                       "No trace data captured yet.\n"
                       "Use Trace > Start, generate some traffic, then Stop.");
        return;
    }

    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Save Trace as CSV",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "trace.csv");

    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ff, "*.csv");
    gtk_file_filter_set_name(ff, "CSV trace (*.csv)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        strncpy(g_app.trace_filename, fn, APP_TRACE_FILE_LEN - 1);
        g_app.trace_filename[APP_TRACE_FILE_LEN - 1] = '\0';
        g_free(fn);

        FILE *fp = fopen(g_app.trace_filename, "w");
        if (fp) {
            fprintf(fp, "seq,timestamp,direction,id,type,dlc,data\n");
            pthread_mutex_lock(&g_app.trace_mutex);
            size_t n = g_app.trace_len;
            for (size_t i = 0; i < n; i++)
                trace_csv_write_row(fp, &g_app.trace_buf[i]);
            pthread_mutex_unlock(&g_app.trace_mutex);
            fclose(fp);
            gui_status_message("Saved %zu frame(s) to %s",
                               n, g_app.trace_filename);
        } else {
            gui_show_error(g_gui.window, "Trace Error",
                           "Could not open file for writing.");
        }
    }
    gtk_widget_destroy(dlg);
}

/**
 * @brief Open the advanced transmit window.
 */
static void on_transmit(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_transmit_window();
}

/**
 * @brief Open the connection-settings dialog.
 */
static void on_settings(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_settings_dialog(g_gui.window);
}

/**
 * @brief Database > Load — choose and load a DBC for signal analysis.
 */
static void on_db_load(GtkWidget *w, gpointer d)
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
 * @brief Database > Load Demo — load the bundled demo database.
 */
static void on_db_demo(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_signal_load_default_dbc();
}

/**
 * @brief Database > Clear — unload the active database.
 */
static void on_db_clear(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_signal_clear_dbc();
}

/**
 * @brief Open the About dialog.
 */
static void on_about(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gui_show_about_dialog(g_gui.window);
}

/**
 * @brief Quit action — tear down and destroy the main window.
 */
static void on_quit(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    txp_stop_all_cyclic();
    app_do_disconnect();
    gtk_widget_destroy(g_gui.window);
}

/**
 * @brief View > Roll Up Tx — toggle TX deduplication.
 */
static void on_dedup_toggle(GtkCheckMenuItem *item, gpointer d)
{
    (void)d;
    g_app.dedup_mode = gtk_check_menu_item_get_active(item);
    gui_clear_trace();
    gui_status_message("Tx roll-up: %s",
                       g_app.dedup_mode ? "ON" : "OFF");
}

/**
 * @brief View > ID Format — switch ID display radix.
 */
static void on_id_format_toggle(GtkCheckMenuItem *item, gpointer d)
{
    int fmt = GPOINTER_TO_INT(d);
    if (!gtk_check_menu_item_get_active(item))
        return;

    g_app.id_format = fmt;
    gui_refresh_trace_display();
    gui_status_message("ID display format: %s",
        fmt == APP_ID_FORMAT_DEC ? "decimal" : "hexadecimal");
}

/**
 * @brief View > Data Format — switch payload display format.
 */
static void on_data_format_toggle(GtkCheckMenuItem *item, gpointer d)
{
    int fmt = GPOINTER_TO_INT(d);
    if (!gtk_check_menu_item_get_active(item))
        return;

    g_app.data_format = fmt;
    gui_refresh_trace_display();
    gui_status_message("Data display format: %s",
        fmt == APP_DATA_FORMAT_ASCII ? "ASCII" :
        fmt == APP_DATA_FORMAT_DEC ? "decimal" : "hexadecimal");
}

/* ------------------------------------------------------------------ */
/* Menu helpers                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a mnemonic menu item wired to a callback.
 * @param label Mnemonic label. @param cb Activate callback. @param data User data.
 * @return The menu item.
 */
static GtkWidget *menu_item(const char *label,
                             GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    if (cb) g_signal_connect(item, "activate", cb, data);
    return item;
}

/**
 * @brief Add one "caption: value" cell to the statistics bar.
 * @param bar      The statistics bar box.
 * @param caption  Bold caption text.
 * @param initial  Initial value text.
 * @return The value label, so @ref gui_update_stats can refresh it.
 */
static GtkWidget *stats_bar_field(GtkWidget *bar, const char *caption,
                                  const char *initial)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    GtkWidget *cap = gtk_label_new(NULL);
    char markup[64];
    snprintf(markup, sizeof(markup), "<small><b>%s</b></small>", caption);
    gtk_label_set_markup(GTK_LABEL(cap), markup);
    gtk_box_pack_start(GTK_BOX(box), cap, FALSE, FALSE, 0);

    GtkWidget *val = gtk_label_new(NULL);
    char vmarkup[64];
    snprintf(vmarkup, sizeof(vmarkup), "<small>%s</small>", initial);
    gtk_label_set_markup(GTK_LABEL(val), vmarkup);
    gtk_box_pack_start(GTK_BOX(box), val, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), box, FALSE, FALSE, 0);
    return val;
}

/**
 * @brief Append a vertical separator to the statistics bar. @param bar Bar box.
 */
static void stats_bar_separator(GtkWidget *bar)
{
    gtk_box_pack_start(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                       FALSE, FALSE, 0);
}

/**
 * @brief Build the statistics bar shown directly below the menu bar.
 * @return The populated bar widget.
 */
static GtkWidget *build_stats_bar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 3);
    gtk_widget_set_margin_bottom(bar, 3);

    g_gui.lbl_connection = stats_bar_field(bar, "Interface:", "—");
    stats_bar_separator(bar);
    g_gui.lbl_bitrate    = stats_bar_field(bar, "Bitrate:", "—");
    stats_bar_separator(bar);

    /* Bus load as a compact progress bar. */
    GtkWidget *load_cap = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(load_cap), "<small><b>Bus Load:</b></small>");
    gtk_box_pack_start(GTK_BOX(bar), load_cap, FALSE, FALSE, 0);
    g_gui.progress_bus_load = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(
        GTK_PROGRESS_BAR(g_gui.progress_bus_load), TRUE);
    gtk_widget_set_size_request(g_gui.progress_bus_load, 130, -1);
    gtk_widget_set_valign(g_gui.progress_bus_load, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(bar), g_gui.progress_bus_load, FALSE, FALSE, 0);
    stats_bar_separator(bar);

    g_gui.lbl_rx        = stats_bar_field(bar, "Rx:", "0");
    stats_bar_separator(bar);
    g_gui.lbl_tx        = stats_bar_field(bar, "Tx:", "0");
    stats_bar_separator(bar);
    g_gui.lbl_err       = stats_bar_field(bar, "Err:", "0");
    stats_bar_separator(bar);
    g_gui.lbl_bus_state = stats_bar_field(bar, "State:", "—");

    return bar;
}

/* ------------------------------------------------------------------ */
/* Menu bar                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Build the application menu bar. @return The menu bar widget.
 */
static GtkWidget *build_menubar(void)
{
    GtkWidget *bar = gtk_menu_bar_new();

    /* --- File --- */
    GtkWidget *file_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Connect…",     G_CALLBACK(on_connect),     NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Disconnect",   G_CALLBACK(on_disconnect),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
        menu_item("_Quit",         G_CALLBACK(on_quit),        NULL));

    GtkWidget *file_item = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_item);

    /* --- Trace --- */
    GtkWidget *trace_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(trace_menu),
        menu_item("_Start",        G_CALLBACK(on_trace_start), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(trace_menu),
        menu_item("S_top",         G_CALLBACK(on_trace_stop),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(trace_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(trace_menu),
        menu_item("_Save as CSV…", G_CALLBACK(on_trace_save),  NULL));

    GtkWidget *trace_item = gtk_menu_item_new_with_mnemonic("_Trace");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(trace_item), trace_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), trace_item);

    /* --- CAN --- */
    GtkWidget *can_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Settings…",    G_CALLBACK(on_settings),    NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Transmit…",    G_CALLBACK(on_transmit),    NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(can_menu),
        menu_item("_Clear Trace",  G_CALLBACK(on_clear),       NULL));

    GtkWidget *can_item = gtk_menu_item_new_with_mnemonic("_CAN");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(can_item), can_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), can_item);

    /* --- View --- */
    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *dedup_item = gtk_check_menu_item_new_with_mnemonic(
        "_Roll Up Tx Messages");
    g_signal_connect(dedup_item, "toggled",
                     G_CALLBACK(on_dedup_toggle), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), dedup_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
        gtk_separator_menu_item_new());

    GtkWidget *id_menu = gtk_menu_new();
    GtkWidget *id_hex = gtk_radio_menu_item_new_with_mnemonic(
        NULL, "_Hexadecimal");
    GtkWidget *id_dec = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(id_hex), "_Decimal");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(id_hex),
        g_app.id_format == APP_ID_FORMAT_HEX);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(id_dec),
        g_app.id_format == APP_ID_FORMAT_DEC);
    g_signal_connect(id_hex, "toggled",
        G_CALLBACK(on_id_format_toggle),
        GINT_TO_POINTER(APP_ID_FORMAT_HEX));
    g_signal_connect(id_dec, "toggled",
        G_CALLBACK(on_id_format_toggle),
        GINT_TO_POINTER(APP_ID_FORMAT_DEC));
    gtk_menu_shell_append(GTK_MENU_SHELL(id_menu), id_hex);
    gtk_menu_shell_append(GTK_MENU_SHELL(id_menu), id_dec);

    GtkWidget *id_item = gtk_menu_item_new_with_mnemonic("_ID Format");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(id_item), id_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), id_item);

    GtkWidget *data_menu = gtk_menu_new();
    GtkWidget *data_hex = gtk_radio_menu_item_new_with_mnemonic(
        NULL, "_Hexadecimal");
    GtkWidget *data_dec = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(data_hex), "_Decimal");
    GtkWidget *data_ascii = gtk_radio_menu_item_new_with_mnemonic_from_widget(
        GTK_RADIO_MENU_ITEM(data_hex), "_ASCII");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_hex),
        g_app.data_format == APP_DATA_FORMAT_HEX);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_dec),
        g_app.data_format == APP_DATA_FORMAT_DEC);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(data_ascii),
        g_app.data_format == APP_DATA_FORMAT_ASCII);
    g_signal_connect(data_hex, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_HEX));
    g_signal_connect(data_dec, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_DEC));
    g_signal_connect(data_ascii, "toggled",
        G_CALLBACK(on_data_format_toggle),
        GINT_TO_POINTER(APP_DATA_FORMAT_ASCII));
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_hex);
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_dec);
    gtk_menu_shell_append(GTK_MENU_SHELL(data_menu), data_ascii);

    GtkWidget *data_item = gtk_menu_item_new_with_mnemonic("_Data Format");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(data_item), data_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), data_item);

    GtkWidget *view_item = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_item);

    /* --- Database (DBC signal analysis) --- */
    GtkWidget *db_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(db_menu),
        menu_item("_Load DBC…",     G_CALLBACK(on_db_load),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(db_menu),
        menu_item("Load _Demo DBC", G_CALLBACK(on_db_demo),  NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(db_menu),
        gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(db_menu),
        menu_item("_Clear Database", G_CALLBACK(on_db_clear), NULL));

    GtkWidget *db_item = gtk_menu_item_new_with_mnemonic("_Database");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(db_item), db_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), db_item);

    /* --- Help --- */
    GtkWidget *help_menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu),
        menu_item("_About",        G_CALLBACK(on_about),       NULL));

    GtkWidget *help_item = gtk_menu_item_new_with_mnemonic("_Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), help_item);

    return bar;
}

/* ------------------------------------------------------------------ */
/* Toolbar                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Create an icon tool button.
 * @param icon Icon name. @param tooltip Tooltip. @param cb Click callback.
 * @return The tool item as a widget.
 */
static GtkWidget *make_toolbtn(const char *icon, const char *tooltip,
                                GCallback cb)
{
    GtkToolItem *btn = gtk_tool_button_new(
        gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR),
        NULL);
    gtk_tool_item_set_tooltip_text(btn, tooltip);
    if (cb) g_signal_connect(btn, "clicked", cb, NULL);
    return GTK_WIDGET(btn);
}

/**
 * @brief Build the main toolbar. @return The toolbar widget.
 */
static GtkWidget *build_toolbar(void)
{
    GtkWidget *tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_BOTH_HORIZ);

    g_gui.toolbar_connect = make_toolbtn(
        "network-wired", "Connect (F5)", G_CALLBACK(on_connect));
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_connect), -1);

    g_gui.toolbar_disconnect = make_toolbtn(
        "network-offline", "Disconnect (F6)", G_CALLBACK(on_disconnect));
    gtk_widget_set_sensitive(g_gui.toolbar_disconnect, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_disconnect), -1);

    GtkToolItem *sep1 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep1, -1);

    GtkWidget *clear_btn = make_toolbtn(
        "edit-clear", "Clear Trace", G_CALLBACK(on_clear));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(clear_btn), -1);

    GtkToolItem *sep2 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep2, -1);

    g_gui.toolbar_trace_start = make_toolbtn(
        "media-record", "Start Trace", G_CALLBACK(on_trace_start));
    gtk_widget_set_sensitive(g_gui.toolbar_trace_start, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_trace_start), -1);

    g_gui.toolbar_trace_stop = make_toolbtn(
        "media-playback-stop", "Stop Trace", G_CALLBACK(on_trace_stop));
    gtk_widget_set_sensitive(g_gui.toolbar_trace_stop, FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(tb),
                       GTK_TOOL_ITEM(g_gui.toolbar_trace_stop), -1);

    GtkToolItem *sep3 = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep3, -1);

    GtkWidget *tx_btn = make_toolbtn(
        "mail-send", "Transmit Message…", G_CALLBACK(on_transmit));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(tx_btn), -1);

    GtkWidget *cfg_btn = make_toolbtn(
        "preferences-system", "Connection Settings…",
        G_CALLBACK(on_settings));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), GTK_TOOL_ITEM(cfg_btn), -1);

    return tb;
}

/* ------------------------------------------------------------------ */
/* Keyboard shortcuts                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Global key handler (F5 connect, F6 disconnect, Ctrl+Del clear).
 * @param w Widget. @param ev Key event. @param d Unused. @return TRUE if handled.
 */
static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    switch (ev->keyval) {
    case GDK_KEY_F5: on_connect(NULL, NULL);     return TRUE;
    case GDK_KEY_F6: on_disconnect(NULL, NULL);  return TRUE;
    case GDK_KEY_Delete:
        if (ev->state & GDK_CONTROL_MASK) {
            on_clear(NULL, NULL);
            return TRUE;
        }
        break;
    default: break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Inline transmit panel (shown as a notebook tab in the main window)  */
/* ------------------------------------------------------------------ */

/**
 * @brief Strictly parse a bounded hex value.
 * @param s Text. @param max Max value. @param out Result. @return 0 ok, -1 error.
 */
static int parse_hex_u32_strict(const char *s, uint32_t max, uint32_t *out)
{
    if (!s || !out) return -1;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0' || *s == '+' || *s == '-') return -1;

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (errno == ERANGE || end == s || v > max) return -1;

    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return -1;

    *out = (uint32_t)v;
    return 0;
}

/**
 * @brief Map a row-index pointer to its transmit-row struct.
 * @param data Row index as pointer. @return Row pointer or NULL.
 */
static tx_panel_row_t *txp_row_from_data(gpointer data)
{
    int row = GPOINTER_TO_INT(data);
    if (row < 0 || row >= TX_PANEL_MAX_ROWS)
        return NULL;
    return &s_txp.rows[row];
}

static void txp_set_status(tx_panel_row_t *row,
                           const char *color,
                           const char *msg)
{
    if (!row || !GTK_IS_LABEL(row->status_lbl))
        return;

    if (!color || !msg || msg[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(row->status_lbl), msg ? msg : "");
        return;
    }

    char markup[192];
    snprintf(markup, sizeof(markup),
             "<span color='%s'>%s</span>", color, msg);
    gtk_label_set_markup(GTK_LABEL(row->status_lbl), markup);
}

/**
 * @brief Reset one transmit row to defaults. @param row Row.
 */
static void txp_clear_row(tx_panel_row_t *row)
{
    if (!row) return;

    gtk_entry_set_text(GTK_ENTRY(row->id_entry), "001");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->ext_check), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->rtr_check), FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->dlc_spin), 8);
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++)
        if (row->data_entry[i])
            gtk_entry_set_text(GTK_ENTRY(row->data_entry[i]), "00");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->interval_spin), 100);
    txp_stop_cyclic_row(row, "Cleared");
}

/**
 * @brief Reset every transmit row to defaults.
 */
static void txp_clear_all(void)
{
    for (int i = 0; i < s_txp.count; i++)
        txp_clear_row(&s_txp.rows[i]);
}

/**
 * @brief Append a new transmit row to the panel grid.
 */
static void txp_add_row(void)
{
    if (s_txp.count >= TX_PANEL_MAX_ROWS)
        return;

    int row_index = s_txp.count;
    int grid_row  = row_index + 1;          /* grid row 0 holds the header */
    GtkGrid *grid = GTK_GRID(s_txp.rows_box);
    tx_panel_row_t *row = &s_txp.rows[row_index];

    row->num_lbl = gtk_label_new(NULL);
    char num[16];
    snprintf(num, sizeof(num), "%d", row_index + 1);
    gtk_label_set_text(GTK_LABEL(row->num_lbl), num);
    gtk_grid_attach(grid, row->num_lbl, TXG_COL_NUM, grid_row, 1, 1);

    row->id_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(row->id_entry), 8);
    gtk_entry_set_text(GTK_ENTRY(row->id_entry), "001");
    gtk_entry_set_width_chars(GTK_ENTRY(row->id_entry), 5);
    gtk_widget_set_hexpand(row->id_entry, TRUE);
    gtk_grid_attach(grid, row->id_entry, TXG_COL_ID, grid_row, 1, 1);

    row->ext_check = gtk_check_button_new();
    gtk_widget_set_tooltip_text(row->ext_check, "Extended ID");
    gtk_widget_set_halign(row->ext_check, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, row->ext_check, TXG_COL_EXT, grid_row, 1, 1);

    row->rtr_check = gtk_check_button_new();
    gtk_widget_set_tooltip_text(row->rtr_check, "Remote frame");
    gtk_widget_set_halign(row->rtr_check, GTK_ALIGN_CENTER);
    g_signal_connect(row->rtr_check, "toggled",
                     G_CALLBACK(txp_rtr_toggled), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->rtr_check, TXG_COL_RTR, grid_row, 1, 1);

    /* DLC range follows the link mode: 0..8 for classic CAN, 0..64 for CAN FD.
     * gui_tx_panel_update_fd() widens the upper bound when FD is active. */
    int dlc_max = g_app.fd_mode ? CANFD_DATA_MAX : CAN_MAX_DLC;
    GtkAdjustment *dlc_adj = gtk_adjustment_new(8, 0, dlc_max, 1, 8, 0);
    row->dlc_spin = gtk_spin_button_new(dlc_adj, 1, 0);
    g_signal_connect(row->dlc_spin, "value-changed",
                     G_CALLBACK(txp_dlc_changed), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->dlc_spin, TXG_COL_DLC, grid_row, 1, 1);

    /* Data bytes live in a wrapping flow-box so a 64-byte FD frame stays inside
     * the window (the bytes wrap to new lines instead of forcing a scroll). */
    row->data_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(row->data_box),
                                    GTK_SELECTION_NONE);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(row->data_box),
                                   GTK_ORIENTATION_HORIZONTAL);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(row->data_box), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(row->data_box), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(row->data_box),
                                           CANFD_DATA_MAX);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(row->data_box), 2);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(row->data_box), 2);
    gtk_widget_set_hexpand(row->data_box, TRUE);
    gtk_grid_attach(grid, row->data_box, TXG_COL_DATA, grid_row, 1, 1);

    for (int i = 0; i < TX_PANEL_DATA_COLS; i++) {
        row->data_entry[i] = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(row->data_entry[i]), 2);
        gtk_entry_set_text(GTK_ENTRY(row->data_entry[i]), "00");
        gtk_entry_set_width_chars(GTK_ENTRY(row->data_entry[i]), 2);
        gtk_flow_box_insert(GTK_FLOW_BOX(row->data_box),
                            row->data_entry[i], -1);
        /* The byte (and its auto-created GtkFlowBoxChild wrapper) is shown or
         * hidden by txp_update_data_fields(); keep show_all from revealing the
         * bytes beyond the current DLC. */
        GtkWidget *wrap = gtk_widget_get_parent(row->data_entry[i]);
        gtk_widget_set_no_show_all(row->data_entry[i], TRUE);
        if (wrap)
            gtk_widget_set_no_show_all(wrap, TRUE);
    }

    GtkAdjustment *int_adj = gtk_adjustment_new(100, 1, 60000, 10, 100, 0);
    row->interval_spin = gtk_spin_button_new(int_adj, 10, 0);
    gtk_grid_attach(grid, row->interval_spin, TXG_COL_INTERVAL, grid_row, 1, 1);

    row->send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(row->send_btn, "clicked",
                     G_CALLBACK(txp_send_once), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->send_btn, TXG_COL_SEND, grid_row, 1, 1);

    row->start_btn = gtk_button_new_with_label("Start");
    g_signal_connect(row->start_btn, "clicked",
                     G_CALLBACK(txp_start_cyclic), GINT_TO_POINTER(row_index));
    gtk_grid_attach(grid, row->start_btn, TXG_COL_START, grid_row, 1, 1);

    row->stop_btn = gtk_button_new_with_label("Stop");
    g_signal_connect(row->stop_btn, "clicked",
                     G_CALLBACK(txp_stop_cyclic), GINT_TO_POINTER(row_index));
    gtk_widget_set_sensitive(row->stop_btn, FALSE);
    gtk_grid_attach(grid, row->stop_btn, TXG_COL_STOP, grid_row, 1, 1);

    row->status_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(row->status_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(row->status_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(row->status_lbl, 80, -1);
    gtk_grid_attach(grid, row->status_lbl, TXG_COL_STATUS, grid_row, 1, 1);

    txp_update_data_fields(row);
    s_txp.count++;
    gtk_widget_show_all(s_txp.rows_box);
}

/**
 * @brief Remove and destroy the last transmit row.
 */
static void txp_remove_last_row(void)
{
    if (s_txp.count <= 0) return;
    tx_panel_row_t *row = &s_txp.rows[s_txp.count - 1];
    if (row->cyclic_id) {
        g_source_remove(row->cyclic_id);
        row->cyclic_id = 0;
    }
    /* Destroy every widget that makes up this grid row.  Destroying the flow-box
     * (data_box) also destroys all its child byte entries. */
    GtkWidget *widgets[] = {
        row->num_lbl, row->id_entry, row->ext_check, row->rtr_check,
        row->dlc_spin, row->data_box, row->interval_spin, row->send_btn,
        row->start_btn, row->stop_btn, row->status_lbl,
    };
    for (size_t i = 0; i < sizeof(widgets) / sizeof(widgets[0]); i++)
        if (widgets[i]) gtk_widget_destroy(widgets[i]);
    memset(row, 0, sizeof(*row));
    s_txp.count--;
}

/**
 * @brief Show a red error status on a row. @param row Row. @param msg Message.
 */
static void txp_set_error(tx_panel_row_t *row, const char *msg)
{
    txp_set_status(row, "red", msg);
}

/**
 * @brief Show exactly DLC data-byte fields for a row (none if RTR). @param row Row.
 */
static void txp_update_data_fields(tx_panel_row_t *row)
{
    if (!row || !GTK_IS_SPIN_BUTTON(row->dlc_spin))
        return;

    gboolean rtr = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->rtr_check));
    int dlc = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(row->dlc_spin));

    /* Show exactly DLC byte fields (none for a remote frame); the data indexes
     * are added/removed dynamically as the DLC spin changes. */
    for (int i = 0; i < TX_PANEL_DATA_COLS; i++) {
        if (!row->data_entry[i])
            continue;
        gboolean visible = !rtr && i < dlc;
        GtkWidget *wrap = gtk_widget_get_parent(row->data_entry[i]);
        gtk_widget_set_visible(row->data_entry[i], visible);
        if (wrap)
            gtk_widget_set_visible(wrap, visible);
    }
}

/**
 * @brief Round a byte count up to a valid CAN FD payload length.
 *
 * CAN FD frames only carry specific lengths (0..8, 12, 16, 20, 24, 32, 48, 64).
 * @param n  Requested length.
 * @return The smallest valid CAN FD length >= @p n (capped at 64).
 */
static uint8_t canfd_round_len(int n)
{
    static const uint8_t valid[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        if (n <= valid[i])
            return valid[i];
    return CANFD_DATA_MAX;
}

/**
 * @brief Validate a transmit row and assemble a frame.
 * @param row Row. @param msg Output frame. @return TRUE if valid.
 */
static gboolean txp_build_frame(tx_panel_row_t *row, can_msg_t *msg)
{
    if (!row || !msg)
        return FALSE;

    memset(msg, 0, sizeof(*msg));

    msg->is_extended = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->ext_check));
    msg->is_remote = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(row->rtr_check));

    /* Frames are sent as CAN FD when the link is in FD mode (BRS enabled). */
    msg->is_fd  = g_app.fd_mode ? 1 : 0;
    msg->is_brs = g_app.fd_mode ? 1 : 0;

    int req_len = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(row->dlc_spin));
    if (!msg->is_fd && req_len > CAN_MAX_DLC)
        req_len = CAN_MAX_DLC;

    const char *ids = gtk_entry_get_text(GTK_ENTRY(row->id_entry));
    uint32_t id_max = msg->is_extended ? TX_EXT_ID_MAX : TX_STD_ID_MAX;
    if (parse_hex_u32_strict(ids, id_max, &msg->id) != 0) {
        txp_set_error(row, msg->is_extended
            ? "Invalid 29-bit ID"
            : "Invalid 11-bit ID");
        return FALSE;
    }

    if (!msg->is_remote) {
        for (int i = 0; i < req_len && i < TX_PANEL_DATA_COLS; i++) {
            const char *ds = gtk_entry_get_text(
                GTK_ENTRY(row->data_entry[i]));
            uint32_t value = 0;
            if (parse_hex_u32_strict(ds, TX_BYTE_MAX, &value) != 0) {
                char err[64];
                snprintf(err, sizeof(err), "Invalid data byte [%d]", i);
                txp_set_error(row, err);
                return FALSE;
            }
            msg->data[i] = (uint8_t)value;
        }
    }

    /* CAN FD payloads must be a valid length; pad up if needed (extra bytes
     * are already zero from the memset above). */
    msg->dlc = msg->is_fd ? canfd_round_len(req_len) : (uint8_t)req_len;
    return TRUE;
}

/**
 * @brief Build and enqueue a row's frame for transmission.
 * @param row_index Row. @param cyclic Non-zero if from the cyclic timer. @return TRUE on success.
 */
static gboolean txp_queue_frame(int row_index, gboolean cyclic)
{
    tx_panel_row_t *row = txp_row_from_data(GINT_TO_POINTER(row_index));
    if (!row)
        return FALSE;

    if (!g_app.connected) {
        txp_set_error(row, "Not connected");
        return FALSE;
    }

    can_msg_t *msg = calloc(1, sizeof(can_msg_t));
    if (!msg) {
        txp_set_error(row, "Out of memory");
        return FALSE;
    }

    if (!txp_build_frame(row, msg)) {
        free(msg);
        return FALSE;
    }

    g_async_queue_push(g_app.tx_queue, msg);
    if (!cyclic)
        txp_set_status(row, "green", "Queued");
    return TRUE;
}

/**
 * @brief Stop a row's cyclic timer and update its buttons. @param row Row. @param status Status text.
 */
static void txp_stop_cyclic_row(tx_panel_row_t *row, const char *status)
{
    if (!row)
        return;

    if (row->cyclic_id) {
        g_source_remove(row->cyclic_id);
        row->cyclic_id = 0;
    }
    if (GTK_IS_WIDGET(row->start_btn))
        gtk_widget_set_sensitive(row->start_btn, TRUE);
    if (GTK_IS_WIDGET(row->stop_btn))
        gtk_widget_set_sensitive(row->stop_btn, FALSE);
    if (status)
        txp_set_status(row, NULL, status);
}

/**
 * @brief Stop cyclic transmission on every row.
 */
static void txp_stop_all_cyclic(void)
{
    for (int i = 0; i < TX_PANEL_MAX_ROWS; i++)
        txp_stop_cyclic_row(&s_txp.rows[i], "");
}

/**
 * @brief Cyclic-timer callback for one row. @param data Row index. @return Continue/remove.
 */
static gboolean txp_cyclic_tick(gpointer data)
{
    int row_index = GPOINTER_TO_INT(data);
    tx_panel_row_t *row = txp_row_from_data(data);
    if (!row || !row->cyclic_id)
        return G_SOURCE_REMOVE;

    if (!g_app.connected) {
        txp_stop_cyclic_row(row, "Disconnected");
        return G_SOURCE_REMOVE;
    }

    txp_queue_frame(row_index, TRUE);
    return G_SOURCE_CONTINUE;
}

/**
 * @brief "Send" button handler for a row. @param w Widget. @param d Row index.
 */
static void txp_send_once(GtkWidget *w, gpointer d)
{
    (void)w;
    txp_queue_frame(GPOINTER_TO_INT(d), FALSE);
}

/**
 * @brief "Start" button handler — arm a row's cyclic timer. @param w Widget. @param d Row index.
 */
static void txp_start_cyclic(GtkWidget *w, gpointer d)
{
    (void)w;
    int row_index = GPOINTER_TO_INT(d);
    tx_panel_row_t *row = txp_row_from_data(d);
    if (!row || row->cyclic_id)
        return;

    if (!g_app.connected) {
        txp_set_error(row, "Not connected");
        return;
    }

    can_msg_t frame;
    if (!txp_build_frame(row, &frame))
        return;

    guint ms = (guint)gtk_spin_button_get_value_as_int(
                   GTK_SPIN_BUTTON(row->interval_spin));
    if (ms < 1)
        ms = 100;

    row->cyclic_id = g_timeout_add(ms, txp_cyclic_tick,
                                   GINT_TO_POINTER(row_index));
    gtk_widget_set_sensitive(row->start_btn, FALSE);
    gtk_widget_set_sensitive(row->stop_btn, TRUE);
    txp_queue_frame(row_index, TRUE);
    txp_set_status(row, "blue", "Cyclic running");
}

/**
 * @brief "Stop" button handler — disarm a row's cyclic timer. @param w Widget. @param d Row index.
 */
static void txp_stop_cyclic(GtkWidget *w, gpointer d)
{
    (void)w;
    tx_panel_row_t *row = txp_row_from_data(d);
    txp_stop_cyclic_row(row, "");
}

/**
 * @brief DLC spin handler — refresh visible data fields. @param btn Spin. @param d Row index.
 */
static void txp_dlc_changed(GtkSpinButton *btn, gpointer d)
{
    (void)btn;
    txp_update_data_fields(txp_row_from_data(d));
}

/**
 * @brief RTR toggle handler — refresh visible data fields. @param btn Button. @param d Row index.
 */
static void txp_rtr_toggled(GtkToggleButton *btn, gpointer d)
{
    (void)btn;
    txp_update_data_fields(txp_row_from_data(d));
}

/**
 * @brief Create a centred transmit-panel header label. @param text Text. @return The label.
 */
static GtkWidget *txp_header_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_margin_start(label, 3);
    gtk_widget_set_margin_end(label, 3);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
    return label;
}

/**
 * @brief Build the inline transmit panel (header + first row). @return The panel frame.
 */
static GtkWidget *create_transmit_panel(void)
{
    GtkWidget *frame = gtk_frame_new("Transmit Messages");
    gtk_widget_set_margin_start(frame, 4);
    gtk_widget_set_margin_end(frame, 4);
    gtk_widget_set_margin_top(frame, 4);
    gtk_widget_set_margin_bottom(frame, 4);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);
    gtk_container_add(GTK_CONTAINER(frame), outer);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);

    GtkWidget *control_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top), control_btn_box, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear Rows");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(txp_clear_all), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), clear_btn, FALSE, FALSE, 0);

    GtkWidget *add_btn = gtk_button_new_with_label("Add Row");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(txp_add_row), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), add_btn, FALSE, FALSE, 0);

    GtkWidget *remove_btn = gtk_button_new_with_label("Remove Last");
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(txp_remove_last_row), NULL);
    gtk_box_pack_start(GTK_BOX(control_btn_box), remove_btn, FALSE, FALSE, 0);

    GtkWidget *advanced_btn = gtk_button_new_with_label("Advanced TX...");
    g_signal_connect(advanced_btn, "clicked", G_CALLBACK(on_transmit), NULL);
    gtk_box_pack_end(GTK_BOX(top), advanced_btn, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    /* Never scroll horizontally: the grid stretches to the panel width so the
     * Send/Start/Stop buttons are always visible and the columns resize by
     * ratio with the window.  Vertical scrolling kicks in only for many rows. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    s_txp.rows_box = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(s_txp.rows_box), 6);
    gtk_grid_set_row_spacing(GTK_GRID(s_txp.rows_box), 4);
    gtk_widget_set_hexpand(s_txp.rows_box, TRUE);

    /* Header row (grid row 0) – aligned with every transmit row below. */
    struct { int col; const char *txt; gboolean expand; } hdr[] = {
        { TXG_COL_NUM,      "#",            FALSE },
        { TXG_COL_ID,       "CAN-ID",       TRUE  },
        { TXG_COL_EXT,      "Ext",          FALSE },
        { TXG_COL_RTR,      "RTR",          FALSE },
        { TXG_COL_DLC,      "DLC",          FALSE },
        { TXG_COL_DATA,     "Data (hex)",   TRUE  },
        { TXG_COL_INTERVAL, "Cycle (ms)",   FALSE },
        { TXG_COL_STATUS,   "Status",       FALSE },
    };
    for (size_t i = 0; i < sizeof(hdr) / sizeof(hdr[0]); i++) {
        GtkWidget *h = txp_header_label(hdr[i].txt);
        gtk_widget_set_hexpand(h, hdr[i].expand);
        gtk_grid_attach(GTK_GRID(s_txp.rows_box), h, hdr[i].col, 0, 1, 1);
    }

    gtk_container_add(GTK_CONTAINER(scroll), s_txp.rows_box);

    /* Initialize with one default row */
    s_txp.count = 0;
    txp_add_row();
    gui_tx_panel_update_fd();

    return frame;
}

/* Sync every transmit row's DLC range and visible data fields with the current
 * CAN FD link mode (called after connect and at panel creation). */
void gui_tx_panel_update_fd(void)
{
    int max = g_app.fd_mode ? CANFD_DATA_MAX : CAN_MAX_DLC;
    for (int r = 0; r < s_txp.count; r++) {
        tx_panel_row_t *row = &s_txp.rows[r];
        if (!GTK_IS_SPIN_BUTTON(row->dlc_spin))
            continue;
        GtkAdjustment *adj = gtk_spin_button_get_adjustment(
            GTK_SPIN_BUTTON(row->dlc_spin));
        gtk_adjustment_set_upper(adj, max);
        if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(row->dlc_spin)) > max)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->dlc_spin), max);
        txp_update_data_fields(row);
    }
}


/* ------------------------------------------------------------------ */
/* Main window                                                          */
/* ------------------------------------------------------------------ */

GtkWidget *gui_create_main_window(GtkApplication *app)
{
    GtkWidget *window = gtk_application_window_new(app);
    g_gui.window = window;

    gtk_window_set_title(GTK_WINDOW(window), "CANoScope");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    /* Application / window icon (Taksys logo). */
    GdkPixbuf *win_icon = gui_load_logo(128);
    if (win_icon) {
        gtk_window_set_icon(GTK_WINDOW(window), win_icon);
        g_object_unref(win_icon);
    }

    /* On window close: disconnect CAN then allow default destroy */
    g_signal_connect(window, "delete-event",
                     G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(window, "key-press-event",
                     G_CALLBACK(on_key_press), NULL);

    /* Outer vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* Menu bar */
    GtkWidget *menubar = build_menubar();
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* Statistics bar (directly below the menu bar) */
    gtk_box_pack_start(GTK_BOX(vbox), build_stats_bar(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Toolbar */
    GtkWidget *toolbar = build_toolbar();
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Main content: a notebook with live CAN trace, decoded signals,
     * realtime signal graphs, two-signal math analysis, and DBC creation. */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    /* Page 1 — Receive / Transmit (trace on top, transmit rows below). */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *trace_scroll = create_trace_view();
    gtk_paned_pack1(GTK_PANED(paned), trace_scroll, TRUE, TRUE);
    GtkWidget *tx_panel = create_transmit_panel();
    gtk_paned_pack2(GTK_PANED(paned), tx_panel, TRUE, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), paned,
                             gtk_label_new("Receive / Transmit"));

    /* Page 2 — Signal Analysis. */
    GtkWidget *sig_page = gui_create_signal_view();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sig_page,
                             gtk_label_new("Signal Analysis"));

    /* Page 3 — Signal Analysis Viewer (multi-signal graph over time). */
    GtkWidget *plot_page = gui_create_signal_plot();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), plot_page,
                             gtk_label_new("Signal Analysis Viewer"));

    /* Page 4 — Math (two-signal realtime statistical analysis). */
    GtkWidget *math_page = gui_create_math_view();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), math_page,
                             gtk_label_new("Math"));

    /* Page 5 — DB Creation (generate/update DBC files from RX rows). */
    GtkWidget *db_page = gui_create_db_creation_view();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), db_page,
                             gtk_label_new("DB Creation"));

    /* Status bar */
    g_gui.statusbar    = gtk_statusbar_new();
    g_gui.statusbar_ctx = gtk_statusbar_get_context_id(
        GTK_STATUSBAR(g_gui.statusbar), "main");
    gtk_box_pack_start(GTK_BOX(vbox),
                       g_gui.statusbar, FALSE, FALSE, 0);

    gtk_statusbar_push(GTK_STATUSBAR(g_gui.statusbar),
                       g_gui.statusbar_ctx,
                       "Ready – use File > Connect or press F5 to connect.");

    /* ---- Footer bar ---- */
    GtkWidget *footer_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), footer_sep, FALSE, FALSE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(footer, 8);
    gtk_widget_set_margin_end(footer, 8);
    gtk_widget_set_margin_top(footer, 3);
    gtk_widget_set_margin_bottom(footer, 3);
    gtk_box_pack_start(GTK_BOX(vbox), footer, FALSE, FALSE, 0);

    /* Company icon (Taksys logo) in front of the footer text. */
    GdkPixbuf *footer_logo = gui_load_logo(20);
    if (footer_logo) {
        GtkWidget *footer_icon = gtk_image_new_from_pixbuf(footer_logo);
        g_object_unref(footer_logo);
        gtk_widget_set_margin_end(footer_icon, 6);
        gtk_box_pack_start(GTK_BOX(footer), footer_icon, FALSE, FALSE, 0);
    }

    GtkWidget *footer_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer_lbl),
        "<small><b>Taksys</b>  |  "
        "Author: Subhajit Roy  |  "
        "<a href=\"mailto:subhajitroy005@gmail.com\">"
        "subhajitroy005@gmail.com</a>  |  "
        "License: Apache-2.0</small>");
    gtk_label_set_xalign(GTK_LABEL(footer_lbl), 0.5f);
    gtk_box_pack_start(GTK_BOX(footer), footer_lbl, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
    gui_update_stats();

    /* Auto-load the bundled demo database so DBC-backed tabs are populated
     * out of the box (no-op if the asset is missing). */
    gui_signal_load_default_dbc();
    return window;
}

/* ------------------------------------------------------------------ */
/* About dialog                                                         */
/* ------------------------------------------------------------------ */

void gui_show_about_dialog(GtkWidget *parent)
{
    GtkAboutDialog *dlg = GTK_ABOUT_DIALOG(gtk_about_dialog_new());
    gtk_about_dialog_set_program_name(dlg, CANOSCOPE_APP_NAME);
    gtk_about_dialog_set_version(dlg, CANOSCOPE_VERSION);
    gtk_about_dialog_set_comments(dlg,
        "Open-source CAN bus monitor and analyser for Linux.\n"
        "Uses the Linux SocketCAN subsystem (PF_CAN).\n"
        "Inspired by PEAK-System PCAN-View.\n\n"
        "Developed and maintained by " CANOSCOPE_VENDOR ".");
    gtk_about_dialog_set_copyright(dlg, CANOSCOPE_COPYRIGHT);
    gtk_about_dialog_set_license_type(dlg, GTK_LICENSE_APACHE_2_0);
    gtk_about_dialog_set_website(dlg, "https://taksys.in");
    gtk_about_dialog_set_website_label(dlg, "Taksys");

    GdkPixbuf *logo = gui_load_logo(96);
    if (logo) {
        gtk_about_dialog_set_logo(dlg, logo);
        g_object_unref(logo);
    }

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(dlg),
                                     GTK_WINDOW(parent));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(GTK_WIDGET(dlg));
}
