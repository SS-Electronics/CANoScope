/**
 * @file db_creation.c
 * @brief DB Creation tab for building/updating DBC files from live RX rows.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/dbc.h"

#define RAW_TEXT_MAX ((CANFD_DATA_MAX * 2u) + 1u)

enum {
    MCOL_LABEL = 0,
    MCOL_ID,
    MCOL_EXT,
    MCOL_DLC,
    MCOL_RAW,
    MCOL_NUM
};

static struct {
    GtkWidget    *target_entry;
    GtkWidget    *msg_combo;
    GtkListStore *msg_store;
    GtkWidget    *rx_count_label;
    GtkWidget    *message_name_entry;
    GtkWidget    *signal_name_entry;
    GtkWidget    *start_spin;
    GtkWidget    *length_spin;
    GtkWidget    *endian_combo;
    GtkWidget    *signed_check;
    GtkWidget    *factor_spin;
    GtkWidget    *offset_spin;
    GtkWidget    *min_spin;
    GtkWidget    *max_spin;
    GtkWidget    *unit_entry;
    GtkWidget    *sample_data_label;
    GtkWidget    *sample_raw_label;
    GtkWidget    *sample_value_label;
    GtkWidget    *status_label;
    guint         refresh_source;
    gboolean      preserve_form_on_refresh;
} s_tab;

static void db_creation_refresh_messages(void);
static void update_sample_preview(void);

static void set_status(const char *fmt, ...)
{
    if (!s_tab.status_label)
        return;

    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(s_tab.status_label), text);
}

static void fmt_double(char *buf, size_t sz, double v)
{
    snprintf(buf, sz, "%.6f", v);
    char *dot = strchr(buf, '.');
    if (!dot)
        return;
    char *end = buf + strlen(buf) - 1;
    while (end > dot && *end == '0')
        *end-- = '\0';
    if (end == dot)
        *end = '\0';
}

static void format_raw_data(char *buf, size_t sz,
                            const uint8_t *data, uint8_t dlc)
{
    if (sz == 0)
        return;
    buf[0] = '\0';
    size_t pos = 0;
    for (uint8_t i = 0; i < dlc && pos + 3 <= sz; i++)
        pos += (size_t)snprintf(buf + pos, sz - pos, "%02X", data[i]);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_raw_data(const char *raw, uint8_t *out, uint8_t dlc)
{
    if (!raw || !out)
        return;
    for (uint8_t i = 0; i < dlc && raw[i * 2] && raw[i * 2 + 1]; i++) {
        int hi = hex_value(raw[i * 2]);
        int lo = hex_value(raw[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            break;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

static void default_message_name(char *buf, size_t sz,
                                 uint32_t id, int is_ext)
{
    if (is_ext)
        snprintf(buf, sz, "Msg_%08X", id);
    else
        snprintf(buf, sz, "Msg_%03X", id);
}

static gboolean normalize_name(const char *input, const char *fallback,
                               char out[DBC_NAME_MAX])
{
    const char *src = input;
    while (src && isspace((unsigned char)*src))
        src++;
    if (!src || !*src)
        src = fallback;
    if (!src || !*src)
        return FALSE;

    size_t pos = 0;
    if (!(isalpha((unsigned char)*src) || *src == '_'))
        out[pos++] = '_';

    for (; *src && pos + 1 < DBC_NAME_MAX; src++) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '_')
            out[pos++] = (char)c;
        else if (isspace(c) || c == '-' || c == '.')
            out[pos++] = '_';
    }

    while (pos > 0 && out[pos - 1] == '_')
        pos--;
    out[pos] = '\0';
    return pos > 0;
}

static gboolean get_selected_message(uint32_t *id,
                                     int *is_ext,
                                     uint8_t *dlc,
                                     uint8_t data[CANFD_DATA_MAX],
                                     char *data_text,
                                     size_t data_text_sz)
{
    if (!s_tab.msg_combo)
        return FALSE;

    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(GTK_COMBO_BOX(s_tab.msg_combo), &iter))
        return FALSE;

    guint row_id = 0, row_dlc = 0;
    gboolean row_ext = FALSE;
    gchar *raw = NULL;
    gchar *label = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s_tab.msg_store), &iter,
                       MCOL_ID, &row_id,
                       MCOL_EXT, &row_ext,
                       MCOL_DLC, &row_dlc,
                       MCOL_RAW, &raw,
                       MCOL_LABEL, &label,
                       -1);

    if (id)
        *id = row_id;
    if (is_ext)
        *is_ext = row_ext ? 1 : 0;
    if (dlc)
        *dlc = (uint8_t)(row_dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : row_dlc);
    if (data) {
        memset(data, 0, CANFD_DATA_MAX);
        parse_raw_data(raw, data,
                       (uint8_t)(row_dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : row_dlc));
    }
    if (data_text && data_text_sz > 0)
        snprintf(data_text, data_text_sz, "%s", label ? label : "");

    g_free(raw);
    g_free(label);
    return TRUE;
}

static gboolean signal_fits_payload(uint16_t start_bit, uint16_t length,
                                    int little_endian, uint8_t dlc)
{
    if (length == 0 || length > 64)
        return FALSE;

    uint16_t total = (uint16_t)dlc * 8u;
    if (total == 0 || start_bit >= total)
        return FALSE;

    if (little_endian)
        return (uint32_t)start_bit + (uint32_t)length <= total;

    int bit = start_bit;
    for (uint16_t i = 0; i < length; i++) {
        if (bit < 0 || bit >= total)
            return FALSE;
        if ((bit & 7) == 0)
            bit += 15;
        else
            bit -= 1;
    }
    return TRUE;
}

static void selected_message_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    (void)data;

    uint32_t id = 0;
    int is_ext = 0;
    uint8_t dlc = 0;
    if (!get_selected_message(&id, &is_ext, &dlc, NULL, NULL, 0)) {
        gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), "-");
        update_sample_preview();
        return;
    }

    guint total_bits = dlc ? (guint)dlc * 8u : 1u;
    GtkAdjustment *start_adj = gtk_spin_button_get_adjustment(
        GTK_SPIN_BUTTON(s_tab.start_spin));
    gtk_adjustment_set_upper(start_adj, total_bits - 1u);
    if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s_tab.start_spin)) >=
        (int)total_bits)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.start_spin), 0);

    GtkAdjustment *len_adj = gtk_spin_button_get_adjustment(
        GTK_SPIN_BUTTON(s_tab.length_spin));
    gtk_adjustment_set_upper(len_adj, total_bits < 64u ? total_bits : 64u);
    if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s_tab.length_spin)) >
        (int)(total_bits < 64u ? total_bits : 64u))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.length_spin),
                                  total_bits < 64u ? total_bits : 64u);

    if (!s_tab.preserve_form_on_refresh) {
        char fallback[DBC_NAME_MAX];
        default_message_name(fallback, sizeof(fallback), id, is_ext);

        const char *path = gtk_entry_get_text(GTK_ENTRY(s_tab.target_entry));
        gboolean used_existing_name = FALSE;
        if (path && *path && g_file_test(path, G_FILE_TEST_EXISTS)) {
            char err[256] = {0};
            dbc_db_t *db = dbc_load_file(path, err, sizeof(err));
            const dbc_message_t *m = dbc_find_message(db, id, is_ext);
            if (m) {
                gtk_entry_set_text(GTK_ENTRY(s_tab.message_name_entry), m->name);
                used_existing_name = TRUE;
            }
            dbc_free(db);
        }
        if (!used_existing_name)
            gtk_entry_set_text(GTK_ENTRY(s_tab.message_name_entry), fallback);
    }

    update_sample_preview();
}

static void update_sample_preview(void)
{
    if (!s_tab.sample_raw_label || !s_tab.sample_value_label)
        return;

    uint32_t id = 0;
    int is_ext = 0;
    uint8_t dlc = 0;
    uint8_t data[CANFD_DATA_MAX] = {0};
    char label[GUI_TRACE_DATA_TEXT_MAX] = {0};
    if (!get_selected_message(&id, &is_ext, &dlc, data, label, sizeof(label))) {
        gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), "-");
        return;
    }
    (void)id;
    (void)is_ext;

    gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), label);

    dbc_signal_t sig;
    memset(&sig, 0, sizeof(sig));
    sig.start_bit = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tab.start_spin));
    sig.length = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tab.length_spin));
    sig.little_endian = gtk_combo_box_get_active(
        GTK_COMBO_BOX(s_tab.endian_combo)) == 0;
    sig.is_signed = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tab.signed_check)) ? 1 : 0;
    sig.factor = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(s_tab.factor_spin));
    sig.offset = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(s_tab.offset_spin));

    if (!signal_fits_payload(sig.start_bit, sig.length,
                             sig.little_endian, dlc)) {
        gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), "-");
        return;
    }

    int64_t sraw = 0;
    uint64_t raw = dbc_extract_raw(data, dlc, &sig);
    double phys = dbc_decode_physical(&sig, raw, &sraw);

    char raw_buf[32];
    char val_buf[64];
    snprintf(raw_buf, sizeof(raw_buf), "%lld", (long long)sraw);
    fmt_double(val_buf, sizeof(val_buf), phys);
    gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), raw_buf);
    gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), val_buf);
}

static void signal_form_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    update_sample_preview();
}

static gboolean refresh_timer_cb(gpointer data)
{
    (void)data;
    s_tab.refresh_source = 0;
    db_creation_refresh_messages();
    return G_SOURCE_REMOVE;
}

void gui_db_creation_trace_changed(void)
{
    if (!s_tab.msg_store)
        return;
    if (!s_tab.refresh_source)
        s_tab.refresh_source = g_timeout_add(250, refresh_timer_cb, NULL);
}

static void db_creation_refresh_messages(void)
{
    if (!s_tab.msg_store)
        return;

    uint32_t old_id = 0;
    int old_ext = 0;
    gboolean had_selection =
        get_selected_message(&old_id, &old_ext, NULL, NULL, NULL, 0);

    size_t total = gui_trace_collect_rx_messages(NULL, 0);
    gui_trace_rx_message_t *rows = NULL;
    if (total > 0) {
        rows = g_new0(gui_trace_rx_message_t, total);
        gui_trace_collect_rx_messages(rows, total);
    }

    gtk_list_store_clear(s_tab.msg_store);

    int active = -1;
    for (size_t i = 0; i < total; i++) {
        char raw[RAW_TEXT_MAX];
        format_raw_data(raw, sizeof(raw), rows[i].data, rows[i].dlc);

        char label[GUI_TRACE_DATA_TEXT_MAX + 96];
        snprintf(label, sizeof(label),
                 "%s  %s  DLC %u  Data %s  Count %u",
                 rows[i].id_text,
                 rows[i].is_extended ? "Ext" : "Std",
                 rows[i].dlc,
                 rows[i].data_text[0] ? rows[i].data_text : "-",
                 rows[i].count);

        GtkTreeIter iter;
        gtk_list_store_append(s_tab.msg_store, &iter);
        gtk_list_store_set(s_tab.msg_store, &iter,
                           MCOL_LABEL, label,
                           MCOL_ID, (guint)rows[i].id,
                           MCOL_EXT, (gboolean)rows[i].is_extended,
                           MCOL_DLC, (guint)rows[i].dlc,
                           MCOL_RAW, raw,
                           -1);

        if (had_selection &&
            old_id == rows[i].id &&
            old_ext == (int)rows[i].is_extended)
            active = (int)i;
    }

    if (s_tab.rx_count_label) {
        char txt[64];
        snprintf(txt, sizeof(txt), "%zu RX message%s",
                 total, total == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(s_tab.rx_count_label), txt);
    }

    if (total > 0) {
        s_tab.preserve_form_on_refresh = had_selection;
        gtk_combo_box_set_active(GTK_COMBO_BOX(s_tab.msg_combo),
                                 active >= 0 ? active : 0);
        s_tab.preserve_form_on_refresh = FALSE;
    } else {
        gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), "-");
    }

    g_free(rows);
}

static void on_refresh_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    db_creation_refresh_messages();
}

static void add_dbc_filter(GtkWidget *dlg)
{
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ff, "*.dbc");
    gtk_file_filter_set_name(ff, "CAN database (*.dbc)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
}

static void on_new_db(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Create CAN Database (DBC)",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                      "generated.dbc");
    add_dbc_filter(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            gtk_entry_set_text(GTK_ENTRY(s_tab.target_entry), fn);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_open_db(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open CAN Database (DBC)",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);
    add_dbc_filter(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) {
            gtk_entry_set_text(GTK_ENTRY(s_tab.target_entry), fn);
            selected_message_changed(GTK_COMBO_BOX(s_tab.msg_combo), NULL);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_use_loaded_db(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    const char *path = gui_signal_get_dbc_path();
    if (!path || !*path) {
        set_status("No DBC is currently loaded in Signal Analysis.");
        return;
    }
    gtk_entry_set_text(GTK_ENTRY(s_tab.target_entry), path);
    selected_message_changed(GTK_COMBO_BOX(s_tab.msg_combo), NULL);
}

static gboolean build_signal_from_form(dbc_signal_t *sig,
                                       uint8_t dlc,
                                       char *err,
                                       size_t errsz)
{
    memset(sig, 0, sizeof(*sig));

    const char *raw_name = gtk_entry_get_text(
        GTK_ENTRY(s_tab.signal_name_entry));
    if (!normalize_name(raw_name, "Signal", sig->name)) {
        snprintf(err, errsz, "Signal name is empty.");
        return FALSE;
    }

    sig->start_bit = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tab.start_spin));
    sig->length = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_tab.length_spin));
    sig->little_endian = gtk_combo_box_get_active(
        GTK_COMBO_BOX(s_tab.endian_combo)) == 0;
    sig->is_signed = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_tab.signed_check)) ? 1 : 0;
    sig->factor = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(s_tab.factor_spin));
    sig->offset = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(s_tab.offset_spin));
    sig->min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s_tab.min_spin));
    sig->max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s_tab.max_spin));

    const char *unit = gtk_entry_get_text(GTK_ENTRY(s_tab.unit_entry));
    snprintf(sig->unit, sizeof(sig->unit), "%s", unit ? unit : "");

    if (!signal_fits_payload(sig->start_bit, sig->length,
                             sig->little_endian, dlc)) {
        snprintf(err, errsz,
                 "Signal bits do not fit inside the selected message DLC.");
        return FALSE;
    }

    return TRUE;
}

static void on_save_signal(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;

    uint32_t id = 0;
    int is_ext = 0;
    uint8_t dlc = 0;
    if (!get_selected_message(&id, &is_ext, &dlc, NULL, NULL, 0)) {
        set_status("No RX message is selected.");
        return;
    }

    const char *path = gtk_entry_get_text(GTK_ENTRY(s_tab.target_entry));
    if (!path || !*path) {
        set_status("Select a target DBC file first.");
        return;
    }

    char msg_fallback[DBC_NAME_MAX];
    char msg_name[DBC_NAME_MAX];
    default_message_name(msg_fallback, sizeof(msg_fallback), id, is_ext);
    if (!normalize_name(gtk_entry_get_text(GTK_ENTRY(s_tab.message_name_entry)),
                        msg_fallback, msg_name)) {
        set_status("Message name is empty.");
        return;
    }

    char err[256] = {0};
    dbc_signal_t sig;
    if (!build_signal_from_form(&sig, dlc, err, sizeof(err))) {
        set_status("%s", err);
        return;
    }

    dbc_db_t *db = NULL;
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        db = dbc_load_file(path, err, sizeof(err));
        if (!db) {
            set_status("Could not load target DBC: %s", err);
            return;
        }
    } else {
        db = dbc_create_empty(path);
        if (!db) {
            set_status("Out of memory while creating database.");
            return;
        }
    }

    dbc_message_t *msg = dbc_upsert_message(db, id, is_ext, dlc,
                                            msg_name, err, sizeof(err));
    if (!msg) {
        set_status("%s", err);
        dbc_free(db);
        return;
    }

    int replaced = 0;
    if (dbc_upsert_signal(db, msg, &sig, &replaced, err, sizeof(err)) != 0) {
        set_status("%s", err);
        dbc_free(db);
        return;
    }

    if (dbc_save_file(db, path, err, sizeof(err)) != 0) {
        set_status("%s", err);
        dbc_free(db);
        return;
    }

    dbc_free(db);
    gui_signal_load_dbc(path);

    set_status("%s signal %s in %s.",
               replaced ? "Updated" : "Created",
               sig.name,
               path);
    gui_status_message("%s signal %s in %s.",
                       replaced ? "Updated" : "Created",
                       sig.name,
                       path);
}

static GtkWidget *grid_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    return label;
}

static GtkWidget *new_double_spin(double value, int digits)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(-1000000000.0,
                                                     1000000000.0,
                                                     0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_hexpand(spin, TRUE);
    return spin;
}

GtkWidget *gui_create_db_creation_view(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 8);
    gtk_widget_set_margin_bottom(outer, 8);

    GtkWidget *target_frame = gtk_frame_new("Target Database");
    gtk_box_pack_start(GTK_BOX(outer), target_frame, FALSE, FALSE, 0);

    GtkWidget *target_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(target_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(target_grid), 6);
    gtk_widget_set_margin_start(target_grid, 8);
    gtk_widget_set_margin_end(target_grid, 8);
    gtk_widget_set_margin_top(target_grid, 8);
    gtk_widget_set_margin_bottom(target_grid, 8);
    gtk_container_add(GTK_CONTAINER(target_frame), target_grid);

    s_tab.target_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s_tab.target_entry, TRUE);
    gtk_grid_attach(GTK_GRID(target_grid), grid_label("DBC file"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(target_grid), s_tab.target_entry, 1, 0, 1, 1);

    GtkWidget *target_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *new_btn = gtk_button_new_with_label("New...");
    GtkWidget *open_btn = gtk_button_new_with_label("Open...");
    GtkWidget *loaded_btn = gtk_button_new_with_label("Use Loaded");
    g_signal_connect(new_btn, "clicked", G_CALLBACK(on_new_db), NULL);
    g_signal_connect(open_btn, "clicked", G_CALLBACK(on_open_db), NULL);
    g_signal_connect(loaded_btn, "clicked", G_CALLBACK(on_use_loaded_db), NULL);
    gtk_box_pack_start(GTK_BOX(target_buttons), new_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_buttons), open_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_buttons), loaded_btn, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(target_grid), target_buttons, 2, 0, 1, 1);

    GtkWidget *source_frame = gtk_frame_new("Source RX Message");
    gtk_box_pack_start(GTK_BOX(outer), source_frame, FALSE, FALSE, 0);

    GtkWidget *source_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(source_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(source_grid), 6);
    gtk_widget_set_margin_start(source_grid, 8);
    gtk_widget_set_margin_end(source_grid, 8);
    gtk_widget_set_margin_top(source_grid, 8);
    gtk_widget_set_margin_bottom(source_grid, 8);
    gtk_container_add(GTK_CONTAINER(source_frame), source_grid);

    s_tab.msg_store = gtk_list_store_new(MCOL_NUM,
                                         G_TYPE_STRING,
                                         G_TYPE_UINT,
                                         G_TYPE_BOOLEAN,
                                         G_TYPE_UINT,
                                         G_TYPE_STRING);
    s_tab.msg_combo = gtk_combo_box_new_with_model(
        GTK_TREE_MODEL(s_tab.msg_store));
    GtkCellRenderer *rend = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(s_tab.msg_combo), rend, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(s_tab.msg_combo),
                                  rend, "text", MCOL_LABEL);
    gtk_widget_set_hexpand(s_tab.msg_combo, TRUE);
    g_signal_connect(s_tab.msg_combo, "changed",
                     G_CALLBACK(selected_message_changed), NULL);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_btn, "clicked",
                     G_CALLBACK(on_refresh_clicked), NULL);

    s_tab.rx_count_label = gtk_label_new("0 RX messages");
    gtk_label_set_xalign(GTK_LABEL(s_tab.rx_count_label), 0.0f);

    gtk_grid_attach(GTK_GRID(source_grid), grid_label("Message ID"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), s_tab.msg_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), refresh_btn, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), s_tab.rx_count_label, 1, 1, 2, 1);

    s_tab.sample_data_label = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(s_tab.sample_data_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s_tab.sample_data_label),
                            PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(source_grid), grid_label("Latest RX"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), s_tab.sample_data_label, 1, 2, 2, 1);

    GtkWidget *edit_frame = gtk_frame_new("Message and Signal");
    gtk_widget_set_vexpand(edit_frame, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), edit_frame, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 7);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    gtk_container_add(GTK_CONTAINER(edit_frame), grid);

    s_tab.message_name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s_tab.message_name_entry, TRUE);
    s_tab.signal_name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(s_tab.signal_name_entry), "Signal");
    gtk_widget_set_hexpand(s_tab.signal_name_entry, TRUE);

    s_tab.start_spin = gtk_spin_button_new_with_range(0, 511, 1);
    s_tab.length_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.length_spin), 8);

    s_tab.endian_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_tab.endian_combo),
                                   "Intel little-endian");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_tab.endian_combo),
                                   "Motorola big-endian");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_tab.endian_combo), 0);

    s_tab.signed_check = gtk_check_button_new();
    s_tab.factor_spin = new_double_spin(1.0, 6);
    s_tab.offset_spin = new_double_spin(0.0, 6);
    s_tab.min_spin = new_double_spin(0.0, 6);
    s_tab.max_spin = new_double_spin(255.0, 6);
    s_tab.unit_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(s_tab.unit_entry), DBC_UNIT_MAX - 1);

    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Message name"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.message_name_entry, 1, row, 3, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Signal name"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.signal_name_entry, 1, row, 3, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Start bit"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.start_spin, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Length"), 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.length_spin, 3, row, 1, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Byte order"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.endian_combo, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Signed"), 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.signed_check, 3, row, 1, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Factor"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.factor_spin, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Offset"), 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.offset_spin, 3, row, 1, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Min"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.min_spin, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Max"), 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.max_spin, 3, row, 1, 1);
    row++;
    gtk_grid_attach(GTK_GRID(grid), grid_label("Unit"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.unit_entry, 1, row, 3, 1);
    row++;

    s_tab.sample_raw_label = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(s_tab.sample_raw_label), 0.0f);
    s_tab.sample_value_label = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(s_tab.sample_value_label), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Sample raw"), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.sample_raw_label, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("Sample value"), 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_tab.sample_value_label, 3, row, 1, 1);
    row++;

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *save_btn = gtk_button_new_with_label("Create / Update Signal");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_signal), NULL);
    gtk_box_pack_end(GTK_BOX(actions), save_btn, FALSE, FALSE, 0);
    s_tab.status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_tab.status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s_tab.status_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(actions), s_tab.status_label, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), actions, 0, row, 4, 1);

    GtkWidget *watch[] = {
        s_tab.start_spin, s_tab.length_spin, s_tab.endian_combo,
        s_tab.signed_check, s_tab.factor_spin, s_tab.offset_spin
    };
    for (size_t i = 0; i < sizeof(watch) / sizeof(watch[0]); i++) {
        const char *sig = GTK_IS_SPIN_BUTTON(watch[i]) ? "value-changed" :
                          GTK_IS_TOGGLE_BUTTON(watch[i]) ? "toggled" :
                          "changed";
        g_signal_connect(watch[i], sig, G_CALLBACK(signal_form_changed), NULL);
    }

    db_creation_refresh_messages();
    return outer;
}
