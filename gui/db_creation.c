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

/** @brief Height floor for the signal comment box (px). */
#define DBC_COMMENT_MIN_H 44

#define RAW_TEXT_MAX ((CANFD_DATA_MAX * 2u) + 1u)
#define GRAPH_MAX_SAMPLES 2048
#define GRAPH_WINDOW_SEC  20.0

typedef struct {
    double t;
    double value;
} graph_sample_t;

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
    GtkWidget    *comment_view;
    GtkWidget    *sample_data_label;
    GtkWidget    *sample_raw_label;
    GtkWidget    *sample_value_label;
    GtkWidget    *graph_area;
    GtkWidget    *status_label;
    guint         refresh_source;
    gboolean      preserve_form_on_refresh;
    gboolean      refreshing_messages;
    graph_sample_t graph_samples[GRAPH_MAX_SAMPLES];
    int           graph_head;
    int           graph_count;
    gint64        graph_start_us;
    uint32_t      selected_id;
    int           selected_ext;
    gboolean      selected_valid;
} s_tab;

static void db_creation_refresh_messages(void);
static void update_sample_preview(void);
static void graph_clear(void);

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

static void set_comment_text(const char *text)
{
    if (!s_tab.comment_view)
        return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(s_tab.comment_view));
    gtk_text_buffer_set_text(buf, text ? text : "", -1);
}

static void get_comment_text(char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (!s_tab.comment_view)
        return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(s_tab.comment_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    snprintf(out, outsz, "%s", text ? text : "");
    g_free(text);
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

static void read_decode_signal(dbc_signal_t *sig)
{
    memset(sig, 0, sizeof(*sig));
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
}

static int graph_sample_index(int logical)
{
    return (s_tab.graph_head - s_tab.graph_count + logical +
            GRAPH_MAX_SAMPLES) % GRAPH_MAX_SAMPLES;
}

static void graph_clear(void)
{
    s_tab.graph_head = 0;
    s_tab.graph_count = 0;
    s_tab.graph_start_us = 0;
    if (s_tab.graph_area)
        gtk_widget_queue_draw(s_tab.graph_area);
}

static void graph_append(double value)
{
    gint64 now_us = g_get_monotonic_time();
    if (s_tab.graph_start_us == 0)
        s_tab.graph_start_us = now_us;

    int idx = s_tab.graph_head;
    s_tab.graph_samples[idx].t =
        (double)(now_us - s_tab.graph_start_us) / 1000000.0;
    s_tab.graph_samples[idx].value = value;
    s_tab.graph_head = (s_tab.graph_head + 1) % GRAPH_MAX_SAMPLES;
    if (s_tab.graph_count < GRAPH_MAX_SAMPLES)
        s_tab.graph_count++;

    if (s_tab.graph_area)
        gtk_widget_queue_draw(s_tab.graph_area);
}

static void graph_draw_text(cairo_t *cr, const char *text, double x, double y)
{
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

static gboolean on_graph_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;

    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    double W = a.width;
    double H = a.height;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    double left = 58.0;
    double right = 12.0;
    double top = 16.0;
    double bottom = 34.0;
    double x0 = left;
    double x1 = W - right;
    double y0 = top;
    double y1 = H - bottom;
    if (x1 <= x0 + 20.0 || y1 <= y0 + 20.0)
        return FALSE;

    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_set_line_width(cr, 1.0);

    double last_t = 0.0;
    if (s_tab.graph_count > 0) {
        int last_idx = graph_sample_index(s_tab.graph_count - 1);
        last_t = s_tab.graph_samples[last_idx].t;
    }
    double t_start = last_t > GRAPH_WINDOW_SEC ?
                     last_t - GRAPH_WINDOW_SEC : 0.0;
    double t_end = t_start + GRAPH_WINDOW_SEC;
    if (last_t > t_end)
        t_end = last_t;

    double y_min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s_tab.min_spin));
    double y_max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s_tab.max_spin));
    if (y_max <= y_min) {
        y_min = 0.0;
        y_max = 1.0;
        gboolean found = FALSE;
        for (int i = 0; i < s_tab.graph_count; i++) {
            int idx = graph_sample_index(i);
            graph_sample_t *s = &s_tab.graph_samples[idx];
            if (s->t < t_start)
                continue;
            if (!found) {
                y_min = y_max = s->value;
                found = TRUE;
            } else {
                if (s->value < y_min) y_min = s->value;
                if (s->value > y_max) y_max = s->value;
            }
        }
        if (y_max <= y_min) {
            y_min -= 0.5;
            y_max += 0.5;
        }
    }
    double y_span = y_max - y_min;
    if (y_span <= 0.0)
        y_span = 1.0;

    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    for (int i = 0; i <= 5; i++) {
        double x = x0 + (x1 - x0) * i / 5.0;
        cairo_move_to(cr, x, y0);
        cairo_line_to(cr, x, y1);
        cairo_stroke(cr);
    }
    for (int i = 0; i <= 4; i++) {
        double y = y1 - (y1 - y0) * i / 4.0;
        cairo_move_to(cr, x0, y);
        cairo_line_to(cr, x1, y);
        cairo_stroke(cr);
    }

    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);

    for (int i = 0; i <= 5; i++) {
        double t = t_start + (t_end - t_start) * i / 5.0;
        double x = x0 + (x1 - x0) * i / 5.0;
        char label[32];
        snprintf(label, sizeof(label), "%.1fs", t);
        graph_draw_text(cr, label, x - 12.0, H - 12.0);
    }
    for (int i = 0; i <= 4; i++) {
        double v = y_min + y_span * i / 4.0;
        double y = y1 - (y1 - y0) * i / 4.0;
        char label[32];
        snprintf(label, sizeof(label), "%.4g", v);
        graph_draw_text(cr, label, 5.0, y + 3.0);
    }

    if (s_tab.graph_count == 0) {
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        graph_draw_text(cr, "No samples", x0 + 12.0, y0 + 22.0);
        return FALSE;
    }

    cairo_set_source_rgb(cr, 0.10, 0.42, 0.78);
    cairo_set_line_width(cr, 1.8);
    gboolean started = FALSE;
    double last_x = 0.0, last_y = 0.0;
    for (int i = 0; i < s_tab.graph_count; i++) {
        int idx = graph_sample_index(i);
        graph_sample_t *s = &s_tab.graph_samples[idx];
        if (s->t < t_start)
            continue;
        double x = x0 + ((s->t - t_start) / (t_end - t_start)) * (x1 - x0);
        double norm = (s->value - y_min) / y_span;
        if (norm < 0.0) norm = 0.0;
        if (norm > 1.0) norm = 1.0;
        double y = y1 - norm * (y1 - y0);
        if (!started) {
            cairo_move_to(cr, x, y);
            started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
        last_x = x;
        last_y = y;
    }
    if (started)
        cairo_stroke(cr);

    cairo_arc(cr, last_x, last_y, 3.0, 0, 2.0 * G_PI);
    cairo_fill(cr);

    return FALSE;
}

static void selected_message_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    (void)data;

    if (s_tab.refreshing_messages)
        return;

    uint32_t id = 0;
    int is_ext = 0;
    uint8_t dlc = 0;
    if (!get_selected_message(&id, &is_ext, &dlc, NULL, NULL, 0)) {
        gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), "-");
        if (s_tab.selected_valid)
            graph_clear();
        s_tab.selected_valid = FALSE;
        update_sample_preview();
        return;
    }

    gboolean selection_changed =
        !s_tab.selected_valid ||
        s_tab.selected_id != id ||
        s_tab.selected_ext != is_ext;
    s_tab.selected_id = id;
    s_tab.selected_ext = is_ext;
    s_tab.selected_valid = TRUE;

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

    if (selection_changed)
        graph_clear();
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
    read_decode_signal(&sig);

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
    graph_clear();
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

void gui_db_creation_handle_message(const can_msg_t *msg)
{
    if (!s_tab.graph_area || !msg || msg->is_error ||
        msg->direction != CAN_DIR_RX)
        return;

    uint32_t id = 0;
    int is_ext = 0;
    if (!get_selected_message(&id, &is_ext, NULL, NULL, NULL, 0))
        return;
    if (id != msg->id || is_ext != (msg->is_extended ? 1 : 0))
        return;

    dbc_signal_t sig;
    read_decode_signal(&sig);
    if (!signal_fits_payload(sig.start_bit, sig.length,
                             sig.little_endian, msg->dlc))
        return;

    int64_t sraw = 0;
    uint64_t raw = dbc_extract_raw(msg->data, msg->dlc, &sig);
    double phys = dbc_decode_physical(&sig, raw, &sraw);

    char id_buf[GUI_TRACE_ID_TEXT_MAX];
    char data_buf[GUI_TRACE_DATA_TEXT_MAX];
    char latest[GUI_TRACE_DATA_TEXT_MAX + 96];
    gui_format_id(id_buf, sizeof(id_buf), msg->id, msg->is_extended);
    gui_format_data(data_buf, sizeof(data_buf), msg->data, msg->dlc);
    snprintf(latest, sizeof(latest), "%s  %s  DLC %u  Data %s",
             id_buf,
             msg->is_extended ? "Ext" : "Std",
             msg->dlc,
             data_buf[0] ? data_buf : "-");
    gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), latest);

    char raw_buf[32];
    char val_buf[64];
    snprintf(raw_buf, sizeof(raw_buf), "%lld", (long long)sraw);
    fmt_double(val_buf, sizeof(val_buf), phys);
    gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), raw_buf);
    gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), val_buf);

    graph_append(phys);
}

void gui_db_creation_prefill_signal(uint32_t message_id,
                                    int is_extended,
                                    uint8_t dlc,
                                    const char *signal_name,
                                    uint16_t start_bit,
                                    uint8_t bit_length,
                                    int byte_order,
                                    int is_signed,
                                    double factor,
                                    double offset,
                                    double minimum,
                                    double maximum,
                                    const char *unit,
                                    const char *comment)
{
    if (!s_tab.msg_store || !s_tab.msg_combo) {
        gui_status_message("Open DB Creation before promoting a Bit Analysis candidate.");
        return;
    }

    if (dlc > CANFD_DATA_MAX)
        dlc = CANFD_DATA_MAX;

    db_creation_refresh_messages();

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(s_tab.msg_store), &iter);
    int active = -1;
    int idx = 0;
    while (valid) {
        guint row_id = 0;
        gboolean row_ext = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(s_tab.msg_store), &iter,
                           MCOL_ID, &row_id,
                           MCOL_EXT, &row_ext,
                           -1);
        if (row_id == message_id &&
            row_ext == (gboolean)(is_extended ? 1 : 0)) {
            active = idx;
            break;
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_tab.msg_store), &iter);
        idx++;
    }

    if (active < 0) {
        char id_text[GUI_TRACE_ID_TEXT_MAX];
        char label[GUI_TRACE_DATA_TEXT_MAX + 96];
        gui_format_id(id_text, sizeof(id_text), message_id, is_extended);
        snprintf(label, sizeof(label),
                 "%s  %s  DLC %u  Data -  Count 0",
                 id_text, is_extended ? "Ext" : "Std", dlc);

        GtkTreeIter row;
        gtk_list_store_append(s_tab.msg_store, &row);
        gtk_list_store_set(s_tab.msg_store, &row,
                           MCOL_LABEL, label,
                           MCOL_ID, (guint)message_id,
                           MCOL_EXT, (gboolean)(is_extended ? 1 : 0),
                           MCOL_DLC, (guint)dlc,
                           MCOL_RAW, "",
                           -1);
        active = idx;
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(s_tab.msg_combo), active);

    char fallback[DBC_NAME_MAX];
    default_message_name(fallback, sizeof(fallback), message_id, is_extended);
    gtk_entry_set_text(GTK_ENTRY(s_tab.message_name_entry), fallback);
    gtk_entry_set_text(GTK_ENTRY(s_tab.signal_name_entry),
                       signal_name && *signal_name ? signal_name : "Signal");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.start_spin), start_bit);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.length_spin),
                              bit_length ? bit_length : 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_tab.endian_combo),
                             byte_order ? 0 : 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_tab.signed_check),
                                 is_signed ? TRUE : FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.factor_spin), factor);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.offset_spin), offset);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.min_spin), minimum);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_tab.max_spin), maximum);
    gtk_entry_set_text(GTK_ENTRY(s_tab.unit_entry), unit ? unit : "");
    set_comment_text(comment);

    const char *loaded = gui_signal_get_dbc_path();
    if (loaded && *loaded &&
        (!gtk_entry_get_text(GTK_ENTRY(s_tab.target_entry)) ||
         !*gtk_entry_get_text(GTK_ENTRY(s_tab.target_entry)))) {
        gtk_entry_set_text(GTK_ENTRY(s_tab.target_entry), loaded);
    }

    graph_clear();
    update_sample_preview();
    if (g_gui.main_notebook && g_gui.db_creation_page) {
        gint page = gtk_notebook_page_num(GTK_NOTEBOOK(g_gui.main_notebook),
                                          g_gui.db_creation_page);
        if (page >= 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_gui.main_notebook),
                                          page);
    }
    set_status("Prefilled candidate %s from Bit Analysis. Review, choose a DBC file, then create/update.",
               signal_name && *signal_name ? signal_name : "Signal");
    gui_status_message("Bit Analysis candidate copied to DB Creation for review.");
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

    s_tab.refreshing_messages = TRUE;
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
        s_tab.refreshing_messages = FALSE;
        selected_message_changed(GTK_COMBO_BOX(s_tab.msg_combo), NULL);
        s_tab.preserve_form_on_refresh = FALSE;
    } else {
        s_tab.refreshing_messages = FALSE;
        gtk_label_set_text(GTK_LABEL(s_tab.sample_data_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_raw_label), "-");
        gtk_label_set_text(GTK_LABEL(s_tab.sample_value_label), "-");
        if (s_tab.selected_valid)
            graph_clear();
        s_tab.selected_valid = FALSE;
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
    get_comment_text(sig->comment, sizeof(sig->comment));

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
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 4);
    gtk_widget_set_margin_bottom(outer, 4);

    GtkWidget *target_frame = gtk_frame_new("Target Database");
    gtk_box_pack_start(GTK_BOX(outer), target_frame, FALSE, FALSE, 0);

    GtkWidget *target_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(target_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(target_grid), 6);
    gtk_widget_set_margin_start(target_grid, 8);
    gtk_widget_set_margin_end(target_grid, 8);
    gtk_widget_set_margin_top(target_grid, 5);
    gtk_widget_set_margin_bottom(target_grid, 5);
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
    gtk_grid_set_row_spacing(GTK_GRID(source_grid), 4);
    gtk_widget_set_margin_start(source_grid, 8);
    gtk_widget_set_margin_end(source_grid, 8);
    gtk_widget_set_margin_top(source_grid, 5);
    gtk_widget_set_margin_bottom(source_grid, 5);
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

    s_tab.sample_data_label = gtk_label_new("-");
    gtk_label_set_xalign(GTK_LABEL(s_tab.sample_data_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s_tab.sample_data_label),
                            PANGO_ELLIPSIZE_END);

    /* Latest RX and the RX count share a row: the count is short and the
     * spare row costs height this tab cannot afford at 1366x768. */
    gtk_grid_attach(GTK_GRID(source_grid), grid_label("Latest RX"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), s_tab.sample_data_label, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(source_grid), s_tab.rx_count_label, 2, 1, 1, 1);

    GtkWidget *edit_frame = gtk_frame_new("Message and Signal");
    gtk_widget_set_vexpand(edit_frame, TRUE);
    gtk_box_pack_start(GTK_BOX(outer), edit_frame, TRUE, TRUE, 0);

    GtkWidget *edit_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(edit_body, 8);
    gtk_widget_set_margin_end(edit_body, 8);
    gtk_widget_set_margin_top(edit_body, 5);
    gtk_widget_set_margin_bottom(edit_body, 5);
    gtk_container_add(GTK_CONTAINER(edit_frame), edit_body);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    /* Ten form rows: every pixel of row spacing costs ten down the page. */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_widget_set_vexpand(grid, TRUE);
    gtk_box_pack_start(GTK_BOX(edit_body), grid, TRUE, TRUE, 0);

    /* Right column: plot on top, comment beneath it. The comment lives here
     * rather than in the form grid because the grid is the tab's tallest
     * element at 1366x768, while this column has room to spare. */
    GtkWidget *right_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(right_col, TRUE);
    gtk_widget_set_vexpand(right_col, TRUE);
    gtk_box_pack_start(GTK_BOX(edit_body), right_col, TRUE, TRUE, 0);

    GtkWidget *graph_frame = gtk_frame_new("Sample Value vs Time");
    gtk_widget_set_hexpand(graph_frame, TRUE);
    gtk_widget_set_vexpand(graph_frame, TRUE);
    gtk_box_pack_start(GTK_BOX(right_col), graph_frame, TRUE, TRUE, 0);

    s_tab.graph_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(s_tab.graph_area, TRUE);
    gtk_widget_set_vexpand(s_tab.graph_area, TRUE);
    g_signal_connect(s_tab.graph_area, "draw", G_CALLBACK(on_graph_draw), NULL);
    gtk_container_add(GTK_CONTAINER(graph_frame), s_tab.graph_area);

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
    s_tab.comment_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(s_tab.comment_view),
                                GTK_WRAP_WORD_CHAR);

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
    GtkWidget *comment_frame = gtk_frame_new("Comment");
    GtkWidget *comment_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(comment_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    /* The scrolled window, not the text view, owns the height floor: a
     * size request on the child is absorbed by the viewport and the field
     * collapses to nothing. */
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(comment_scroll), DBC_COMMENT_MIN_H);
    /* A bare scrolled window draws no border under several themes, leaving a
     * white text view on a white frame that reads as a missing field. Border
     * it explicitly so the input area is visible everywhere. */
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(comment_scroll),
                                        GTK_SHADOW_IN);
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(
            css, "scrolledwindow { border: 1px solid alpha(currentColor,0.3); }",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(comment_scroll),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    gtk_container_add(GTK_CONTAINER(comment_scroll), s_tab.comment_view);
    gtk_container_add(GTK_CONTAINER(comment_frame), comment_scroll);
    gtk_box_pack_end(GTK_BOX(right_col), comment_frame, FALSE, FALSE, 0);

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
