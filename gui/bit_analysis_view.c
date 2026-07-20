/**
 * @file bit_analysis_view.c
 * @brief GTK Bit Analysis tab for reverse-engineering classic CAN payloads.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/bit_analysis.h"

#define BA_SURVEY_MAX_IDS 256
#define BA_UNIQUE_TRACK   64
#define BA_UI_REFRESH_MS  100
#define BA_UI_ANALYZE_MAX_SAMPLES 5000u

/**
 * @name Bit matrix geometry
 *
 * @details
 * The matrix is an 8x8 grid of bit cells with a row-label gutter on the left
 * and a column-label strip on top. #BA_MATRIX_CELL_MIN is the smallest cell
 * that still fits a legible glyph; below it @ref on_matrix_draw gives up and
 * renders nothing. The widget therefore requests #BA_MATRIX_MIN_W by
 * #BA_MATRIX_MIN_H so it can never be allocated into that dead zone.
 * @{
 */
#define BA_MATRIX_LEFT      44  /**< Row-label gutter width (px).             */
#define BA_MATRIX_TOP       26  /**< Column-label strip height (px).          */
#define BA_MATRIX_PAD_R     10  /**< Right padding (px).                      */
#define BA_MATRIX_PAD_B     12  /**< Bottom padding (px).                     */
#define BA_MATRIX_CELL_MIN  12  /**< Smallest legible cell edge (px).         */
#define BA_MATRIX_MIN_W \
    (BA_MATRIX_LEFT + BA_MATRIX_PAD_R + 8 * BA_MATRIX_CELL_MIN)
#define BA_MATRIX_MIN_H \
    (BA_MATRIX_TOP + BA_MATRIX_PAD_B + 8 * BA_MATRIX_CELL_MIN)
/** @} */

/**
 * @name Field timeline geometry
 *
 * @details
 * The timeline shares the right-hand column with the bit matrix. Both expand,
 * so without an explicit floor the matrix's larger minimum consumes the column
 * and collapses the timeline to nothing. These values keep the plot area big
 * enough to stay readable at 1366x768.
 * @{
 */
#define BA_TIMELINE_MIN_W  180  /**< Minimum timeline width (px).             */
#define BA_TIMELINE_MIN_H  90   /**< Minimum timeline height (px).            */
/** @} */

typedef struct {
    uint32_t id;
    uint8_t ext;
    uint8_t dir;
    uint8_t is_fd;
    uint8_t dlc;
    uint8_t data[CANFD_DATA_MAX];
    uint8_t prev_data[CANFD_DATA_MAX];
    uint8_t have_prev;
    uint64_t count;
    uint64_t change_count;
    uint64_t changed_mask;
    uint64_t unique_hashes[BA_UNIQUE_TRACK];
    uint32_t unique_count;
    uint8_t unique_overflow;
    int64_t first_ts_ns;
    int64_t last_ts_ns;
    double mean_period_ms;
    double m2_period_ms;
    double min_period_ms;
    double max_period_ms;
} ba_survey_row_t;

enum {
    SURV_COL_LABEL = 0,
    SURV_COL_ID,
    SURV_COL_EXT,
    SURV_COL_DIR,
    SURV_COL_FD,
    SURV_COL_DLC,
    SURV_COL_COUNT,
    SURV_COL_MEAN,
    SURV_COL_JITTER,
    SURV_COL_CHANGED,
    SURV_COL_CHANGE_RATE,
    SURV_COL_UNIQUE,
    SURV_COL_DATA,
    SURV_COL_NUM
};

enum {
    SEG_COL_NUM = 0,
    SEG_COL_TYPE,
    SEG_COL_LABEL,
    SEG_COL_VALUE,
    SEG_COL_FRAMES,
    SEG_COL_NUM_COLS
};

enum {
    CAND_COL_RANK = 0,
    CAND_COL_START,
    CAND_COL_DBC_START,
    CAND_COL_LEN,
    CAND_COL_ORDER,
    CAND_COL_SIGNED,
    CAND_COL_PEARSON,
    CAND_COL_SPEARMAN,
    CAND_COL_R2,
    CAND_COL_FACTOR,
    CAND_COL_OFFSET,
    CAND_COL_MAE,
    CAND_COL_VAL_N,
    CAND_COL_VAL_MAE,
    CAND_COL_LAG,
    CAND_COL_TRANSITIONS,
    CAND_COL_COUNTER,
    CAND_COL_TIMESTAMP,
    CAND_COL_TS_RES,
    CAND_COL_CHECKSUM,
    CAND_COL_MUX,
    CAND_COL_MUX_VALUES,
    CAND_COL_MUX_FRAMES,
    CAND_COL_MUX_BITS,
    CAND_COL_MUX_RANGES,
    CAND_COL_SCORE,
    CAND_COL_CONF,
    CAND_COL_INDEX,
    CAND_COL_NUM
};

typedef enum {
    BA_VIEW_LIVE = 0,
    BA_VIEW_BASELINE,
    BA_VIEW_XOR,
    BA_VIEW_ACTIVITY,
    BA_VIEW_ENTROPY,
    BA_VIEW_CORRELATION
} ba_view_mode_t;

typedef struct {
    ba_session_t session;
    ba_survey_row_t survey[BA_SURVEY_MAX_IDS];
    int survey_count;

    GtkWidget *target_combo;
    GtkListStore *target_store;
    GtkWidget *dir_combo;
    GtkWidget *dlc_spin;
    GtkWidget *accept_varying_check;
    GtkWidget *fd_full_check;
    GtkWidget *mode_combo;
    GtkWidget *state_label;
    GtkWidget *summary_label;
    GtkWidget *matrix_area;
    GtkWidget *timeline_area;
    GtkWidget *status_label;

    GtkWidget *input_name_entry;
    GtkWidget *input_type_combo;
    GtkWidget *input_value_spin;
    GtkWidget *input_unit_entry;
    GtkWidget *segment_type_combo;
    GtkWidget *segment_label_entry;
    GtkListStore *segment_store;
    GtkWidget *segment_view;

    GtkWidget *field_start_spin;
    GtkWidget *field_len_spin;
    GtkWidget *field_order_combo;
    GtkWidget *field_signed_check;
    GtkWidget *field_raw_label;
    GtkWidget *field_phys_label;
    GtkWidget *bit_stats_label;
    GtkListStore *candidate_store;
    GtkWidget *candidate_view;
    GtkWidget *inspector_label;

    guint refresh_source;
    gboolean dirty_survey;
    gboolean dirty_view;
    gboolean suppress_target_changed;
    int selected_bit;
    int selected_candidate;
    guint analysis_generation;
    gboolean analysis_running;
} bit_ui_t;

static bit_ui_t s_ba;

static void refresh_target_store(void);
static void refresh_segment_store(void);
static void refresh_candidate_store(void);
static void refresh_summary(void);
static void refresh_field_preview(void);
static void refresh_bit_stats_label(void);
static void refresh_inspector(const ba_candidate_t *c);
static void sync_controls_from_session(void);
static void add_analysis_filter(GtkFileChooser *chooser);
static gboolean save_session_with_dialog(void);
static void on_target_changed(GtkComboBox *combo, gpointer data);

static void invalidate_analysis_results(void)
{
    s_ba.analysis_generation++;
}

static void set_status(const char *fmt, ...)
{
    if (!s_ba.status_label)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(s_ba.status_label), buf);
}

static uint64_t payload_hash(const uint8_t *data, uint8_t dlc)
{
    uint64_t h = 1469598103934665603ull;
    h ^= dlc;
    h *= 1099511628211ull;
    for (uint8_t i = 0; i < dlc; i++) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int bit_count64(uint64_t v)
{
    int n = 0;
    while (v) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

static ba_survey_row_t *find_survey_row(uint32_t id, uint8_t ext, uint8_t dir)
{
    for (int i = 0; i < s_ba.survey_count; i++) {
        ba_survey_row_t *r = &s_ba.survey[i];
        if (r->id == id && r->ext == ext && r->dir == dir)
            return r;
    }
    if (s_ba.survey_count >= BA_SURVEY_MAX_IDS)
        return NULL;

    ba_survey_row_t *r = &s_ba.survey[s_ba.survey_count++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->ext = ext;
    r->dir = dir;
    r->min_period_ms = 0.0;
    r->max_period_ms = 0.0;
    return r;
}

static void update_survey(const can_msg_t *msg)
{
    if (!msg || msg->is_error || msg->is_remote)
        return;

    ba_survey_row_t *r = find_survey_row(msg->id,
                                         msg->is_extended ? 1u : 0u,
                                         msg->direction);
    if (!r)
        return;

    int64_t ts = ba_timespec_to_ns(&msg->timestamp);
    if (r->count == 0)
        r->first_ts_ns = ts;
    if (r->last_ts_ns > 0 && ts > r->last_ts_ns) {
        double dt_ms = (double)(ts - r->last_ts_ns) / 1e6;
        uint64_t k = r->count;
        double delta = dt_ms - r->mean_period_ms;
        r->mean_period_ms += delta / (double)k;
        r->m2_period_ms += delta * (dt_ms - r->mean_period_ms);
        if (r->min_period_ms == 0.0 || dt_ms < r->min_period_ms)
            r->min_period_ms = dt_ms;
        if (dt_ms > r->max_period_ms)
            r->max_period_ms = dt_ms;
    }
    r->last_ts_ns = ts;

    uint8_t dlc = msg->dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : msg->dlc;
    if (r->have_prev) {
        if (dlc != r->dlc || memcmp(r->prev_data, msg->data, dlc) != 0)
            r->change_count++;
        uint8_t max = dlc > r->dlc ? dlc : r->dlc;
        if (max > BA_CLASSIC_MAX_BYTES)
            max = BA_CLASSIC_MAX_BYTES;
        for (uint8_t b = 0; b < max; b++) {
            uint8_t diff = r->prev_data[b] ^ msg->data[b];
            for (uint8_t bit = 0; bit < 8; bit++)
                if (diff & (uint8_t)(1u << bit))
                    r->changed_mask |= (uint64_t)1 << (b * 8u + bit);
        }
    }

    uint64_t h = payload_hash(msg->data, dlc);
    int known = 0;
    for (uint32_t i = 0; i < r->unique_count; i++) {
        if (r->unique_hashes[i] == h) {
            known = 1;
            break;
        }
    }
    if (!known) {
        if (r->unique_count < BA_UNIQUE_TRACK)
            r->unique_hashes[r->unique_count++] = h;
        else
            r->unique_overflow = 1;
    }

    r->count++;
    r->is_fd = msg->is_fd ? 1u : 0u;
    r->dlc = dlc;
    memcpy(r->data, msg->data, dlc);
    memcpy(r->prev_data, msg->data, dlc);
    r->have_prev = 1;
    s_ba.dirty_survey = TRUE;
}

static gboolean refresh_timer_cb(gpointer data)
{
    (void)data;
    if (s_ba.dirty_survey) {
        refresh_target_store();
        s_ba.dirty_survey = FALSE;
    }
    if (s_ba.dirty_view) {
        refresh_summary();
        refresh_segment_store();
        refresh_field_preview();
        refresh_bit_stats_label();
        if (s_ba.matrix_area)
            gtk_widget_queue_draw(s_ba.matrix_area);
        if (s_ba.timeline_area)
            gtk_widget_queue_draw(s_ba.timeline_area);
        s_ba.dirty_view = FALSE;
    }
    return G_SOURCE_CONTINUE;
}

static void ensure_refresh_timer(void)
{
    if (!s_ba.refresh_source)
        s_ba.refresh_source = g_timeout_add(BA_UI_REFRESH_MS,
                                            refresh_timer_cb, NULL);
}

static void format_target_label(char *buf, size_t sz,
                                uint32_t id, uint8_t ext, uint8_t dir,
                                uint8_t is_fd, uint8_t dlc, uint64_t count)
{
    char idbuf[GUI_TRACE_ID_TEXT_MAX];
    gui_format_id(idbuf, sizeof(idbuf), id, ext);
    snprintf(buf, sz, "%s  %s  %s  DLC %u  Count %llu",
             idbuf,
             ext ? "Ext" : "Std",
             is_fd ? "FD" : (dir == CAN_DIR_TX ? "Tx" : "Rx"),
             dlc,
             (unsigned long long)count);
}

static void refresh_target_store(void)
{
    if (!s_ba.target_store)
        return;

    uint32_t old_id = 0;
    gboolean old_ext = FALSE;
    guint old_dir = CAN_DIR_RX;
    gboolean had_selection = FALSE;
    GtkTreeIter old_iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(s_ba.target_combo),
                                      &old_iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(s_ba.target_store), &old_iter,
                           SURV_COL_ID, &old_id,
                           SURV_COL_EXT, &old_ext,
                           SURV_COL_DIR, &old_dir,
                           -1);
        had_selection = TRUE;
    }

    s_ba.suppress_target_changed = TRUE;
    gtk_list_store_clear(s_ba.target_store);
    int active = -1;
    for (int i = 0; i < s_ba.survey_count; i++) {
        ba_survey_row_t *r = &s_ba.survey[i];
        char label[128];
        char data[GUI_TRACE_DATA_TEXT_MAX];
        char mean[32], jitter[32], changed[32], change_rate[32], unique[32];
        format_target_label(label, sizeof(label), r->id, r->ext, r->dir,
                            r->is_fd, r->dlc, r->count);
        gui_format_data(data, sizeof(data), r->data, r->dlc);
        snprintf(mean, sizeof(mean), "%.3g ms", r->mean_period_ms);
        double jitter_ms = r->count > 2 ?
            sqrt(r->m2_period_ms / (double)(r->count - 2u)) : 0.0;
        snprintf(jitter, sizeof(jitter), "%.3g ms", jitter_ms);
        snprintf(changed, sizeof(changed), "%d", bit_count64(r->changed_mask));
        snprintf(change_rate, sizeof(change_rate), "%.1f%%",
                 r->count > 1 ? 100.0 * (double)r->change_count /
                 (double)(r->count - 1u) : 0.0);
        snprintf(unique, sizeof(unique), "%s%u",
                 r->unique_overflow ? ">=" : "", r->unique_count);

        GtkTreeIter iter;
        gtk_list_store_append(s_ba.target_store, &iter);
        gtk_list_store_set(s_ba.target_store, &iter,
                           SURV_COL_LABEL, label,
                           SURV_COL_ID, r->id,
                           SURV_COL_EXT, (gboolean)r->ext,
                           SURV_COL_DIR, (guint)r->dir,
                           SURV_COL_FD, (gboolean)r->is_fd,
                           SURV_COL_DLC, (guint)r->dlc,
                           SURV_COL_COUNT, (guint64)r->count,
                           SURV_COL_MEAN, mean,
                           SURV_COL_JITTER, jitter,
                           SURV_COL_CHANGED, changed,
                           SURV_COL_CHANGE_RATE, change_rate,
                           SURV_COL_UNIQUE, unique,
                           SURV_COL_DATA, data,
                           -1);

        if (had_selection && old_id == r->id &&
            old_ext == (gboolean)r->ext && old_dir == (guint)r->dir)
            active = i;
    }
    if (s_ba.survey_count > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.target_combo),
                                 active >= 0 ? active : 0);
    s_ba.suppress_target_changed = FALSE;
    if (!had_selection && s_ba.survey_count > 0)
        on_target_changed(GTK_COMBO_BOX(s_ba.target_combo), NULL);
}

static gboolean get_selected_target(uint32_t *id, int *ext,
                                    uint8_t *dlc, guint *dir)
{
    GtkTreeIter iter;
    if (!s_ba.target_combo ||
        !gtk_combo_box_get_active_iter(GTK_COMBO_BOX(s_ba.target_combo),
                                       &iter))
        return FALSE;

    guint row_id = 0, row_dlc = 0, row_dir = CAN_DIR_RX;
    gboolean row_ext = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(s_ba.target_store), &iter,
                       SURV_COL_ID, &row_id,
                       SURV_COL_EXT, &row_ext,
                       SURV_COL_DLC, &row_dlc,
                       SURV_COL_DIR, &row_dir,
                       -1);
    if (id) *id = row_id;
    if (ext) *ext = row_ext ? 1 : 0;
    if (dlc) *dlc = (uint8_t)(row_dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : row_dlc);
    if (dir) *dir = row_dir;
    return TRUE;
}

static ba_input_type_t active_input_type(void)
{
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(s_ba.input_type_combo));
    if (idx < 0 || idx > BA_INPUT_EVENT_ONLY)
        idx = BA_INPUT_CONTINUOUS;
    return (ba_input_type_t)idx;
}

static ba_segment_type_t active_segment_type(void)
{
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(s_ba.segment_type_combo));
    if (idx < 0 || idx > BA_SEGMENT_EVENT)
        idx = BA_SEGMENT_STEP;
    return (ba_segment_type_t)idx;
}

static ba_byte_order_t active_field_order(void)
{
    return gtk_combo_box_get_active(GTK_COMBO_BOX(s_ba.field_order_combo)) == 1 ?
        BA_BYTE_ORDER_MOTOROLA : BA_BYTE_ORDER_INTEL;
}

static void sync_input_to_session(void)
{
    const char *name = gtk_entry_get_text(GTK_ENTRY(s_ba.input_name_entry));
    const char *unit = gtk_entry_get_text(GTK_ENTRY(s_ba.input_unit_entry));
    double value = gtk_spin_button_get_value(
        GTK_SPIN_BUTTON(s_ba.input_value_spin));
    ba_session_set_input(&s_ba.session, active_input_type(), name, value,
                         unit, active_input_type() != BA_INPUT_EVENT_ONLY);
}

static gboolean session_has_work(void)
{
    return s_ba.session.sample_count > 0 ||
           s_ba.session.segment_count > 0 ||
           s_ba.session.baseline_valid ||
           s_ba.session.candidate_count > 0 ||
           s_ba.session.state == BA_SESSION_CAPTURING ||
           s_ba.session.state == BA_SESSION_ANALYZING;
}

static gboolean confirm_target_switch(uint32_t id, int ext, uint8_t dlc)
{
    if (!session_has_work())
        return TRUE;
    if (s_ba.session.can_id == id &&
        s_ba.session.is_extended == (ext ? 1 : 0) &&
        s_ba.session.expected_dlc == dlc)
        return TRUE;

    if (s_ba.session.state == BA_SESSION_CAPTURING ||
        s_ba.session.state == BA_SESSION_ANALYZING)
        s_ba.session.state = BA_SESSION_PAUSED;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_gui.window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Save the current Bit Analysis session before switching targets?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "Switching target will reset baseline, samples, markers, and candidate results for the current session.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Discard", GTK_RESPONSE_REJECT,
                           "_Save", GTK_RESPONSE_ACCEPT,
                           NULL);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_CANCEL || response == GTK_RESPONSE_DELETE_EVENT) {
        sync_controls_from_session();
        set_status("Target switch cancelled; current Bit Analysis session kept.");
        return FALSE;
    }
    if (response == GTK_RESPONSE_ACCEPT) {
        if (!save_session_with_dialog()) {
            sync_controls_from_session();
            set_status("Target switch cancelled; session was not saved.");
            return FALSE;
        }
    }
    return TRUE;
}

static void on_target_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    (void)data;
    if (s_ba.suppress_target_changed)
        return;

    uint32_t id = 0;
    int ext = 0;
    uint8_t dlc = BA_CLASSIC_MAX_BYTES;
    guint dir = CAN_DIR_RX;
    if (!get_selected_target(&id, &ext, &dlc, &dir))
        return;

    if (!confirm_target_switch(id, ext, dlc))
        return;

    invalidate_analysis_results();
    ba_session_configure_target(&s_ba.session, id, ext, dlc);
    s_ba.session.include_rx = dir == CAN_DIR_RX;
    s_ba.session.include_tx = dir == CAN_DIR_TX;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.dlc_spin), dlc);
    s_ba.selected_candidate = -1;
    s_ba.selected_bit = -1;
    refresh_summary();
    refresh_candidate_store();
    refresh_segment_store();
    refresh_field_preview();
    refresh_bit_stats_label();
    refresh_inspector(NULL);
    if (s_ba.matrix_area)
        gtk_widget_queue_draw(s_ba.matrix_area);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
    set_status("Selected target 0x%X %s DLC %u.",
               id, ext ? "extended" : "standard", dlc);
}

static void apply_controls_to_session(void)
{
    uint32_t id = s_ba.session.can_id;
    int ext = s_ba.session.is_extended;
    uint8_t dlc = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.dlc_spin));
    guint dir_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(s_ba.dir_combo));
    ba_session_configure_target(&s_ba.session, id, ext, dlc);
    s_ba.session.include_rx = dir_mode == 0 || dir_mode == 2;
    s_ba.session.include_tx = dir_mode == 1 || dir_mode == 2;
    s_ba.session.accept_varying_dlc = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_ba.accept_varying_check)) ? 1u : 0u;
    s_ba.session.analyze_fd_payload = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_ba.fd_full_check)) ? 1u : 0u;
    sync_input_to_session();
}

static void sync_controls_from_session(void)
{
    if (s_ba.dlc_spin)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.dlc_spin),
                                  s_ba.session.expected_dlc);
    if (s_ba.dir_combo) {
        int dir = 2;
        if (s_ba.session.include_rx && !s_ba.session.include_tx)
            dir = 0;
        else if (!s_ba.session.include_rx && s_ba.session.include_tx)
            dir = 1;
        gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.dir_combo), dir);
    }
    if (s_ba.accept_varying_check)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(s_ba.accept_varying_check),
            s_ba.session.accept_varying_dlc);
    if (s_ba.fd_full_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_ba.fd_full_check),
                                     s_ba.session.analyze_fd_payload);
    if (s_ba.input_name_entry)
        gtk_entry_set_text(GTK_ENTRY(s_ba.input_name_entry),
                           s_ba.session.input_name);
    if (s_ba.input_type_combo)
        gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.input_type_combo),
                                 s_ba.session.input_type);
    if (s_ba.input_value_spin)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.input_value_spin),
                                  s_ba.session.current_input_value);
    if (s_ba.input_unit_entry)
        gtk_entry_set_text(GTK_ENTRY(s_ba.input_unit_entry),
                           s_ba.session.input_unit);

    if (s_ba.target_store && s_ba.target_combo) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(
            GTK_TREE_MODEL(s_ba.target_store), &iter);
        int idx = 0;
        s_ba.suppress_target_changed = TRUE;
        while (valid) {
            guint id = 0, dlc = 0;
            gboolean ext = FALSE;
            gtk_tree_model_get(GTK_TREE_MODEL(s_ba.target_store), &iter,
                               SURV_COL_ID, &id,
                               SURV_COL_EXT, &ext,
                               SURV_COL_DLC, &dlc,
                               -1);
            if (id == s_ba.session.can_id &&
                ext == (gboolean)s_ba.session.is_extended &&
                dlc == s_ba.session.expected_dlc) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.target_combo),
                                         idx);
                break;
            }
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(s_ba.target_store),
                                             &iter);
            idx++;
        }
        s_ba.suppress_target_changed = FALSE;
    }
}

static void on_start_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    apply_controls_to_session();
    s_ba.session.state = BA_SESSION_CAPTURING;
    set_status("Capture started. Add baseline and experiment markers as you change the physical input.");
    s_ba.dirty_view = TRUE;
}

static void on_pause_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    if (s_ba.session.state == BA_SESSION_CAPTURING) {
        s_ba.session.state = BA_SESSION_PAUSED;
        set_status("Capture paused.");
    } else {
        s_ba.session.state = BA_SESSION_CAPTURING;
        set_status("Capture resumed.");
    }
    s_ba.dirty_view = TRUE;
}

static void on_reset_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    uint32_t id = s_ba.session.can_id;
    int ext = s_ba.session.is_extended;
    uint8_t dlc = s_ba.session.expected_dlc;
    invalidate_analysis_results();
    ba_session_configure_target(&s_ba.session, id, ext, dlc);
    s_ba.selected_candidate = -1;
    s_ba.selected_bit = -1;
    refresh_candidate_store();
    refresh_segment_store();
    refresh_summary();
    refresh_bit_stats_label();
    if (s_ba.matrix_area)
        gtk_widget_queue_draw(s_ba.matrix_area);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
    set_status("Bit Analysis session reset.");
}

static void on_baseline_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    ba_session_capture_baseline(&s_ba.session);
    if (s_ba.session.baseline_valid)
        set_status("Captured statistical baseline from %zu target frame(s).",
                   s_ba.session.baseline_sample_count ?
                   s_ba.session.baseline_sample_count :
                   s_ba.session.sample_count);
    else
        set_status("No target frames available for baseline.");
    s_ba.dirty_view = TRUE;
}

static void on_add_marker_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    sync_input_to_session();
    const char *label = gtk_entry_get_text(GTK_ENTRY(s_ba.segment_label_entry));
    if (!label || !*label)
        label = ba_segment_type_name(active_segment_type());
    ba_session_add_segment(&s_ba.session, active_segment_type(), label,
                           s_ba.session.current_input_value,
                           s_ba.session.current_input_valid,
                           g_get_real_time() * 1000);
    set_status("Started %s marker at %.6g %s.",
               ba_segment_type_name(active_segment_type()),
               s_ba.session.current_input_value,
               s_ba.session.input_unit);
    refresh_segment_store();
}

static void on_end_marker_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    ba_session_end_segment(&s_ba.session, g_get_real_time() * 1000);
    set_status("Ended current experiment segment.");
    refresh_segment_store();
}

typedef struct {
    ba_session_t work_session;
    guint generation;
} analysis_work_t;

static gboolean analysis_done_idle(gpointer data)
{
    analysis_work_t *work = data;
    if (work->generation == s_ba.analysis_generation) {
        memcpy(s_ba.session.bit_stats, work->work_session.bit_stats,
               sizeof(s_ba.session.bit_stats));
        memcpy(s_ba.session.candidates, work->work_session.candidates,
               sizeof(s_ba.session.candidates));
        s_ba.session.candidate_count = work->work_session.candidate_count;
        s_ba.session.state = BA_SESSION_REVIEW;
        s_ba.analysis_running = FALSE;
        refresh_candidate_store();
        refresh_summary();
        refresh_field_preview();
        refresh_bit_stats_label();
        if (s_ba.matrix_area)
            gtk_widget_queue_draw(s_ba.matrix_area);
        if (s_ba.timeline_area)
            gtk_widget_queue_draw(s_ba.timeline_area);
        set_status("Analysis complete: %zu candidate field(s).",
                   s_ba.session.candidate_count);
    } else if (s_ba.analysis_running) {
        s_ba.analysis_running = FALSE;
        set_status("Discarded stale Bit Analysis worker result.");
    }
    ba_session_destroy(&work->work_session);
    g_free(work);
    return G_SOURCE_REMOVE;
}

static gpointer analysis_thread_main(gpointer data)
{
    analysis_work_t *work = data;
    ba_session_analyze(&work->work_session, 16);
    g_idle_add(analysis_done_idle, work);
    return NULL;
}

static void on_analyze_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    if (s_ba.analysis_running) {
        set_status("Analysis is already running.");
        return;
    }
    if (s_ba.session.sample_count < 2) {
        set_status("Need at least two target frames before analysis.");
        return;
    }

    analysis_work_t *work = g_new0(analysis_work_t, 1);
    work->generation = ++s_ba.analysis_generation;
    work->work_session = s_ba.session;
    size_t n = s_ba.session.sample_count;
    if (n > BA_UI_ANALYZE_MAX_SAMPLES)
        n = BA_UI_ANALYZE_MAX_SAMPLES;
    if (ba_session_init(&work->work_session, n ? n : 1) != 0) {
        g_free(work);
        set_status("Out of memory starting analysis.");
        return;
    }
    ba_session_t *ws = &work->work_session;
    uint32_t id = s_ba.session.can_id;
    uint8_t ext = s_ba.session.is_extended;
    uint8_t dlc = s_ba.session.expected_dlc;
    uint8_t include_rx = s_ba.session.include_rx;
    uint8_t include_tx = s_ba.session.include_tx;
    uint8_t varying = s_ba.session.accept_varying_dlc;
    uint8_t fd = s_ba.session.analyze_fd_payload;
    ba_input_type_t input_type = s_ba.session.input_type;
    double input_value = s_ba.session.current_input_value;
    uint8_t input_valid = s_ba.session.current_input_valid;
    char input_name[BA_LABEL_MAX], input_unit[BA_UNIT_MAX];
    snprintf(input_name, sizeof(input_name), "%s", s_ba.session.input_name);
    snprintf(input_unit, sizeof(input_unit), "%s", s_ba.session.input_unit);
    ba_session_configure_target(ws, id, ext, dlc);
    ws->include_rx = include_rx;
    ws->include_tx = include_tx;
    ws->accept_varying_dlc = varying;
    ws->analyze_fd_payload = fd;
    ws->input_type = input_type;
    ws->current_input_value = input_value;
    ws->current_input_valid = input_valid;
    snprintf(ws->input_name, sizeof(ws->input_name), "%s", input_name);
    snprintf(ws->input_unit, sizeof(ws->input_unit), "%s", input_unit);
    ws->baseline_valid = s_ba.session.baseline_valid;
    ws->baseline_sample_count = s_ba.session.baseline_sample_count;
    memcpy(ws->baseline_data, s_ba.session.baseline_data,
           sizeof(ws->baseline_data));
    memcpy(ws->baseline_bits, s_ba.session.baseline_bits,
           sizeof(ws->baseline_bits));
    memcpy(ws->segments, s_ba.session.segments, sizeof(ws->segments));
    ws->segment_count = s_ba.session.segment_count;
    ws->next_segment_id = s_ba.session.next_segment_id;
    ws->active_segment = s_ba.session.active_segment;
    ws->total_target_frames = s_ba.session.total_target_frames;
    ws->dropped_samples = s_ba.session.dropped_samples;
    ws->first_ts_ns = s_ba.session.first_ts_ns;
    ws->latest_ts_ns = s_ba.session.latest_ts_ns;
    ws->latest_dlc = s_ba.session.latest_dlc;
    memcpy(ws->latest_data, s_ba.session.latest_data,
           sizeof(ws->latest_data));

    size_t start = s_ba.session.sample_count > n ?
        s_ba.session.sample_count - n : 0;
    for (size_t i = 0; i < n; i++) {
        int src = (int)((s_ba.session.sample_head +
                         s_ba.session.sample_capacity -
                         s_ba.session.sample_count + start + i) %
                        s_ba.session.sample_capacity);
        ws->samples[i] = s_ba.session.samples[src];
    }
    ws->sample_count = n;
    ws->sample_capacity = n ? n : 1;
    ws->sample_head = n == ws->sample_capacity ? 0 : n;
    ws->state = BA_SESSION_ANALYZING;

    s_ba.analysis_running = TRUE;
    s_ba.session.state = BA_SESSION_ANALYZING;
    refresh_summary();
    set_status("Analyzing %zu recent target frame(s) in background.", n);
    GThread *thread = g_thread_new("bit-analysis", analysis_thread_main, work);
    g_thread_unref(thread);
}

static void refresh_summary(void)
{
    if (!s_ba.summary_label)
        return;
    char text[512];
    snprintf(text, sizeof(text),
             "State %s | Target 0x%X %s DLC %u | Samples %zu/%zu | Dropped %llu | Baseline %s | Candidates %zu",
             ba_session_state_name(s_ba.session.state),
             s_ba.session.can_id,
             s_ba.session.is_extended ? "Ext" : "Std",
             s_ba.session.expected_dlc,
             s_ba.session.sample_count,
             s_ba.session.sample_capacity,
             (unsigned long long)s_ba.session.dropped_samples,
             s_ba.session.baseline_valid ? "yes" : "no",
             s_ba.session.candidate_count);
    gtk_label_set_text(GTK_LABEL(s_ba.summary_label), text);
    if (s_ba.state_label)
        gtk_label_set_text(GTK_LABEL(s_ba.state_label),
                           ba_session_state_name(s_ba.session.state));
}

static void refresh_segment_store(void)
{
    if (!s_ba.segment_store)
        return;
    gtk_list_store_clear(s_ba.segment_store);
    for (size_t i = 0; i < s_ba.session.segment_count; i++) {
        const ba_segment_t *seg = &s_ba.session.segments[i];
        char value[64], frames[64], num[24];
        snprintf(num, sizeof(num), "%u", seg->id);
        snprintf(value, sizeof(value), seg->input_valid ? "%.6g" : "-",
                 seg->input_value);
        snprintf(frames, sizeof(frames), "%zu", seg->sample_count);
        GtkTreeIter iter;
        gtk_list_store_append(s_ba.segment_store, &iter);
        gtk_list_store_set(s_ba.segment_store, &iter,
                           SEG_COL_NUM, num,
                           SEG_COL_TYPE, ba_segment_type_name(seg->type),
                           SEG_COL_LABEL, seg->label,
                           SEG_COL_VALUE, value,
                           SEG_COL_FRAMES, frames,
                           -1);
    }
}

static void refresh_candidate_store(void)
{
    if (!s_ba.candidate_store)
        return;
    gtk_list_store_clear(s_ba.candidate_store);
    for (size_t i = 0; i < s_ba.session.candidate_count; i++) {
        const ba_candidate_t *c = &s_ba.session.candidates[i];
        char rank[24], start[24], dbc[24], len[24], pear[32], spear[32];
        char r2[32], factor[32], offset[32], mae[32], val_n[24];
        char val_mae[32], lag[32], trans[32];
        char counter[32], timestamp[32], ts_res[32];
        char checksum[32], mux[32], mux_values[24], mux_frames[24];
        char mux_bits[24], mux_ranges[BA_NOTE_MAX], score[32];
        snprintf(rank, sizeof(rank), "%zu", i + 1);
        snprintf(start, sizeof(start), "%u", c->canonical_start_bit);
        snprintf(dbc, sizeof(dbc), "%u", c->dbc_start_bit);
        snprintf(len, sizeof(len), "%u", c->bit_length);
        snprintf(pear, sizeof(pear), "%.3f", c->pearson);
        snprintf(spear, sizeof(spear), "%.3f", c->spearman);
        snprintf(r2, sizeof(r2), "%.3f", c->r_squared);
        snprintf(factor, sizeof(factor), "%.6g", c->factor);
        snprintf(offset, sizeof(offset), "%.6g", c->offset);
        snprintf(mae, sizeof(mae), "%.4g", c->mean_absolute_error);
        snprintf(val_n, sizeof(val_n), "%u", c->validation_sample_count);
        snprintf(val_mae, sizeof(val_mae), "%.4g",
                 c->validation_mean_absolute_error);
        snprintf(lag, sizeof(lag), "%.3g ms", c->best_lag_ms);
        snprintf(trans, sizeof(trans), "%u/%u",
                 c->transition_matches, c->transition_total);
        snprintf(counter, sizeof(counter), "%.3f", c->counter_score);
        snprintf(timestamp, sizeof(timestamp), "%.3f", c->timestamp_score);
        snprintf(ts_res, sizeof(ts_res), "%.4g ms",
                 c->timestamp_resolution_ms);
        snprintf(checksum, sizeof(checksum), "%.3f", c->checksum_score);
        snprintf(mux, sizeof(mux), "%.3f", c->mux_score);
        snprintf(mux_values, sizeof(mux_values), "%u",
                 c->mux_unique_values);
        snprintf(mux_frames, sizeof(mux_frames), "%u",
                 c->mux_min_frames_per_value);
        snprintf(mux_bits, sizeof(mux_bits), "%u",
                 c->mux_conditionally_active_bits);
        snprintf(mux_ranges, sizeof(mux_ranges), "%s",
                 c->mux_active_ranges[0] ? c->mux_active_ranges : "-");
        snprintf(score, sizeof(score), "%.3f", c->total_score);

        GtkTreeIter iter;
        gtk_list_store_append(s_ba.candidate_store, &iter);
        gtk_list_store_set(s_ba.candidate_store, &iter,
                           CAND_COL_RANK, rank,
                           CAND_COL_START, start,
                           CAND_COL_DBC_START, dbc,
                           CAND_COL_LEN, len,
                           CAND_COL_ORDER, ba_byte_order_name(c->byte_order),
                           CAND_COL_SIGNED, c->is_signed ? "Signed" : "Unsigned",
                           CAND_COL_PEARSON, pear,
                           CAND_COL_SPEARMAN, spear,
                           CAND_COL_R2, r2,
                           CAND_COL_FACTOR, factor,
                           CAND_COL_OFFSET, offset,
                           CAND_COL_MAE, mae,
                           CAND_COL_VAL_N, val_n,
                           CAND_COL_VAL_MAE, val_mae,
                           CAND_COL_LAG, lag,
                           CAND_COL_TRANSITIONS, trans,
                           CAND_COL_COUNTER, counter,
                           CAND_COL_TIMESTAMP, timestamp,
                           CAND_COL_TS_RES, ts_res,
                           CAND_COL_CHECKSUM, checksum,
                           CAND_COL_MUX, mux,
                           CAND_COL_MUX_VALUES, mux_values,
                           CAND_COL_MUX_FRAMES, mux_frames,
                           CAND_COL_MUX_BITS, mux_bits,
                           CAND_COL_MUX_RANGES, mux_ranges,
                           CAND_COL_SCORE, score,
                           CAND_COL_CONF, c->confidence,
                           CAND_COL_INDEX, (guint)i,
                           -1);
    }
}

static void refresh_inspector(const ba_candidate_t *c)
{
    if (!s_ba.inspector_label)
        return;
    if (!c) {
        gtk_label_set_text(GTK_LABEL(s_ba.inspector_label),
                           "No candidate selected.");
        return;
    }
    char text[1024];
    snprintf(text, sizeof(text),
             "%s %u|%u (%s, %s)\n"
             "DBC start %u | raw %lld..%lld / %llu..%llu\n"
             "factor %.8g offset %.8g | R2 %.4g | fit MAE %.4g | max error %.4g | lag %.4g ms\n"
             "Validation %u frame(s) | MAE %.4g | max error %.4g\n"
             "Pearson %.4g | Spearman %.4g | transitions %u/%u | counter %.4g | timestamp %.4g | score %.4g | %s\n"
             "Timestamp resolution %.6g ms/count | wrap %llu | checksum-like %.4g\n"
             "Mux %.4g | values %u | min frames/value %u | active bits %u | ranges %s",
             c->proposed_name,
             c->canonical_start_bit,
             c->bit_length,
             ba_byte_order_name(c->byte_order),
             c->is_signed ? "signed" : "unsigned",
             c->dbc_start_bit,
             (long long)c->raw_min_signed,
             (long long)c->raw_max_signed,
             (unsigned long long)c->raw_min_unsigned,
             (unsigned long long)c->raw_max_unsigned,
             c->factor,
             c->offset,
             c->r_squared,
             c->mean_absolute_error,
             c->max_absolute_error,
             c->best_lag_ms,
             c->validation_sample_count,
             c->validation_mean_absolute_error,
             c->validation_max_absolute_error,
             c->pearson,
             c->spearman,
             c->transition_matches,
             c->transition_total,
             c->counter_score,
             c->timestamp_score,
             c->total_score,
             c->confidence,
             c->timestamp_resolution_ms,
             (unsigned long long)c->timestamp_wrap_value,
             c->checksum_score,
             c->mux_score,
             c->mux_unique_values,
             c->mux_min_frames_per_value,
             c->mux_conditionally_active_bits,
             c->mux_active_ranges[0] ? c->mux_active_ranges : "-");
    gtk_label_set_text(GTK_LABEL(s_ba.inspector_label), text);
}

static void on_candidate_selection_changed(GtkTreeSelection *selection,
                                           gpointer data)
{
    (void)data;
    GtkTreeIter iter;
    GtkTreeModel *model = NULL;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        s_ba.selected_candidate = -1;
        refresh_inspector(NULL);
        return;
    }
    guint idx = 0;
    gtk_tree_model_get(model, &iter, CAND_COL_INDEX, &idx, -1);
    if (idx >= s_ba.session.candidate_count)
        return;
    s_ba.selected_candidate = (int)idx;
    const ba_candidate_t *c = &s_ba.session.candidates[idx];
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.field_start_spin),
                              c->canonical_start_bit);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.field_len_spin),
                              c->bit_length);
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.field_order_combo),
                             c->byte_order == BA_BYTE_ORDER_MOTOROLA ? 1 : 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s_ba.field_signed_check),
                                 c->is_signed);
    refresh_inspector(c);
    refresh_field_preview();
    refresh_bit_stats_label();
    if (s_ba.matrix_area)
        gtk_widget_queue_draw(s_ba.matrix_area);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
}

static void refresh_field_preview(void)
{
    if (!s_ba.field_raw_label || !s_ba.field_phys_label)
        return;
    uint16_t start = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_start_spin));
    uint8_t len = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_len_spin));
    ba_byte_order_t order = active_field_order();
    int is_signed = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_ba.field_signed_check));

    uint8_t dlc = s_ba.session.latest_dlc ? s_ba.session.latest_dlc :
                  s_ba.session.expected_dlc;
    uint64_t raw = ba_extract_unsigned(s_ba.session.latest_data, dlc, start,
                                       len, order);
    int64_t sraw = ba_sign_extend(raw, len);

    const ba_candidate_t *c = NULL;
    if (s_ba.selected_candidate >= 0 &&
        (size_t)s_ba.selected_candidate < s_ba.session.candidate_count)
        c = &s_ba.session.candidates[s_ba.selected_candidate];
    double phys = c ? c->offset + c->factor * (is_signed ? (double)sraw :
                  (double)raw) : (is_signed ? (double)sraw : (double)raw);

    char rbuf[64], pbuf[64];
    snprintf(rbuf, sizeof(rbuf), is_signed ? "%lld" : "%llu",
             is_signed ? (long long)sraw : (long long)raw);
    snprintf(pbuf, sizeof(pbuf), "%.8g", phys);
    gtk_label_set_text(GTK_LABEL(s_ba.field_raw_label), rbuf);
    gtk_label_set_text(GTK_LABEL(s_ba.field_phys_label), pbuf);
}

static void refresh_bit_stats_label(void)
{
    if (!s_ba.bit_stats_label)
        return;

    int bit = s_ba.selected_bit;
    if (bit < 0 && s_ba.field_start_spin) {
        bit = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(s_ba.field_start_spin));
    }
    uint16_t total_bits = (uint16_t)(s_ba.session.analyze_fd_payload ?
        (s_ba.session.expected_dlc ? s_ba.session.expected_dlc :
         s_ba.session.latest_dlc) : BA_CLASSIC_MAX_BYTES) * 8u;
    if (total_bits == 0)
        total_bits = BA_CLASSIC_MAX_BITS;
    if (total_bits > BA_FD_MAX_BITS)
        total_bits = BA_FD_MAX_BITS;

    if (bit < 0 || bit >= (int)total_bits) {
        gtk_label_set_text(GTK_LABEL(s_ba.bit_stats_label),
                           "Bit: - | no selection");
        return;
    }

    const ba_bit_stats_t *st = &s_ba.session.bit_stats[bit];
    uint8_t live = ba_get_bit(s_ba.session.latest_data, (uint16_t)bit);
    char baseline[8];
    if (s_ba.session.baseline_valid)
        snprintf(baseline, sizeof(baseline), "%u",
                 s_ba.session.baseline_bits[bit]);
    else
        snprintf(baseline, sizeof(baseline), "n/a");
    char text[384];
    snprintf(text, sizeof(text),
             "Bit %d (B%d.b%d) | live %u | baseline %s | flips %llu | P1 %.3f | entropy %.3f | separation %.3f | corr %.3f | lag %.3g ms | lag score %.3f",
             bit,
             bit / 8,
             bit % 8,
             live,
             baseline,
             (unsigned long long)st->flip_count,
             st->sample_count ? st->probability_one : 0.0,
             st->sample_count ? st->entropy_bits : 0.0,
             st->state_separation,
             st->pearson_correlation,
             st->best_lag_ms,
             st->lagged_score);
    gtk_label_set_text(GTK_LABEL(s_ba.bit_stats_label), text);
    gtk_widget_set_tooltip_text(s_ba.bit_stats_label, text);
}

static double cell_score_for_bit(uint16_t bit)
{
    if (bit >= BA_FD_MAX_BITS)
        return 0.0;
    const ba_bit_stats_t *st = &s_ba.session.bit_stats[bit];
    ba_view_mode_t mode = gtk_combo_box_get_active(GTK_COMBO_BOX(s_ba.mode_combo));
    switch (mode) {
    case BA_VIEW_BASELINE:
        return s_ba.session.baseline_valid ?
            (double)s_ba.session.baseline_bits[bit] : 0.0;
    case BA_VIEW_XOR:
        return s_ba.session.baseline_valid ?
            (double)(ba_get_bit(s_ba.session.latest_data, bit) ^
                     s_ba.session.baseline_bits[bit]) : 0.0;
    case BA_VIEW_ACTIVITY:
        return fmin((double)st->flip_count / 20.0, 1.0);
    case BA_VIEW_ENTROPY:
        return st->entropy_bits;
    case BA_VIEW_CORRELATION:
        return fmax(fmax(fabs(st->pearson_correlation),
                         fabs(st->lagged_score)),
                    st->state_separation);
    case BA_VIEW_LIVE:
    default:
        return ba_get_bit(s_ba.session.latest_data, bit) ? 1.0 : 0.0;
    }
}

static gboolean on_matrix_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    double W = a.width, H = a.height;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);

    double left = BA_MATRIX_LEFT, top = BA_MATRIX_TOP;
    double cw = (W - left - BA_MATRIX_PAD_R) / 8.0;
    double ch = (H - top - BA_MATRIX_PAD_B) / 8.0;
    if (cw < BA_MATRIX_CELL_MIN || ch < BA_MATRIX_CELL_MIN)
        return FALSE;

    for (int col = 0; col < 8; col++) {
        char label[8];
        snprintf(label, sizeof(label), "b%d", col);
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_move_to(cr, left + col * cw + cw * 0.35, 16);
        cairo_show_text(cr, label);
    }

    uint16_t fstart = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_start_spin));
    uint8_t flen = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_len_spin));

    for (int byte = 0; byte < 8; byte++) {
        char label[8];
        snprintf(label, sizeof(label), "B%d", byte);
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_move_to(cr, 8, top + byte * ch + ch * 0.58);
        cairo_show_text(cr, label);

        for (int bit_in_byte = 0; bit_in_byte < 8; bit_in_byte++) {
            uint16_t bit = (uint16_t)(byte * 8 + bit_in_byte);
            double x = left + bit_in_byte * cw;
            double y = top + byte * ch;
            double score = cell_score_for_bit(bit);
            uint8_t live_v = ba_get_bit(s_ba.session.latest_data, bit);

            ba_view_mode_t mode = gtk_combo_box_get_active(
                GTK_COMBO_BOX(s_ba.mode_combo));
            uint8_t v = mode == BA_VIEW_BASELINE ?
                        s_ba.session.baseline_bits[bit] : live_v;
            if (mode == BA_VIEW_LIVE) {
                if (v)
                    cairo_set_source_rgb(cr, 0.08, 0.35, 0.75);
                else
                    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
            } else if (mode == BA_VIEW_BASELINE) {
                if (!s_ba.session.baseline_valid)
                    cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
                else if (v)
                    cairo_set_source_rgb(cr, 0.05, 0.48, 0.32);
                else
                    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
            } else if (mode == BA_VIEW_XOR) {
                cairo_set_source_rgb(cr, score > 0.5 ? 0.92 : 0.94,
                                     score > 0.5 ? 0.28 : 0.94,
                                     score > 0.5 ? 0.18 : 0.94);
            } else if (mode == BA_VIEW_CORRELATION) {
                cairo_set_source_rgb(cr, 1.0, 1.0 - 0.75 * score,
                                     1.0 - 0.85 * score);
            } else {
                cairo_set_source_rgb(cr, 1.0 - 0.65 * score,
                                     1.0 - 0.25 * score,
                                     1.0);
            }
            cairo_rectangle(cr, x + 1, y + 1, cw - 2, ch - 2);
            cairo_fill(cr);

            if (bit == s_ba.selected_bit ||
                (bit >= fstart && bit < (uint16_t)(fstart + flen))) {
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                cairo_set_line_width(cr, bit == s_ba.selected_bit ? 2.0 : 1.2);
                cairo_rectangle(cr, x + 1.5, y + 1.5, cw - 3, ch - 3);
                cairo_stroke(cr);
            }

            char txt[8];
            snprintf(txt, sizeof(txt), "%u", v);
            cairo_set_source_rgb(cr, mode == BA_VIEW_LIVE && v ? 1 : 0.08,
                                 mode == BA_VIEW_LIVE && v ? 1 : 0.08,
                                 mode == BA_VIEW_LIVE && v ? 1 : 0.08);
            cairo_move_to(cr, x + cw * 0.43, y + ch * 0.60);
            cairo_show_text(cr, txt);
        }
    }
    return FALSE;
}

static gboolean on_matrix_button(GtkWidget *widget, GdkEventButton *event,
                                 gpointer data)
{
    (void)data;
    if (event->button != 1)
        return FALSE;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    double left = 44, top = 26;
    double cw = (a.width - left - 10) / 8.0;
    double ch = (a.height - top - 12) / 8.0;
    int col = (int)((event->x - left) / cw);
    int row = (int)((event->y - top) / ch);
    if (row < 0 || row >= 8 || col < 0 || col >= 8)
        return FALSE;
    int bit = row * 8 + col;
    s_ba.selected_bit = bit;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.field_start_spin), bit);
    refresh_field_preview();
    refresh_bit_stats_label();
    gtk_widget_queue_draw(widget);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
    return TRUE;
}

static gboolean on_timeline_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    double W = a.width, H = a.height;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);

    double x0 = 42, x1 = W - 12, y0 = 16, y1 = H - 24;
    if (x1 <= x0 || y1 <= y0)
        return FALSE;
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    for (int i = 0; i <= 5; i++) {
        double x = x0 + (x1 - x0) * i / 5.0;
        cairo_move_to(cr, x, y0);
        cairo_line_to(cr, x, y1);
        cairo_stroke(cr);
    }
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);

    if (s_ba.session.sample_count == 0) {
        cairo_move_to(cr, x0 + 10, y0 + 22);
        cairo_show_text(cr, "No target samples");
        return FALSE;
    }

    uint16_t start = (uint16_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_start_spin));
    uint8_t len = (uint8_t)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(s_ba.field_len_spin));
    ba_byte_order_t order = active_field_order();
    int is_signed = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(s_ba.field_signed_check));

    size_t n = s_ba.session.sample_count;
    size_t begin = n > 800 ? n - 800 : 0;
    const ba_sample_t *first = &s_ba.session.samples[
        (s_ba.session.sample_head + s_ba.session.sample_capacity -
         s_ba.session.sample_count + begin) % s_ba.session.sample_capacity];
    const ba_sample_t *last = &s_ba.session.samples[
        (s_ba.session.sample_head + s_ba.session.sample_capacity - 1) %
        s_ba.session.sample_capacity];
    double t0 = (double)(first->timestamp_ns - s_ba.session.first_ts_ns) / 1e9;
    double t1 = (double)(last->timestamp_ns - s_ba.session.first_ts_ns) / 1e9;
    if (t1 <= t0)
        t1 = t0 + 1.0;

    double minv = 0.0, maxv = 1.0;
    int have = 0;
    for (size_t li = begin; li < n; li++) {
        int idx = (int)((s_ba.session.sample_head +
                         s_ba.session.sample_capacity -
                         s_ba.session.sample_count + li) %
                        s_ba.session.sample_capacity);
        const ba_sample_t *sample = &s_ba.session.samples[idx];
        uint64_t raw = ba_extract_unsigned(sample->data, sample->dlc,
                                           start, len, order);
        double v = is_signed ? (double)ba_sign_extend(raw, len) : (double)raw;
        if (!have) {
            minv = maxv = v;
            have = 1;
        } else {
            if (v < minv) minv = v;
            if (v > maxv) maxv = v;
        }
    }
    if (maxv <= minv) {
        minv -= 0.5;
        maxv += 0.5;
    }
    double span = maxv - minv;

    cairo_set_source_rgb(cr, 0.08, 0.35, 0.75);
    cairo_set_line_width(cr, 1.6);
    gboolean started = FALSE;
    for (size_t li = begin; li < n; li++) {
        int idx = (int)((s_ba.session.sample_head +
                         s_ba.session.sample_capacity -
                         s_ba.session.sample_count + li) %
                        s_ba.session.sample_capacity);
        const ba_sample_t *sample = &s_ba.session.samples[idx];
        double t = (double)(sample->timestamp_ns -
                            s_ba.session.first_ts_ns) / 1e9;
        uint64_t raw = ba_extract_unsigned(sample->data, sample->dlc,
                                           start, len, order);
        double v = is_signed ? (double)ba_sign_extend(raw, len) : (double)raw;
        double x = x0 + (t - t0) / (t1 - t0) * (x1 - x0);
        double y = y1 - (v - minv) / span * (y1 - y0);
        if (!started) {
            cairo_move_to(cr, x, y);
            started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    if (started)
        cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    char label[96];
    snprintf(label, sizeof(label), "Field %u|%u raw", start, len);
    cairo_move_to(cr, x0 + 8, y0 + 14);
    cairo_show_text(cr, label);
    return FALSE;
}

static void on_field_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    refresh_field_preview();
    refresh_bit_stats_label();
    if (s_ba.matrix_area)
        gtk_widget_queue_draw(s_ba.matrix_area);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
}

static void add_analysis_filter(GtkFileChooser *chooser)
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "CANoScope analysis sessions");
    gtk_file_filter_add_pattern(filter, "*.canoscope-analysis.json");
    gtk_file_chooser_add_filter(chooser, filter);

    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(chooser, all);
}

static void refresh_loaded_session_view(void)
{
    sync_controls_from_session();
    s_ba.selected_candidate = -1;
    s_ba.selected_bit = -1;
    refresh_segment_store();
    refresh_candidate_store();
    refresh_summary();
    refresh_field_preview();
    refresh_bit_stats_label();
    refresh_inspector(NULL);
    if (s_ba.matrix_area)
        gtk_widget_queue_draw(s_ba.matrix_area);
    if (s_ba.timeline_area)
        gtk_widget_queue_draw(s_ba.timeline_area);
}

static gboolean save_session_with_dialog(void)
{
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save Bit Analysis Session",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),
                                                   TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
                                      "bit-analysis.canoscope-analysis.json");
    add_analysis_filter(GTK_FILE_CHOOSER(dialog));

    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char *path = filename;
        if (!g_str_has_suffix(filename, ".canoscope-analysis.json"))
            path = g_strconcat(filename, ".canoscope-analysis.json", NULL);

        char err[256] = {0};
        if (ba_session_save_json(&s_ba.session, path, err, sizeof(err)) == 0) {
            set_status("Saved Bit Analysis session to %s.", path);
            saved = TRUE;
        } else {
            set_status("Session save failed: %s", err);
        }
        if (path != filename)
            g_free(path);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
    return saved;
}

static void on_save_session_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    save_session_with_dialog();
}

static void on_load_session_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Load Bit Analysis Session",
        GTK_WINDOW(g_gui.window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);
    add_analysis_filter(GTK_FILE_CHOOSER(dialog));

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        char err[256] = {0};
        if (ba_session_load_json(&s_ba.session, filename, err, sizeof(err)) == 0) {
            invalidate_analysis_results();
            refresh_loaded_session_view();
            set_status("Loaded Bit Analysis session from %s.", filename);
        } else {
            set_status("Session load failed: %s", err);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_mark_validated_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    if (s_ba.selected_candidate < 0 ||
        (size_t)s_ba.selected_candidate >= s_ba.session.candidate_count) {
        set_status("Select a candidate to validate.");
        return;
    }

    ba_candidate_t *c = &s_ba.session.candidates[s_ba.selected_candidate];
    size_t validation_n = 0;
    double validation_mae = 0.0;
    double validation_maxe = 0.0;
    if (!ba_candidate_validate_segments(&s_ba.session, c, -1.0,
                                        &validation_n,
                                        &validation_mae,
                                        &validation_maxe)) {
        if (validation_n == 0) {
            set_status("Add a Validation segment with target frames and input values before marking a candidate validated.");
        } else {
            set_status("Validation failed on %zu frame(s): MAE %.6g, max error %.6g.",
                       validation_n, validation_mae, validation_maxe);
        }
        return;
    }

    snprintf(c->confidence, sizeof(c->confidence), "Validated");
    c->validation_sample_count = (uint32_t)validation_n;
    c->validation_mean_absolute_error = validation_mae;
    c->validation_max_absolute_error = validation_maxe;
    if (!c->notes[0])
        snprintf(c->notes, sizeof(c->notes),
                 "Validated against %zu validation frame(s): MAE %.6g, max error %.6g.",
                 validation_n, validation_mae, validation_maxe);
    refresh_candidate_store();
    refresh_inspector(c);
    set_status("Marked %s as Validated on %zu frame(s): MAE %.6g.",
               c->proposed_name, validation_n, validation_mae);
}

static void on_promote_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    if (s_ba.selected_candidate < 0 ||
        (size_t)s_ba.selected_candidate >= s_ba.session.candidate_count) {
        set_status("Select a candidate to promote.");
        return;
    }
    const ba_candidate_t *c = &s_ba.session.candidates[s_ba.selected_candidate];
    char comment[DBC_COMMENT_MAX];
    snprintf(comment, sizeof(comment),
             "Reverse engineered with CANoScope Bit Analysis. Confidence: %s. "
             "Evidence: frames %u, R2 %.6g, fit MAE %.6g, lag %.6g ms, "
             "validation frames %u, validation MAE %.6g.",
             c->confidence[0] ? c->confidence : "Unknown",
             c->sample_count,
             c->r_squared,
             c->mean_absolute_error,
             c->best_lag_ms,
             c->validation_sample_count,
             c->validation_mean_absolute_error);
    gui_db_creation_prefill_signal(
        s_ba.session.can_id,
        s_ba.session.is_extended,
        s_ba.session.expected_dlc,
        c->proposed_name,
        c->dbc_start_bit,
        c->bit_length,
        c->byte_order == BA_BYTE_ORDER_INTEL ? 1 : 0,
        c->is_signed,
        c->factor,
        c->offset,
        c->physical_min,
        c->physical_max,
        c->unit[0] ? c->unit : s_ba.session.input_unit,
        comment);
    set_status("Promoted candidate %s to DB Creation for review.",
               c->proposed_name);
}

void gui_bit_analysis_reset(void)
{
    on_reset_clicked(NULL, NULL);
}

void gui_bit_analysis_trace_changed(void)
{
    s_ba.dirty_survey = TRUE;
}

void gui_bit_analysis_handle_message(const can_msg_t *msg)
{
    if (!msg)
        return;
    update_survey(msg);
    if (ba_session_push_frame(&s_ba.session, msg)) {
        s_ba.dirty_view = TRUE;
        if (s_ba.session.active_segment >= 0 &&
            (size_t)s_ba.session.active_segment < s_ba.session.segment_count) {
            ba_segment_t *seg =
                &s_ba.session.segments[s_ba.session.active_segment];
            seg->sample_count++;
        }
    }
}

static GtkWidget *make_tree(GtkListStore *store)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(view),
                                 GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    return view;
}

static void add_text_column(GtkWidget *view, const char *title,
                            int model_col, int min_width)
{
    GtkCellRenderer *rend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        title, rend, "text", model_col, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_min_width(col, min_width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
}

static void set_paned_ratio_on_allocate(GtkWidget *widget,
                                        GtkAllocation *allocation,
                                        gpointer data)
{
    int per_mille = GPOINTER_TO_INT(data);
    if (per_mille <= 0 || per_mille >= 1000)
        return;

    GtkOrientation orientation =
        gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));
    int span = orientation == GTK_ORIENTATION_HORIZONTAL ?
               allocation->width : allocation->height;
    int last_span = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(widget), "canoscope-paned-span"));
    if (span <= 0 || span == last_span)
        return;

    g_object_set_data(G_OBJECT(widget), "canoscope-paned-span",
                      GINT_TO_POINTER(span));
    gtk_paned_set_position(GTK_PANED(widget), span * per_mille / 1000);
}

GtkWidget *gui_create_bit_analysis_view(void)
{
    if (!s_ba.session.samples)
        ba_session_init(&s_ba.session, BA_DEFAULT_CAPACITY);
    s_ba.selected_bit = -1;
    s_ba.selected_candidate = -1;

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(outer, TRUE);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    gtk_widget_set_margin_top(outer, 8);
    gtk_widget_set_margin_bottom(outer, 8);

    GtkWidget *target_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(outer), target_bar, FALSE, FALSE, 0);
    GtkWidget *target_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *target_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(target_bar), target_controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_bar), target_actions, FALSE, FALSE, 0);

    s_ba.target_store = gtk_list_store_new(SURV_COL_NUM,
        G_TYPE_STRING, G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_UINT,
        G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_UINT64, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING);
    s_ba.target_combo = gtk_combo_box_new_with_model(
        GTK_TREE_MODEL(s_ba.target_store));
    GtkCellRenderer *target_rend = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(s_ba.target_combo),
                               target_rend, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(s_ba.target_combo),
                                  target_rend, "text", SURV_COL_LABEL);
    gtk_widget_set_size_request(s_ba.target_combo, 300, -1);
    g_signal_connect(s_ba.target_combo, "changed",
                     G_CALLBACK(on_target_changed), NULL);

    gtk_box_pack_start(GTK_BOX(target_controls), gtk_label_new("Target:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_controls), s_ba.target_combo,
                       FALSE, FALSE, 0);

    s_ba.dir_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.dir_combo), "RX");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.dir_combo), "TX");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.dir_combo), "Both");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.dir_combo), 0);
    gtk_box_pack_start(GTK_BOX(target_controls), gtk_label_new("Dir:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_controls), s_ba.dir_combo,
                       FALSE, FALSE, 0);

    s_ba.dlc_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.dlc_spin), 8);
    gtk_box_pack_start(GTK_BOX(target_controls), gtk_label_new("DLC:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_controls), s_ba.dlc_spin,
                       FALSE, FALSE, 0);

    s_ba.accept_varying_check = gtk_check_button_new_with_label("Varying DLC");
    s_ba.fd_full_check = gtk_check_button_new_with_label("FD full payload");
    gtk_box_pack_start(GTK_BOX(target_controls), s_ba.accept_varying_check,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_controls), s_ba.fd_full_check,
                       FALSE, FALSE, 0);

    GtkWidget *start_btn = gtk_button_new_with_label("Start");
    GtkWidget *pause_btn = gtk_button_new_with_label("Pause/Resume");
    GtkWidget *reset_btn = gtk_button_new_with_label("Reset");
    GtkWidget *baseline_btn = gtk_button_new_with_label("Capture Baseline");
    GtkWidget *analyze_btn = gtk_button_new_with_label("Analyze");
    GtkWidget *save_session_btn = gtk_button_new_with_label("Save Session");
    GtkWidget *load_session_btn = gtk_button_new_with_label("Load Session");
    g_signal_connect(start_btn, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(pause_btn, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked), NULL);
    g_signal_connect(baseline_btn, "clicked",
                     G_CALLBACK(on_baseline_clicked), NULL);
    g_signal_connect(analyze_btn, "clicked",
                     G_CALLBACK(on_analyze_clicked), NULL);
    g_signal_connect(save_session_btn, "clicked",
                     G_CALLBACK(on_save_session_clicked), NULL);
    g_signal_connect(load_session_btn, "clicked",
                     G_CALLBACK(on_load_session_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(target_actions), start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), pause_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), baseline_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), analyze_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), save_session_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(target_actions), load_session_btn, FALSE, FALSE, 0);

    GtkWidget *exp_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(outer), exp_bar, FALSE, FALSE, 0);
    s_ba.input_name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(s_ba.input_name_entry), "Input");
    gtk_widget_set_size_request(s_ba.input_name_entry, 150, -1);
    s_ba.input_type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.input_type_combo),
                                   "Boolean");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.input_type_combo),
                                   "Enumerated");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.input_type_combo),
                                   "Continuous");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.input_type_combo),
                                   "Event");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.input_type_combo),
                             BA_INPUT_CONTINUOUS);
    s_ba.input_value_spin = gtk_spin_button_new_with_range(-1e9, 1e9, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s_ba.input_value_spin), 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.input_value_spin), 0.0);
    s_ba.input_unit_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(s_ba.input_unit_entry), BA_UNIT_MAX - 1);
    gtk_widget_set_size_request(s_ba.input_unit_entry, 70, -1);
    s_ba.segment_type_combo = gtk_combo_box_text_new();
    for (int i = 0; i <= BA_SEGMENT_EVENT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.segment_type_combo),
                                       ba_segment_type_name((ba_segment_type_t)i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.segment_type_combo),
                             BA_SEGMENT_STEP);
    s_ba.segment_label_entry = gtk_entry_new();
    gtk_widget_set_size_request(s_ba.segment_label_entry, 140, -1);
    GtkWidget *mark_btn = gtk_button_new_with_label("Add Marker");
    GtkWidget *end_mark_btn = gtk_button_new_with_label("End Segment");
    g_signal_connect(mark_btn, "clicked",
                     G_CALLBACK(on_add_marker_clicked), NULL);
    g_signal_connect(end_mark_btn, "clicked",
                     G_CALLBACK(on_end_marker_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(exp_bar), gtk_label_new("Input:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.input_name_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.input_type_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.input_value_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.input_unit_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.segment_type_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), s_ba.segment_label_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), mark_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(exp_bar), end_mark_btn, FALSE, FALSE, 0);

    s_ba.summary_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s_ba.summary_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(outer), s_ba.summary_label, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);
    g_signal_connect(paned, "size-allocate",
                     G_CALLBACK(set_paned_ratio_on_allocate),
                     GINT_TO_POINTER(500));
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_vexpand(left, TRUE);
    gtk_widget_set_size_request(left, 420, -1);
    gtk_paned_pack1(GTK_PANED(paned), left, TRUE, TRUE);

    GtkWidget *survey_frame = gtk_frame_new("Live CAN ID Survey");
    gtk_widget_set_size_request(survey_frame, -1, GUI_LIST_MIN_H);
    gtk_box_pack_start(GTK_BOX(left), survey_frame, TRUE, TRUE, 0);
    GtkWidget *survey_view = make_tree(s_ba.target_store);
    add_text_column(survey_view, "Message", SURV_COL_LABEL, 220);
    add_text_column(survey_view, "Mean", SURV_COL_MEAN, 70);
    add_text_column(survey_view, "Jitter", SURV_COL_JITTER, 70);
    add_text_column(survey_view, "Bits", SURV_COL_CHANGED, 50);
    add_text_column(survey_view, "Change", SURV_COL_CHANGE_RATE, 70);
    add_text_column(survey_view, "Unique", SURV_COL_UNIQUE, 60);
    GtkWidget *survey_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(survey_scroll), survey_view);
    gtk_container_add(GTK_CONTAINER(survey_frame), survey_scroll);

    GtkWidget *seg_frame = gtk_frame_new("Experiment Segments");
    gtk_widget_set_size_request(seg_frame, -1, GUI_LIST_MIN_H);
    gtk_box_pack_start(GTK_BOX(left), seg_frame, TRUE, TRUE, 0);
    s_ba.segment_store = gtk_list_store_new(SEG_COL_NUM_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING);
    s_ba.segment_view = make_tree(s_ba.segment_store);
    add_text_column(s_ba.segment_view, "#", SEG_COL_NUM, 36);
    add_text_column(s_ba.segment_view, "Type", SEG_COL_TYPE, 70);
    add_text_column(s_ba.segment_view, "Label", SEG_COL_LABEL, 120);
    add_text_column(s_ba.segment_view, "Input", SEG_COL_VALUE, 70);
    add_text_column(s_ba.segment_view, "Frames", SEG_COL_FRAMES, 60);
    GtkWidget *seg_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(seg_scroll), s_ba.segment_view);
    gtk_container_add(GTK_CONTAINER(seg_frame), seg_scroll);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_vexpand(right, TRUE);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, TRUE);

    GtkWidget *field_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(right), field_bar, FALSE, FALSE, 0);
    GtkWidget *field_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *field_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(field_bar), field_controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_bar), field_actions, FALSE, FALSE, 0);
    s_ba.field_start_spin = gtk_spin_button_new_with_range(0, 511, 1);
    s_ba.field_len_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_ba.field_len_spin), 1);
    s_ba.field_order_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.field_order_combo), "Intel");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.field_order_combo), "Motorola");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.field_order_combo), 0);
    s_ba.field_signed_check = gtk_check_button_new_with_label("Signed");
    s_ba.field_raw_label = gtk_label_new("-");
    s_ba.field_phys_label = gtk_label_new("-");
    GtkWidget *validated_btn = gtk_button_new_with_label("Mark Validated");
    GtkWidget *promote_btn = gtk_button_new_with_label("Promote to DB Creation");
    g_signal_connect(validated_btn, "clicked",
                     G_CALLBACK(on_mark_validated_clicked), NULL);
    g_signal_connect(promote_btn, "clicked",
                     G_CALLBACK(on_promote_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(field_controls), gtk_label_new("Field start:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_start_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), gtk_label_new("Len:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_len_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_order_combo,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_signed_check,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), gtk_label_new("Raw:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_raw_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), gtk_label_new("Fit:"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_controls), s_ba.field_phys_label,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_actions), validated_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field_actions), promote_btn, FALSE, FALSE, 0);

    GtkWidget *matrix_frame = gtk_frame_new("64-bit Matrix");
    gtk_box_pack_start(GTK_BOX(right), matrix_frame, TRUE, TRUE, 0);
    GtkWidget *matrix_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(matrix_frame), matrix_box);
    GtkWidget *mode_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(matrix_box), mode_bar, FALSE, FALSE, 0);
    s_ba.mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Live");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Baseline");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Baseline XOR");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Activity");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Entropy");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_ba.mode_combo), "Correlation");
    gtk_combo_box_set_active(GTK_COMBO_BOX(s_ba.mode_combo), BA_VIEW_LIVE);
    g_signal_connect(s_ba.mode_combo, "changed",
                     G_CALLBACK(on_field_changed), NULL);
    gtk_box_pack_start(GTK_BOX(mode_bar), gtk_label_new("Mode:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_bar), s_ba.mode_combo, FALSE, FALSE, 0);
    s_ba.matrix_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_ba.matrix_area,
                                BA_MATRIX_MIN_W, BA_MATRIX_MIN_H);
    gtk_widget_set_hexpand(s_ba.matrix_area, TRUE);
    gtk_widget_set_vexpand(s_ba.matrix_area, TRUE);
    gtk_widget_add_events(s_ba.matrix_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(s_ba.matrix_area, "draw", G_CALLBACK(on_matrix_draw), NULL);
    g_signal_connect(s_ba.matrix_area, "button-press-event",
                     G_CALLBACK(on_matrix_button), NULL);
    gtk_box_pack_start(GTK_BOX(matrix_box), s_ba.matrix_area, TRUE, TRUE, 0);
    s_ba.bit_stats_label = gtk_label_new("Bit: - | no selection");
    gtk_label_set_xalign(GTK_LABEL(s_ba.bit_stats_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(s_ba.bit_stats_label),
                            PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(matrix_box), s_ba.bit_stats_label,
                       FALSE, FALSE, 0);

    GtkWidget *timeline_frame = gtk_frame_new("Selected Field Timeline");
    gtk_widget_set_hexpand(timeline_frame, TRUE);
    gtk_widget_set_vexpand(timeline_frame, TRUE);
    gtk_box_pack_start(GTK_BOX(right), timeline_frame, TRUE, TRUE, 0);
    s_ba.timeline_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(s_ba.timeline_area,
                                BA_TIMELINE_MIN_W, BA_TIMELINE_MIN_H);
    gtk_widget_set_hexpand(s_ba.timeline_area, TRUE);
    gtk_widget_set_vexpand(s_ba.timeline_area, TRUE);
    g_signal_connect(s_ba.timeline_area, "draw",
                     G_CALLBACK(on_timeline_draw), NULL);
    gtk_container_add(GTK_CONTAINER(timeline_frame), s_ba.timeline_area);

    GtkWidget *watch[] = {
        s_ba.field_start_spin, s_ba.field_len_spin,
        s_ba.field_order_combo, s_ba.field_signed_check
    };
    for (size_t i = 0; i < sizeof(watch) / sizeof(watch[0]); i++) {
        const char *sig = GTK_IS_SPIN_BUTTON(watch[i]) ? "value-changed" :
                          GTK_IS_TOGGLE_BUTTON(watch[i]) ? "toggled" :
                          "changed";
        g_signal_connect(watch[i], sig, G_CALLBACK(on_field_changed), NULL);
    }

    GtkWidget *cand_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand(cand_paned, TRUE);
    gtk_widget_set_vexpand(cand_paned, TRUE);
    g_signal_connect(cand_paned, "size-allocate",
                     G_CALLBACK(set_paned_ratio_on_allocate),
                     GINT_TO_POINTER(720));
    gtk_box_pack_start(GTK_BOX(left), cand_paned, TRUE, TRUE, 0);

    GtkWidget *cand_frame = gtk_frame_new("Candidate Fields");
    gtk_paned_pack1(GTK_PANED(cand_paned), cand_frame, TRUE, TRUE);
    s_ba.candidate_store = gtk_list_store_new(CAND_COL_NUM,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_UINT);
    s_ba.candidate_view = make_tree(s_ba.candidate_store);
    add_text_column(s_ba.candidate_view, "#", CAND_COL_RANK, 36);
    add_text_column(s_ba.candidate_view, "Start", CAND_COL_START, 52);
    add_text_column(s_ba.candidate_view, "DBC", CAND_COL_DBC_START, 52);
    add_text_column(s_ba.candidate_view, "Len", CAND_COL_LEN, 42);
    add_text_column(s_ba.candidate_view, "Order", CAND_COL_ORDER, 70);
    add_text_column(s_ba.candidate_view, "Type", CAND_COL_SIGNED, 70);
    add_text_column(s_ba.candidate_view, "r", CAND_COL_PEARSON, 54);
    add_text_column(s_ba.candidate_view, "rho", CAND_COL_SPEARMAN, 54);
    add_text_column(s_ba.candidate_view, "R2", CAND_COL_R2, 54);
    add_text_column(s_ba.candidate_view, "Factor", CAND_COL_FACTOR, 70);
    add_text_column(s_ba.candidate_view, "Offset", CAND_COL_OFFSET, 70);
    add_text_column(s_ba.candidate_view, "MAE", CAND_COL_MAE, 54);
    add_text_column(s_ba.candidate_view, "ValN", CAND_COL_VAL_N, 48);
    add_text_column(s_ba.candidate_view, "ValMAE", CAND_COL_VAL_MAE, 60);
    add_text_column(s_ba.candidate_view, "Lag", CAND_COL_LAG, 58);
    add_text_column(s_ba.candidate_view, "Trans", CAND_COL_TRANSITIONS, 58);
    add_text_column(s_ba.candidate_view, "Counter", CAND_COL_COUNTER, 64);
    add_text_column(s_ba.candidate_view, "Time", CAND_COL_TIMESTAMP, 54);
    add_text_column(s_ba.candidate_view, "TimeRes", CAND_COL_TS_RES, 64);
    add_text_column(s_ba.candidate_view, "Checksum", CAND_COL_CHECKSUM, 74);
    add_text_column(s_ba.candidate_view, "Mux", CAND_COL_MUX, 54);
    add_text_column(s_ba.candidate_view, "MuxVals", CAND_COL_MUX_VALUES, 62);
    add_text_column(s_ba.candidate_view, "MuxMin", CAND_COL_MUX_FRAMES, 62);
    add_text_column(s_ba.candidate_view, "MuxBits", CAND_COL_MUX_BITS, 62);
    add_text_column(s_ba.candidate_view, "MuxRanges", CAND_COL_MUX_RANGES, 90);
    add_text_column(s_ba.candidate_view, "Score", CAND_COL_SCORE, 58);
    add_text_column(s_ba.candidate_view, "Confidence", CAND_COL_CONF, 90);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(s_ba.candidate_view));
    g_signal_connect(sel, "changed",
                     G_CALLBACK(on_candidate_selection_changed), NULL);
    GtkWidget *cand_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(cand_scroll), s_ba.candidate_view);
    gtk_container_add(GTK_CONTAINER(cand_frame), cand_scroll);

    GtkWidget *insp_frame = gtk_frame_new("Candidate Inspector");
    gtk_widget_set_size_request(insp_frame, -1, GUI_INSPECTOR_MIN_H);
    gtk_paned_pack2(GTK_PANED(cand_paned), insp_frame, FALSE, TRUE);
    s_ba.inspector_label = gtk_label_new("No candidate selected.");
    gtk_label_set_xalign(GTK_LABEL(s_ba.inspector_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(s_ba.inspector_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(s_ba.inspector_label), TRUE);
    /* A selected candidate produces many wrapped lines; scroll them instead
     * of letting the label's natural height drive the page. */
    GtkWidget *insp_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(insp_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(insp_scroll), s_ba.inspector_label);
    gtk_container_add(GTK_CONTAINER(insp_frame), insp_scroll);

    s_ba.status_label = gtk_label_new(
        "Passive analysis mode. Use active transmission only on an isolated test bench.");
    gtk_label_set_xalign(GTK_LABEL(s_ba.status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(outer), s_ba.status_label, FALSE, FALSE, 0);

    refresh_summary();
    refresh_bit_stats_label();
    ensure_refresh_timer();
    return outer;
}
