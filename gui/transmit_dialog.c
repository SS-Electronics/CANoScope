/**
 * @file transmit_dialog.c
 * @brief Advanced ("Transmit...") CAN/CAN FD message composer window.
 *
 * @details
 * A non-modal secondary window for hand-crafting a single frame with full
 * control over: identifier (hex) and standard/extended flag, data vs. remote
 * (RTR) frame, CAN FD + BRS, DLC (0..8 classic, 0..64 FD), individual data
 * bytes, and one-shot or cyclic transmission at a configurable interval.
 * Frames are validated by @ref build_frame and pushed onto `g_app.tx_queue`.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/can_message.h"

/* ------------------------------------------------------------------ */
/* Window state                                                         */
/* ------------------------------------------------------------------ */

#define MAX_DATA_ENTRIES 64
#define TX_STD_ID_MAX    0x7FFu
#define TX_EXT_ID_MAX    0x1FFFFFFFu
#define TX_BYTE_MAX      0xFFu

/** @brief Widget state for the advanced transmit window (singleton @ref s_tx). */
typedef struct {
    GtkWidget *window;
    GtkWidget *id_entry;
    GtkWidget *ext_check;
    GtkWidget *rtr_check;
    GtkWidget *fd_check;
    GtkWidget *brs_check;
    GtkWidget *dlc_spin;
    GtkWidget *data_entry[MAX_DATA_ENTRIES];
    GtkWidget *data_grid;
    GtkWidget *interval_spin;
    GtkWidget *cyclic_btn_start;
    GtkWidget *cyclic_btn_stop;
    GtkWidget *status_label;
    guint      cyclic_timer_id;
} tx_win_t;

/** @brief The single advanced-transmit window instance. */
static tx_win_t s_tx;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Strictly parse a hex string into a bounded `uint32_t`.
 * @param s    Input text. @param max Maximum allowed value. @param out Result.
 * @return 0 on success, -1 on malformed input or overflow.
 */
static int parse_hex_u32(const char *s, uint32_t max, uint32_t *out)
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
 * @brief Validate the form fields and assemble a @ref can_msg_t.
 * @param msg  Output frame.
 * @return TRUE if valid (status label cleared), FALSE on a validation error.
 */
static gboolean build_frame(can_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));

    msg->is_extended = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.ext_check));
    msg->is_remote   = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.rtr_check));
    msg->is_fd       = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.fd_check));
    msg->is_brs      = msg->is_fd && gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.brs_check));

    if (msg->is_fd && msg->is_remote) {
        gtk_label_set_markup(GTK_LABEL(s_tx.status_label),
            "<span color='red'>CAN FD does not support RTR frames</span>");
        return FALSE;
    }

    if (msg->is_fd && !g_app.fd_mode) {
        gtk_label_set_markup(GTK_LABEL(s_tx.status_label),
            "<span color='red'>Connection is not in CAN FD mode</span>");
        return FALSE;
    }

    const char *id_str = gtk_entry_get_text(GTK_ENTRY(s_tx.id_entry));
    uint32_t id_max = msg->is_extended ? TX_EXT_ID_MAX : TX_STD_ID_MAX;
    if (parse_hex_u32(id_str, id_max, &msg->id) != 0) {
        gtk_label_set_markup(GTK_LABEL(s_tx.status_label),
            msg->is_extended
                ? "<span color='red'>Invalid 29-bit ID</span>"
                : "<span color='red'>Invalid 11-bit ID</span>");
        return FALSE;
    }

    msg->dlc = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tx.dlc_spin));

    uint8_t max_dlc = msg->is_fd ? 64 : 8;
    if (msg->dlc > max_dlc) msg->dlc = max_dlc;

    if (!msg->is_remote) {
        for (uint8_t i = 0; i < msg->dlc; i++) {
            const char *ds =
                gtk_entry_get_text(GTK_ENTRY(s_tx.data_entry[i]));
            uint32_t v = 0;
            if (strlen(ds) > 0 && parse_hex_u32(ds, TX_BYTE_MAX, &v) != 0) {
                char buf[64];
                snprintf(buf, sizeof(buf),
                         "<span color='red'>Invalid byte at pos %u</span>",
                         i);
                gtk_label_set_markup(GTK_LABEL(s_tx.status_label), buf);
                return FALSE;
            }
            msg->data[i] = (uint8_t)(v & 0xFF);
        }
    }
    return TRUE;
}

/** @brief Build and enqueue the current frame for transmission. */
static void send_frame(void)
{
    if (!g_app.connected) {
        gtk_label_set_markup(GTK_LABEL(s_tx.status_label),
            "<span color='red'>Not connected.</span>");
        return;
    }
    can_msg_t msg;
    if (!build_frame(&msg)) return;

    can_msg_t *copy = malloc(sizeof(can_msg_t));
    if (!copy) return;
    *copy = msg;
    g_async_queue_push(g_app.tx_queue, copy);

    gtk_label_set_markup(GTK_LABEL(s_tx.status_label),
        "<span color='green'>Queued.</span>");
}

/* ------------------------------------------------------------------ */
/* Cyclic send                                                          */
/* ------------------------------------------------------------------ */

/** @brief Cyclic-timer callback: send one frame per tick. @param data Unused. @return Continue/remove. */
static gboolean cyclic_tick(gpointer data)
{
    (void)data;
    if (!g_app.connected || !s_tx.cyclic_timer_id) return G_SOURCE_REMOVE;
    send_frame();
    return G_SOURCE_CONTINUE;
}

/** @brief "Start Cyclic" handler — arm the periodic send timer. @param w Widget. @param d Unused. */
static void on_cyclic_start(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_tx.cyclic_timer_id) return;

    guint interval = (guint)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tx.interval_spin));
    if (interval < 1) interval = 100;

    s_tx.cyclic_timer_id = g_timeout_add(interval, cyclic_tick, NULL);
    gtk_widget_set_sensitive(s_tx.cyclic_btn_start, FALSE);
    gtk_widget_set_sensitive(s_tx.cyclic_btn_stop,  TRUE);
}

/** @brief "Stop Cyclic" handler — disarm the periodic send timer. @param w Widget. @param d Unused. */
static void on_cyclic_stop(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_tx.cyclic_timer_id) {
        g_source_remove(s_tx.cyclic_timer_id);
        s_tx.cyclic_timer_id = 0;
    }
    gtk_widget_set_sensitive(s_tx.cyclic_btn_start, TRUE);
    gtk_widget_set_sensitive(s_tx.cyclic_btn_stop,  FALSE);
    gtk_label_set_text(GTK_LABEL(s_tx.status_label), "Cyclic stopped.");
}

/* ------------------------------------------------------------------ */
/* DLC / FD changed – update data entry sensitivity                    */
/* ------------------------------------------------------------------ */

/** @brief Re-range the DLC spin and enable exactly the active data-byte fields. */
static void update_data_fields(void)
{
    int is_fd  = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.fd_check));
    int is_rtr = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tx.rtr_check));
    int dlc    = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tx.dlc_spin));

    int max_dlc = is_fd ? 64 : 8;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(s_tx.dlc_spin), 0, max_dlc);
    if (dlc > max_dlc)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tx.dlc_spin), max_dlc);
    dlc = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s_tx.dlc_spin));

    gtk_widget_set_sensitive(s_tx.brs_check, is_fd);

    for (int i = 0; i < MAX_DATA_ENTRIES; i++) {
        gboolean en = !is_rtr && (i < dlc) && (i < max_dlc);
        gtk_widget_set_sensitive(s_tx.data_entry[i], en);
    }
}

/** @brief CAN FD toggle handler. @param b Button. @param d Unused. */
static void on_fd_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b; (void)d;
    update_data_fields();
}

/** @brief DLC spin change handler. @param b Spin button. @param d Unused. */
static void on_dlc_changed(GtkSpinButton *b, gpointer d)
{
    (void)b; (void)d;
    update_data_fields();
}

/** @brief Remote-frame toggle handler. @param b Button. @param d Unused. */
static void on_rtr_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b; (void)d;
    update_data_fields();
}

/* ------------------------------------------------------------------ */
/* Window construction                                                  */
/* ------------------------------------------------------------------ */

/** @brief "Send Once" handler. @param w Widget. @param d Unused. */
static void on_send_once(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    send_frame();
}

/** @brief Window destroy handler — cancel the cyclic timer and clear state. @param w Widget. @param d Unused. */
static void on_tx_win_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_tx.cyclic_timer_id) {
        g_source_remove(s_tx.cyclic_timer_id);
        s_tx.cyclic_timer_id = 0;
    }
    s_tx.window = NULL;
}

void gui_show_transmit_window(void)
{
    if (s_tx.window) {
        gtk_window_present(GTK_WINDOW(s_tx.window));
        return;
    }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    s_tx.window = win;
    gtk_window_set_title(GTK_WINDOW(win), "Transmit CAN Message");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 420);
    if (g_gui.window)
        gtk_window_set_transient_for(GTK_WINDOW(win),
                                     GTK_WINDOW(g_gui.window));
    g_signal_connect(win, "destroy", G_CALLBACK(on_tx_win_destroy), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(win), outer);

    /* --- Frame configuration --- */
    GtkWidget *frm_frame = gtk_frame_new("Frame");
    gtk_box_pack_start(GTK_BOX(outer), frm_frame, FALSE, FALSE, 0);
    GtkWidget *frm_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(frm_grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(frm_grid), 6);
    gtk_widget_set_margin_start(frm_grid, 8);
    gtk_widget_set_margin_end(frm_grid, 8);
    gtk_widget_set_margin_top(frm_grid, 6);
    gtk_widget_set_margin_bottom(frm_grid, 6);
    gtk_container_add(GTK_CONTAINER(frm_frame), frm_grid);

    int r = 0;
#define FLABEL(s) do { \
    GtkWidget *_l = gtk_label_new(s); \
    gtk_label_set_xalign(GTK_LABEL(_l), 1.0f); \
    gtk_grid_attach(GTK_GRID(frm_grid), _l, 0, r, 1, 1); \
} while(0)

    /* ID */
    FLABEL("ID (hex):");
    s_tx.id_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(s_tx.id_entry), 8);
    gtk_entry_set_text(GTK_ENTRY(s_tx.id_entry), "001");
    gtk_entry_set_width_chars(GTK_ENTRY(s_tx.id_entry), 10);
    GtkWidget *id_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(id_box), s_tx.id_entry, FALSE, FALSE, 0);
    s_tx.ext_check = gtk_check_button_new_with_label("Extended (29-bit)");
    gtk_box_pack_start(GTK_BOX(id_box), s_tx.ext_check, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(frm_grid), id_box, 1, r++, 1, 1);

    /* Type */
    FLABEL("Type:");
    s_tx.rtr_check = gtk_check_button_new_with_label("Remote Frame (RTR)");
    g_signal_connect(s_tx.rtr_check, "toggled",
                     G_CALLBACK(on_rtr_toggled), NULL);
    gtk_grid_attach(GTK_GRID(frm_grid), s_tx.rtr_check, 1, r++, 1, 1);

    /* CAN FD */
    FLABEL("CAN FD:");
    GtkWidget *fd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    s_tx.fd_check = gtk_check_button_new_with_label("FD Frame");
    g_signal_connect(s_tx.fd_check, "toggled",
                     G_CALLBACK(on_fd_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(fd_box), s_tx.fd_check, FALSE, FALSE, 0);
    s_tx.brs_check = gtk_check_button_new_with_label("BRS");
    gtk_widget_set_sensitive(s_tx.brs_check, FALSE);
    gtk_box_pack_start(GTK_BOX(fd_box), s_tx.brs_check, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(frm_grid), fd_box, 1, r++, 1, 1);

    /* DLC */
    FLABEL("DLC:");
    GtkAdjustment *dlc_adj = gtk_adjustment_new(8, 0, 8, 1, 1, 0);
    s_tx.dlc_spin = gtk_spin_button_new(dlc_adj, 1, 0);
    g_signal_connect(s_tx.dlc_spin, "value-changed",
                     G_CALLBACK(on_dlc_changed), NULL);
    gtk_grid_attach(GTK_GRID(frm_grid), s_tx.dlc_spin, 1, r++, 1, 1);
#undef FLABEL

    /* --- Data bytes --- */
    GtkWidget *data_frame = gtk_frame_new("Data (hex)");
    gtk_box_pack_start(GTK_BOX(outer), data_frame, TRUE, TRUE, 0);

    GtkWidget *data_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(data_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(data_scroll, -1, 80);
    gtk_container_add(GTK_CONTAINER(data_frame), data_scroll);

    s_tx.data_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(s_tx.data_grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(s_tx.data_grid), 4);
    gtk_widget_set_margin_start(s_tx.data_grid, 6);
    gtk_widget_set_margin_top(s_tx.data_grid, 6);
    gtk_container_add(GTK_CONTAINER(data_scroll), s_tx.data_grid);

    for (int i = 0; i < MAX_DATA_ENTRIES; i++) {
        GtkWidget *lbl = gtk_label_new(NULL);
        char lbuf[8];
        snprintf(lbuf, sizeof(lbuf), "[%d]", i);
        gtk_label_set_text(GTK_LABEL(lbl), lbuf);
        gtk_grid_attach(GTK_GRID(s_tx.data_grid), lbl,
                        (i % 8) * 2, i / 8, 1, 1);

        s_tx.data_entry[i] = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(s_tx.data_entry[i]), 2);
        gtk_entry_set_text(GTK_ENTRY(s_tx.data_entry[i]), "00");
        gtk_entry_set_width_chars(GTK_ENTRY(s_tx.data_entry[i]), 3);
        gtk_grid_attach(GTK_GRID(s_tx.data_grid), s_tx.data_entry[i],
                        (i % 8) * 2 + 1, i / 8, 1, 1);

        if (i >= 8)
            gtk_widget_set_sensitive(s_tx.data_entry[i], FALSE);
    }

    /* --- Cyclic send --- */
    GtkWidget *cyc_frame = gtk_frame_new("Cyclic Transmit");
    gtk_box_pack_start(GTK_BOX(outer), cyc_frame, FALSE, FALSE, 0);
    GtkWidget *cyc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(cyc_box, 8);
    gtk_widget_set_margin_end(cyc_box, 8);
    gtk_widget_set_margin_top(cyc_box, 6);
    gtk_widget_set_margin_bottom(cyc_box, 6);
    gtk_container_add(GTK_CONTAINER(cyc_frame), cyc_box);

    gtk_box_pack_start(GTK_BOX(cyc_box),
                       gtk_label_new("Interval (ms):"), FALSE, FALSE, 0);
    GtkAdjustment *int_adj = gtk_adjustment_new(100, 1, 60000, 10, 100, 0);
    s_tx.interval_spin = gtk_spin_button_new(int_adj, 10, 0);
    gtk_box_pack_start(GTK_BOX(cyc_box),
                       s_tx.interval_spin, FALSE, FALSE, 0);

    s_tx.cyclic_btn_start = gtk_button_new_with_label("Start Cyclic");
    g_signal_connect(s_tx.cyclic_btn_start, "clicked",
                     G_CALLBACK(on_cyclic_start), NULL);
    gtk_box_pack_start(GTK_BOX(cyc_box),
                       s_tx.cyclic_btn_start, FALSE, FALSE, 0);

    s_tx.cyclic_btn_stop = gtk_button_new_with_label("Stop Cyclic");
    g_signal_connect(s_tx.cyclic_btn_stop, "clicked",
                     G_CALLBACK(on_cyclic_stop), NULL);
    gtk_widget_set_sensitive(s_tx.cyclic_btn_stop, FALSE);
    gtk_box_pack_start(GTK_BOX(cyc_box),
                       s_tx.cyclic_btn_stop, FALSE, FALSE, 0);

    /* --- Bottom buttons --- */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), btn_box, FALSE, FALSE, 0);

    GtkWidget *send_btn = gtk_button_new_with_label("Send Once");
    g_signal_connect(send_btn, "clicked",
                     G_CALLBACK(on_send_once), NULL);
    gtk_box_pack_start(GTK_BOX(btn_box), send_btn, FALSE, FALSE, 0);

    s_tx.status_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(btn_box), s_tx.status_label,
                       FALSE, FALSE, 0);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close_btn, "clicked",
                              G_CALLBACK(gtk_widget_destroy), win);
    gtk_box_pack_end(GTK_BOX(btn_box), close_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
}
