/**
 * @file settings_dialog.c
 * @brief Modal CAN connection-settings dialog.
 *
 * @details
 * Presents the connection options and, on confirmation, writes them into
 * @ref g_app and triggers @ref app_do_connect.  The user can choose:
 *   - the CAN interface (auto-discovered from `/sys/class/net`, with `vcan0`
 *     always offered as a fallback, and a Refresh button to re-scan),
 *   - the nominal bit rate (a preset, or a custom rate typed into the entry),
 *   - CAN FD mode and its data bit rate,
 *   - listen-only mode,
 *   - an optional acceptance filter (ID + mask, standard or extended).
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/app_state.h"
#include "../inc/socketcan.h"

/* ------------------------------------------------------------------ */
/* Interface discovery                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Fill the interface combo with discovered CAN interfaces.
 *
 * Always lists `vcan0` first, then any other CAN interfaces, and pre-selects
 * the one currently stored in @ref g_app.
 * @param combo  The combo box to populate.
 */
static void populate_interfaces(GtkComboBoxText *combo)
{
#define MAX_IF 32
    char names[MAX_IF][SOCKETCAN_MAX_IFACE];
    int n = socketcan_list_interfaces(names, MAX_IF);

    /* Start from a clean list so this is safe to call again from Refresh. */
    gtk_combo_box_text_remove_all(combo);

    /* Always add vcan0 as the first / fallback option */
    gtk_combo_box_text_append_text(combo, "vcan0");

    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], "vcan0") != 0)
            gtk_combo_box_text_append_text(combo, names[i]);
    }

    /* Pre-select the interface currently stored in g_app */
    GtkTreeModel *model =
        gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    gboolean valid =
        gtk_tree_model_get_iter_first(model, &iter);
    int idx = 0, sel = 0;
    while (valid) {
        gchar *txt = NULL;
        gtk_tree_model_get(model, &iter, 0, &txt, -1);
        if (txt && strcmp(txt, g_app.iface) == 0) sel = idx;
        g_free(txt);
        valid = gtk_tree_model_iter_next(model, &iter);
        idx++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), sel);
#undef MAX_IF
}

/* ------------------------------------------------------------------ */
/* Dialog                                                               */
/* ------------------------------------------------------------------ */

static struct {
    const char *label;
    uint32_t    value;
} s_bitrates[] = {  /**< Selectable nominal bit rates. */
    { "10 kbit/s",   10000   },
    { "20 kbit/s",   20000   },
    { "50 kbit/s",   50000   },
    { "100 kbit/s", 100000   },
    { "125 kbit/s", 125000   },
    { "250 kbit/s", 250000   },
    { "500 kbit/s", 500000   },
    { "800 kbit/s", 800000   },
    { "1 Mbit/s",  1000000   },
};

static struct {
    const char *label;
    uint32_t    value;
} s_fd_bitrates[] = {  /**< Selectable CAN FD data bit rates. */
    { "1 Mbit/s",   1000000  },
    { "2 Mbit/s",   2000000  },
    { "4 Mbit/s",   4000000  },
    { "5 Mbit/s",   5000000  },
    { "8 Mbit/s",   8000000  },
    { "10 Mbit/s", 10000000  },
    { "12 Mbit/s", 12000000  },
};

/**
 * @brief "Enable CAN FD" toggle handler — enables the data-rate widget.
 * @param btn   The CAN FD check button.
 * @param data  The data-bitrate widget to (de)sensitise.
 */
static void on_fd_toggled(GtkToggleButton *btn, gpointer data)
{
    GtkWidget *drate_box = GTK_WIDGET(data);
    gtk_widget_set_sensitive(drate_box,
                             gtk_toggle_button_get_active(btn));
}

/**
 * @brief "Refresh" handler — re-scan and rebuild the interface list.
 * @param btn   The refresh button.
 * @param data  The interface @c GtkComboBoxText to repopulate.
 */
static void on_refresh_interfaces(GtkWidget *btn, gpointer data)
{
    (void)btn;
    populate_interfaces(GTK_COMBO_BOX_TEXT(data));
}

/**
 * @brief Enable/disable the acceptance-filter widgets with their checkbox.
 * @param btn   The "Enable acceptance filter" check button.
 * @param data  A box containing the ID/mask/extended widgets.
 */
static void on_filter_toggled(GtkToggleButton *btn, gpointer data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(data),
                             gtk_toggle_button_get_active(btn));
}

/**
 * @brief Parse a human-friendly bit-rate string into bit/s.
 *
 * Accepts plain numbers ("250000"), `k`/`M` suffixes ("250k", "1 Mbit/s"), and
 * the preset labels.  A bare number below 10000 is treated as kbit/s.
 * @param s  Input text (may be NULL).
 * @return Bit rate in bit/s, or 0 if it cannot be parsed.
 */
static uint32_t parse_bitrate(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') s++;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0.0)
        return 0;
    while (*end == ' ') end++;
    if (*end == 'M' || *end == 'm')
        v *= 1e6;
    else if (*end == 'k' || *end == 'K')
        v *= 1e3;
    else if (v < 10000.0)
        v *= 1e3; /* bare small number → interpret as kbit/s */
    return (uint32_t)(v + 0.5);
}

void gui_show_settings_dialog(GtkWidget *parent)
{
    if (g_app.connected) {
        /* Ask user to disconnect first */
        GtkWidget *ask = gtk_message_dialog_new(
            parent ? GTK_WINDOW(parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_YES_NO,
            "Disconnect from %s before changing settings?",
            g_app.iface);
        gint r = gtk_dialog_run(GTK_DIALOG(ask));
        gtk_widget_destroy(ask);
        if (r == GTK_RESPONSE_YES)
            app_do_disconnect();
        else
            return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Connection Settings",
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Connect", GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    int row = 0;
#define GRID_LABEL(str) do { \
    GtkWidget *_l = gtk_label_new(str); \
    gtk_label_set_xalign(GTK_LABEL(_l), 1.0f); \
    gtk_grid_attach(GTK_GRID(grid), _l, 0, row, 1, 1); \
} while(0)

    /* --- Interface (with a Refresh button to re-scan) --- */
    GRID_LABEL("Interface:");
    GtkWidget *iface_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *iface_combo = GTK_WIDGET(gtk_combo_box_text_new_with_entry());
    populate_interfaces(GTK_COMBO_BOX_TEXT(iface_combo));
    gtk_box_pack_start(GTK_BOX(iface_box), iface_combo, TRUE, TRUE, 0);
    GtkWidget *refresh_btn = gtk_button_new_from_icon_name(
        "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh_btn, "Re-scan CAN interfaces");
    g_signal_connect(refresh_btn, "clicked",
                     G_CALLBACK(on_refresh_interfaces), iface_combo);
    gtk_box_pack_start(GTK_BOX(iface_box), refresh_btn, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), iface_box, 1, row++, 1, 1);

    /* --- Bitrate (editable: pick a preset or type a custom rate) --- */
    GRID_LABEL("Nominal Bitrate:");
    GtkWidget *brate_combo = GTK_WIDGET(gtk_combo_box_text_new_with_entry());
    int brate_sel = 6; /* default 500k */
    for (size_t i = 0; i < sizeof(s_bitrates)/sizeof(s_bitrates[0]); i++) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(brate_combo), s_bitrates[i].label);
        if (s_bitrates[i].value == g_app.bitrate) brate_sel = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(brate_combo), brate_sel);
    gtk_grid_attach(GTK_GRID(grid), brate_combo, 1, row++, 1, 1);

    /* --- Listen Only --- */
    GRID_LABEL("Mode:");
    GtkWidget *lo_check = gtk_check_button_new_with_label("Listen Only");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lo_check),
                                  g_app.listen_only);
    gtk_grid_attach(GTK_GRID(grid), lo_check, 1, row++, 1, 1);

    /* --- CAN FD --- */
    GRID_LABEL("CAN FD:");
    GtkWidget *fd_check = gtk_check_button_new_with_label("Enable CAN FD");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fd_check),
                                  g_app.fd_mode);
    gtk_grid_attach(GTK_GRID(grid), fd_check, 1, row++, 1, 1);

    /* --- Data Bitrate (FD) --- */
    GRID_LABEL("Data Bitrate (FD):");
    GtkWidget *drate_combo = GTK_WIDGET(gtk_combo_box_text_new());
    int drate_sel = 1; /* default 2 Mbit/s */
    for (size_t i = 0; i < sizeof(s_fd_bitrates)/sizeof(s_fd_bitrates[0]); i++) {
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(drate_combo), s_fd_bitrates[i].label);
        if (s_fd_bitrates[i].value == g_app.data_bitrate)
            drate_sel = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(drate_combo), drate_sel);
    gtk_widget_set_sensitive(drate_combo, g_app.fd_mode);
    gtk_grid_attach(GTK_GRID(grid), drate_combo, 1, row++, 1, 1);

    g_signal_connect(fd_check, "toggled",
                     G_CALLBACK(on_fd_toggled), drate_combo);

    /* --- Acceptance filter (optional ID/mask) --- */
    GRID_LABEL("Filter:");
    GtkWidget *filt_check =
        gtk_check_button_new_with_label("Enable acceptance filter");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(filt_check),
                                 g_app.filter_enabled);
    gtk_grid_attach(GTK_GRID(grid), filt_check, 1, row++, 1, 1);

    GtkWidget *filt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(filt_box), gtk_label_new("ID 0x"),
                       FALSE, FALSE, 0);
    GtkWidget *filt_id = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(filt_id), 8);
    GtkWidget *filt_mask = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(filt_mask), 8);
    if (g_app.filter_enabled) {
        char idbuf[16], mbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%X", g_app.filter_id);
        snprintf(mbuf,  sizeof(mbuf),  "%X", g_app.filter_mask);
        gtk_entry_set_text(GTK_ENTRY(filt_id),   idbuf);
        gtk_entry_set_text(GTK_ENTRY(filt_mask), mbuf);
    }
    gtk_box_pack_start(GTK_BOX(filt_box), filt_id, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filt_box), gtk_label_new("Mask 0x"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filt_box), filt_mask, FALSE, FALSE, 0);
    GtkWidget *filt_ext = gtk_check_button_new_with_label("Ext");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(filt_ext),
                                 g_app.filter_ext);
    gtk_box_pack_start(GTK_BOX(filt_box), filt_ext, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(filt_box, g_app.filter_enabled);
    gtk_grid_attach(GTK_GRID(grid), filt_box, 1, row++, 1, 1);
    g_signal_connect(filt_check, "toggled",
                     G_CALLBACK(on_filter_toggled), filt_box);

#undef GRID_LABEL

    /* --- Tip label --- */
    GtkWidget *tip = gtk_label_new(
        "<small><i>Tip: select <b>vcan0</b> for off-hardware testing, or type a "
        "custom bit rate (e.g. 33.3k). Missing vcan/CAN interfaces are created "
        "and brought up automatically (a graphical authentication prompt may "
        "appear — no terminal sudo needed). The acceptance filter passes only "
        "frames where <tt>(id &amp; mask) == (filter &amp; mask)</tt>.</i></small>");
    gtk_label_set_use_markup(GTK_LABEL(tip), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(tip), TRUE);
    gtk_widget_set_margin_top(tip, 8);
    gtk_box_pack_start(GTK_BOX(content), tip, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        /* Read interface */
        const gchar *iface_text =
            gtk_entry_get_text(GTK_ENTRY(
                gtk_bin_get_child(GTK_BIN(iface_combo))));
        strncpy(g_app.iface, iface_text, APP_MAX_IFACE_LEN - 1);

        /* Read bitrate: a chosen preset wins, otherwise parse the typed text
         * so a custom rate (e.g. "33.3k", "666000") can be entered. */
        int bi = gtk_combo_box_get_active(GTK_COMBO_BOX(brate_combo));
        if (bi >= 0 && bi < (int)(sizeof(s_bitrates)/sizeof(s_bitrates[0]))) {
            g_app.bitrate = s_bitrates[bi].value;
        } else {
            const gchar *brate_text = gtk_entry_get_text(GTK_ENTRY(
                gtk_bin_get_child(GTK_BIN(brate_combo))));
            uint32_t custom = parse_bitrate(brate_text);
            if (custom > 0)
                g_app.bitrate = custom;
        }

        /* Read FD mode */
        g_app.fd_mode = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(fd_check));

        /* Read data bitrate */
        int di = gtk_combo_box_get_active(GTK_COMBO_BOX(drate_combo));
        if (di >= 0 &&
            di < (int)(sizeof(s_fd_bitrates)/sizeof(s_fd_bitrates[0])))
            g_app.data_bitrate = s_fd_bitrates[di].value;

        /* Read listen-only */
        g_app.listen_only = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(lo_check));

        /* Read acceptance filter */
        g_app.filter_enabled = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(filt_check));
        if (g_app.filter_enabled) {
            g_app.filter_id = (uint32_t)strtoul(
                gtk_entry_get_text(GTK_ENTRY(filt_id)), NULL, 16);
            g_app.filter_mask = (uint32_t)strtoul(
                gtk_entry_get_text(GTK_ENTRY(filt_mask)), NULL, 16);
            g_app.filter_ext = gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(filt_ext));
        }

        gtk_widget_destroy(dlg);
        app_do_connect();
    } else {
        gtk_widget_destroy(dlg);
    }
}
