/**
 * @file message_view.c
 * @brief CAN trace `GtkTreeView`, value formatting, and statistics rendering.
 *
 * @details
 * Builds and maintains the receive/transmit trace list and its backing
 * `GtkListStore`, including row roll-up (deduplication) by CAN ID, interval and
 * frequency computation, and re-formatting when the ID/data display mode
 * changes.  Also provides the shared value-formatting helpers
 * (@ref gui_format_id, @ref gui_format_data, @ref gui_msg_type_str, …), the
 * statistics refresh (@ref gui_update_stats), the status-bar helper, and the
 * modal error dialog.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"

#define MAX_TRACE_ROWS  50000
#define DEDUP_TABLE_SZ  0x800u   /* initial hash table size */
#define DATA_TEXT_MAX   512
#define RAW_DATA_MAX    ((CANFD_DATA_MAX * 2u) + 1u)
#define ID_TEXT_MAX     16
#define TIME_TEXT_MAX   32
#define INTERVAL_TEXT_MAX 32
#define FREQ_TEXT_MAX   32

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                   */
/* ------------------------------------------------------------------ */

void gui_format_id(char *buf, size_t sz, uint32_t id, int is_ext)
{
    if (sz == 0) return;

    if (g_app.id_format == APP_ID_FORMAT_DEC) {
        snprintf(buf, sz, "%u", id);
    } else if (is_ext) {
        snprintf(buf, sz, "%08X", id);
    } else {
        snprintf(buf, sz, "%03X", id);
    }
}

void gui_format_data(char *buf, size_t sz, const uint8_t *d, uint8_t dlc)
{
    if (sz == 0) return;
    buf[0] = '\0';

    if (dlc == 0) {
        snprintf(buf, sz, "-");
        return;
    }

    size_t pos = 0;
    if (g_app.data_format == APP_DATA_FORMAT_ASCII) {
        for (uint8_t i = 0; i < dlc && pos + 1 < sz; i++) {
            unsigned char c = d[i];
            buf[pos++] = isprint(c) ? (char)c : '.';
        }
        buf[pos] = '\0';
        return;
    }

    for (uint8_t i = 0; i < dlc && pos + 4 < sz; i++) {
        if (g_app.data_format == APP_DATA_FORMAT_DEC) {
            pos += (size_t)snprintf(buf + pos, sz - pos,
                                    i ? " %u" : "%u", d[i]);
        } else {
            pos += (size_t)snprintf(buf + pos, sz - pos,
                                    i ? " %02X" : "%02X", d[i]);
        }
    }
}

/**
 * @brief Format payload bytes as a compact uppercase hex string (no spaces).
 * @param buf Output buffer. @param sz Size. @param d Bytes. @param dlc Count.
 */
static void format_raw_data(char *buf, size_t sz,
                            const uint8_t *d, uint8_t dlc)
{
    if (sz == 0) return;
    buf[0] = '\0';

    size_t pos = 0;
    for (uint8_t i = 0; i < dlc && pos + 3 <= sz; i++) {
        pos += (size_t)snprintf(buf + pos, sz - pos, "%02X", d[i]);
    }
}

/** @brief Convert a hex digit to its value, or -1 if not a hex digit. @param c Char. @return 0..15 or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Parse a compact hex string back into bytes.
 * @param raw Hex string. @param out Output bytes. @param dlc Max bytes.
 * @return Number of bytes parsed.
 */
static uint8_t parse_raw_data(const char *raw, uint8_t *out, uint8_t dlc)
{
    if (!raw || !out) return 0;

    uint8_t count = 0;
    while (count < dlc && raw[count * 2] && raw[count * 2 + 1]) {
        int hi = hex_value(raw[count * 2]);
        int lo = hex_value(raw[count * 2 + 1]);
        if (hi < 0 || lo < 0)
            break;
        out[count] = (uint8_t)((hi << 4) | lo);
        count++;
    }
    return count;
}

/** @brief Convert a `timespec` to nanoseconds. @param ts Time. @return Nanoseconds. */
static gint64 timespec_to_ns(const struct timespec *ts)
{
    return ((gint64)ts->tv_sec * 1000000000LL) + (gint64)ts->tv_nsec;
}

/** @brief Render a `timespec` as `HH:MM:SS.mmm`. @param buf Out. @param sz Size. @param ts Time. */
static void format_timestamp(char *buf, size_t sz, const struct timespec *ts)
{
    if (sz == 0) return;

    time_t sec = ts->tv_sec;
    struct tm tm_info;
    if (!localtime_r(&sec, &tm_info)) {
        snprintf(buf, sz, "--:--:--.---");
        return;
    }

    snprintf(buf, sz, "%02d:%02d:%02d.%03ld",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             ts->tv_nsec / 1000000L);
}

/** @brief Render an inter-frame interval in milliseconds. @param buf Out. @param sz Size. @param delta_ns Delta. */
static void format_interval(char *buf, size_t sz, gint64 delta_ns)
{
    if (sz == 0) return;
    if (delta_ns <= 0) {
        snprintf(buf, sz, "-");
        return;
    }

    snprintf(buf, sz, "%.3f", (double)delta_ns / 1000000.0);
}

/** @brief Render a frame frequency in hertz from an interval. @param buf Out. @param sz Size. @param delta_ns Delta. */
static void format_frequency(char *buf, size_t sz, gint64 delta_ns)
{
    if (sz == 0) return;
    if (delta_ns <= 0) {
        snprintf(buf, sz, "-");
        return;
    }

    double hz = 1000000000.0 / (double)delta_ns;
    if (hz >= 1000.0) {
        snprintf(buf, sz, "%.0f", hz);
    } else if (hz >= 10.0) {
        snprintf(buf, sz, "%.1f", hz);
    } else {
        snprintf(buf, sz, "%.3f", hz);
    }
}

/** @brief Decide whether @p msg should roll up onto an existing row. @param msg Frame. @return TRUE to roll up. */
static gboolean should_rollup_message(const can_msg_t *msg)
{
    if (msg->is_error)
        return FALSE;

    return msg->direction == CAN_DIR_RX || g_app.dedup_mode;
}

const char *gui_msg_type_str(const can_msg_t *m)
{
    if (m->is_error)  return "Err";
    if (m->is_fd)     return m->is_brs ? "FD+BRS" : "FD";
    if (m->is_remote) return m->is_extended ? "ExtRTR" : "StdRTR";
    return m->is_extended ? "Ext" : "Std";
}

const char *gui_bus_state_str(uint8_t st)
{
    switch (st) {
    case CAN_BUS_WARNING: return "Warning";
    case CAN_BUS_PASSIVE: return "Passive";
    case CAN_BUS_OFF:     return "Bus-Off";
    default:              return "Active";
    }
}

const char *gui_bus_state_color(uint8_t st)
{
    switch (st) {
    case CAN_BUS_WARNING: return "orange";
    case CAN_BUS_PASSIVE: return "red";
    case CAN_BUS_OFF:     return "darkred";
    default:              return "green";
    }
}

/* Row foreground colour */
/** @brief Return the row foreground colour for a frame, or NULL for default. @param m Frame. @return Colour or NULL. */
static const char *row_fg(const can_msg_t *m)
{
    if (m->is_error)                return "red";
    if (m->direction == CAN_DIR_TX) return "blue";
    return NULL; /* default */
}

/* ------------------------------------------------------------------ */
/* Trace view creation                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Create the trace `GtkTreeView` and its backing `GtkListStore`.
 * @return A scrolled-window container holding the tree view.
 */
GtkWidget *create_trace_view(void)
{
    GtkListStore *store = gtk_list_store_new(
        TCOL_NUM,
        G_TYPE_UINT64,   /* SEQ   */
        G_TYPE_STRING,   /* DIR   */
        G_TYPE_STRING,   /* TIME  */
        G_TYPE_STRING,   /* ID    */
        G_TYPE_STRING,   /* TYPE  */
        G_TYPE_UINT,     /* DLC   */
        G_TYPE_STRING,   /* DATA  */
        G_TYPE_STRING,   /* INTERVAL */
        G_TYPE_STRING,   /* FREQ */
        G_TYPE_UINT,     /* COUNT */
        G_TYPE_STRING,   /* FG    */
        G_TYPE_UINT,     /* ID_RAW */
        G_TYPE_BOOLEAN,  /* EXT_RAW */
        G_TYPE_UINT,     /* DIR_RAW */
        G_TYPE_STRING,   /* DATA_RAW */
        G_TYPE_INT64     /* TS_NS */
    );
    g_gui.trace_store = store;

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    g_gui.trace_view = tree;

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(tree), TRUE);

    struct { int col; const char *title; int width; } cols[] = {
        { TCOL_SEQ,   "#",         70  },
        { TCOL_DIR,   "Dir",       50  },
        { TCOL_TIME,  "Timestamp", 130 },
        { TCOL_ID,    "ID",        90  },
        { TCOL_TYPE,  "Type",      75  },
        { TCOL_DLC,   "DLC",       45  },
        { TCOL_DATA,  "Data",      350 },
        { TCOL_INTERVAL, "Interval (ms)", 105 },
        { TCOL_FREQ,  "Freq (Hz)", 90  },
        { TCOL_COUNT, "Count",     60  },
    };

    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) {
        GtkCellRenderer   *rend = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes(
            cols[i].title, rend,
            "text",       cols[i].col,
            "foreground", TCOL_FG,
            NULL);
        gtk_tree_view_column_set_sizing(col,
            GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cols[i].width);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_expand(col, i == TCOL_DATA);
        gtk_tree_view_column_set_sort_column_id(
            col, cols[i].col == TCOL_ID ? TCOL_ID_RAW : cols[i].col);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    return scroll;
}

/* ------------------------------------------------------------------ */
/* Statistics panel                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a vertical statistics panel (legacy/optional layout).
 * @return A framed grid of statistic labels.
 */
GtkWidget *create_stats_panel(void)
{
    GtkWidget *frame = gtk_frame_new("Bus Statistics");
    gtk_widget_set_margin_start(frame, 4);
    gtk_widget_set_margin_end(frame, 4);
    gtk_widget_set_margin_top(frame, 2);
    gtk_widget_set_margin_bottom(frame, 2);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 6);
    gtk_widget_set_margin_bottom(grid, 6);
    gtk_container_add(GTK_CONTAINER(frame), grid);

    /* Helper */
    int row = 0;
#define ADD_ROW(lbl_str, widget) do { \
    GtkWidget *_l = gtk_label_new(lbl_str); \
    gtk_label_set_xalign(GTK_LABEL(_l), 1.0f); \
    gtk_grid_attach(GTK_GRID(grid), _l, 0, row, 1, 1); \
    gtk_grid_attach(GTK_GRID(grid), (widget), 1, row, 1, 1); \
    row++; \
} while(0)

    g_gui.lbl_connection = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_connection), 0.0f);
    ADD_ROW("Interface:", g_gui.lbl_connection);

    g_gui.lbl_bitrate = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_bitrate), 0.0f);
    ADD_ROW("Bitrate:", g_gui.lbl_bitrate);

    g_gui.progress_bus_load = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(
        GTK_PROGRESS_BAR(g_gui.progress_bus_load), TRUE);
    gtk_widget_set_size_request(g_gui.progress_bus_load, 200, -1);
    ADD_ROW("Bus Load:", g_gui.progress_bus_load);

    g_gui.lbl_rx = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_rx), 0.0f);
    ADD_ROW("Rx Frames:", g_gui.lbl_rx);

    g_gui.lbl_tx = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_tx), 0.0f);
    ADD_ROW("Tx Frames:", g_gui.lbl_tx);

    g_gui.lbl_err = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_err), 0.0f);
    ADD_ROW("Error Frames:", g_gui.lbl_err);

    g_gui.lbl_bus_state = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(g_gui.lbl_bus_state), 0.0f);
    ADD_ROW("Bus State:", g_gui.lbl_bus_state);
#undef ADD_ROW

    return frame;
}

/* ------------------------------------------------------------------ */
/* Add message to trace (called from GTK main thread via idle_add)     */
/* ------------------------------------------------------------------ */

void gui_add_message(const can_msg_t *msg)
{
    if (!g_gui.trace_store) return;

    /* Feed the Signal Analysis tab (decodes against the loaded DBC, if any). */
    gui_signal_decode_message(msg);

    /* RX rows roll up by CAN ID; optional dedup mode applies the same to TX. */
    if (should_rollup_message(msg)) {
        GtkTreeIter iter;
        gboolean found = FALSE;
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(g_gui.trace_store), &iter)) {
            do {
                guint row_id = 0;
                guint row_dir = 0;
                gboolean row_ext = FALSE;
                gtk_tree_model_get(GTK_TREE_MODEL(g_gui.trace_store),
                                   &iter,
                                   TCOL_ID_RAW, &row_id,
                                   TCOL_EXT_RAW, &row_ext,
                                   TCOL_DIR_RAW, &row_dir,
                                   -1);
                if (row_id == msg->id &&
                    row_ext == (gboolean)msg->is_extended &&
                    row_dir == (guint)msg->direction) {
                    found = TRUE;

                    guint cnt = 0;
                    gint64 last_ts_ns = 0;
                    gtk_tree_model_get(GTK_TREE_MODEL(g_gui.trace_store),
                                       &iter,
                                       TCOL_COUNT, &cnt,
                                       TCOL_TS_NS, &last_ts_ns,
                                       -1);

                    gint64 ts_ns = timespec_to_ns(&msg->timestamp);
                    gint64 delta_ns = last_ts_ns > 0 ? ts_ns - last_ts_ns : 0;
                    if (delta_ns < 0)
                        delta_ns = 0;

                    char ts[TIME_TEXT_MAX];
                    char data_buf[DATA_TEXT_MAX];
                    char raw_data[RAW_DATA_MAX];
                    char interval_buf[INTERVAL_TEXT_MAX];
                    char freq_buf[FREQ_TEXT_MAX];
                    format_timestamp(ts, sizeof(ts), &msg->timestamp);
                    gui_format_data(data_buf, sizeof(data_buf),
                                    msg->data, msg->dlc);
                    format_raw_data(raw_data, sizeof(raw_data),
                                    msg->data, msg->dlc);
                    format_interval(interval_buf, sizeof(interval_buf),
                                    delta_ns);
                    format_frequency(freq_buf, sizeof(freq_buf), delta_ns);

                    gtk_list_store_set(g_gui.trace_store, &iter,
                        TCOL_TIME,  ts,
                        TCOL_TYPE,  gui_msg_type_str(msg),
                        TCOL_DLC,   (guint)msg->dlc,
                        TCOL_DATA,  data_buf,
                        TCOL_INTERVAL, interval_buf,
                        TCOL_FREQ,  freq_buf,
                        TCOL_COUNT, cnt + 1,
                        TCOL_FG,    row_fg(msg),
                        TCOL_DATA_RAW, raw_data,
                        TCOL_TS_NS, ts_ns,
                        -1);
                    break;
                }
            } while (gtk_tree_model_iter_next(
                         GTK_TREE_MODEL(g_gui.trace_store), &iter));
        }
        if (found) return;
    }

    /* Enforce max rows only when a new visible row is appended. */
    gint n = gtk_tree_model_iter_n_children(
        GTK_TREE_MODEL(g_gui.trace_store), NULL);
    if (n >= MAX_TRACE_ROWS) {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(g_gui.trace_store), &first))
            gtk_list_store_remove(g_gui.trace_store, &first);
    }

    /* Format fields */
    char id_buf[ID_TEXT_MAX];
    char data_buf[DATA_TEXT_MAX];
    char raw_data[RAW_DATA_MAX];
    char ts[TIME_TEXT_MAX];
    char interval_buf[INTERVAL_TEXT_MAX];
    char freq_buf[FREQ_TEXT_MAX];
    gint64 ts_ns = timespec_to_ns(&msg->timestamp);
    gui_format_id(id_buf, sizeof(id_buf), msg->id, msg->is_extended);
    gui_format_data(data_buf, sizeof(data_buf), msg->data, msg->dlc);
    format_raw_data(raw_data, sizeof(raw_data), msg->data, msg->dlc);
    format_timestamp(ts, sizeof(ts), &msg->timestamp);
    format_interval(interval_buf, sizeof(interval_buf), 0);
    format_frequency(freq_buf, sizeof(freq_buf), 0);

    /* Compact #-column: one stable index per visible row.  RX rows roll up by
     * CAN ID, so each unique received ID gets exactly one number (1, 2, 3, …).
     * The counter is reset by gui_clear_trace(), so clearing renumbers from 1. */
    guint64 row_num = ++g_gui.trace_row_seq;

    GtkTreeIter iter;
    gtk_list_store_append(g_gui.trace_store, &iter);
    gtk_list_store_set(g_gui.trace_store, &iter,
        TCOL_SEQ,   row_num,
        TCOL_DIR,   msg->direction == CAN_DIR_TX ? "Tx" : "Rx",
        TCOL_TIME,  ts,
        TCOL_ID,    id_buf,
        TCOL_TYPE,  gui_msg_type_str(msg),
        TCOL_DLC,   (guint)msg->dlc,
        TCOL_DATA,  data_buf,
        TCOL_INTERVAL, interval_buf,
        TCOL_FREQ,  freq_buf,
        TCOL_COUNT, 1u,
        TCOL_FG,    row_fg(msg),
        TCOL_ID_RAW, (guint)msg->id,
        TCOL_EXT_RAW, (gboolean)msg->is_extended,
        TCOL_DIR_RAW, (guint)msg->direction,
        TCOL_DATA_RAW, raw_data,
        TCOL_TS_NS, ts_ns,
        -1);

    /* Auto-scroll to bottom */
    GtkTreePath *path = gtk_tree_model_get_path(
        GTK_TREE_MODEL(g_gui.trace_store), &iter);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_gui.trace_view),
                                  path, NULL, FALSE, 0, 0);
    gtk_tree_path_free(path);
}

/* ------------------------------------------------------------------ */
/* Clear trace                                                          */
/* ------------------------------------------------------------------ */

void gui_clear_trace(void)
{
    if (g_gui.trace_store)
        gtk_list_store_clear(g_gui.trace_store);
    /* Restart the #-column numbering so the next frames begin again at 1. */
    g_gui.trace_row_seq = 0;
}

void gui_refresh_trace_display(void)
{
    if (!g_gui.trace_store) return;

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(
            GTK_TREE_MODEL(g_gui.trace_store), &iter))
        return;

    do {
        guint id = 0;
        guint dlc = 0;
        gboolean is_ext = FALSE;
        gchar *raw = NULL;

        gtk_tree_model_get(GTK_TREE_MODEL(g_gui.trace_store), &iter,
                           TCOL_ID_RAW, &id,
                           TCOL_EXT_RAW, &is_ext,
                           TCOL_DLC, &dlc,
                           TCOL_DATA_RAW, &raw,
                           -1);

        uint8_t data[CANFD_DATA_MAX] = {0};
        if (dlc > CANFD_DATA_MAX)
            dlc = CANFD_DATA_MAX;
        parse_raw_data(raw, data, (uint8_t)dlc);

        char id_buf[ID_TEXT_MAX];
        char data_buf[DATA_TEXT_MAX];
        gui_format_id(id_buf, sizeof(id_buf), id, is_ext);
        gui_format_data(data_buf, sizeof(data_buf), data, (uint8_t)dlc);

        gtk_list_store_set(g_gui.trace_store, &iter,
            TCOL_ID, id_buf,
            TCOL_DATA, data_buf,
            -1);

        g_free(raw);
    } while (gtk_tree_model_iter_next(
                 GTK_TREE_MODEL(g_gui.trace_store), &iter));
}

/* ------------------------------------------------------------------ */
/* Update statistics panel                                              */
/* ------------------------------------------------------------------ */

void gui_update_stats(void)
{
    char connection[64];
    char bitrate[64];
    char bus_load[32];
    char rx_text[32];
    char tx_text[32];
    char err_text[32];
    char summary[256];

    if (g_app.connected) {
        snprintf(connection, sizeof(connection), "%s", g_app.iface);
        snprintf(bitrate, sizeof(bitrate), "%u bps", g_app.bitrate);
    } else {
        snprintf(connection, sizeof(connection), "—");
        snprintf(bitrate, sizeof(bitrate), "—");
    }

    double load = g_app.bus_load;
    snprintf(bus_load, sizeof(bus_load), "%.1f%%", load);

    snprintf(rx_text, sizeof(rx_text), "%llu",
             (unsigned long long)g_app.rx_count);
    snprintf(tx_text, sizeof(tx_text), "%llu",
             (unsigned long long)g_app.tx_count);
    snprintf(err_text, sizeof(err_text), "%llu",
             (unsigned long long)g_app.error_count);

    if (g_gui.lbl_connection)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_connection), connection);
    if (g_gui.lbl_bitrate)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_bitrate), bitrate);

    if (g_gui.progress_bus_load) {
        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(g_gui.progress_bus_load), load / 100.0);
        gtk_progress_bar_set_text(
            GTK_PROGRESS_BAR(g_gui.progress_bus_load), bus_load);
    }
    if (g_gui.lbl_bus_load)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_bus_load), bus_load);

    if (g_gui.lbl_rx)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_rx), rx_text);
    if (g_gui.lbl_tx)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_tx), tx_text);
    if (g_gui.lbl_err)
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_err), err_text);

    const char *state_str   = gui_bus_state_str(g_app.bus_state);
    const char *state_color = gui_bus_state_color(g_app.bus_state);
    char markup[128];
    snprintf(markup, sizeof(markup),
             "<span foreground=\"%s\">%s</span>",
             state_color, state_str);
    if (g_gui.lbl_bus_state)
        gtk_label_set_markup(GTK_LABEL(g_gui.lbl_bus_state), markup);

    if (g_gui.lbl_stats_summary) {
        snprintf(summary, sizeof(summary),
                 "Statistics: %s | Rx %s | Tx %s | Err %s | Load %s",
                 connection, rx_text, tx_text, err_text, bus_load);
        gtk_label_set_text(GTK_LABEL(g_gui.lbl_stats_summary), summary);
    }
}

/* ------------------------------------------------------------------ */
/* Status bar                                                           */
/* ------------------------------------------------------------------ */

void gui_status_message(const char *fmt, ...)
{
    if (!g_gui.statusbar) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_statusbar_pop(GTK_STATUSBAR(g_gui.statusbar),
                      g_gui.statusbar_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(g_gui.statusbar),
                       g_gui.statusbar_ctx, buf);
}

/* ------------------------------------------------------------------ */
/* Error dialog                                                         */
/* ------------------------------------------------------------------ */

void gui_show_error(GtkWidget *parent, const char *title, const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}
