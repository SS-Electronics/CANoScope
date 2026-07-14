/**
 * @file math_view.c
 * @brief Realtime two-signal math analysis tab.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/dbc.h"

#define MATH_MAX_SAMPLES 4096

typedef struct {
    char     label[2 * DBC_NAME_MAX + 2];
    char     unit[DBC_UNIT_MAX];
    uint32_t id;
    uint8_t  ext;
    int      sig_idx;
} math_signal_t;

typedef struct {
    double t;
    double x;
    double y;
} math_pair_t;

typedef struct {
    int    n;
    double min_x, max_x, min_y, max_y;
    double mean_x, mean_y;
    double std_x, std_y;
    double cov;
    double pearson;
    double spearman;
    double slope;
    double intercept;
    double r2;
    double mae;
    double rmse;
    int    best_lag;
    double best_lag_corr;
} math_stats_t;

static struct {
    math_signal_t *signals;
    int            signal_count;

    GtkWidget *combo_x;
    GtkWidget *combo_y;
    GtkWidget *time_area;
    GtkWidget *scatter_area;
    GtkWidget *stats_label;
    GtkWidget *window_spin;

    math_pair_t samples[MATH_MAX_SAMPLES];
    int         head;
    int         count;
    gint64      start_us;

    int      x_idx;
    int      y_idx;
    double   last_x;
    double   last_y;
    gboolean has_x;
    gboolean has_y;
} s_math;

typedef struct {
    double v;
    int    idx;
} rank_item_t;

static int sample_index(int logical)
{
    return (s_math.head - s_math.count + logical + MATH_MAX_SAMPLES) %
           MATH_MAX_SAMPLES;
}

static void math_clear_samples(void)
{
    s_math.head = 0;
    s_math.count = 0;
    s_math.start_us = 0;
    s_math.has_x = FALSE;
    s_math.has_y = FALSE;
    if (s_math.time_area)
        gtk_widget_queue_draw(s_math.time_area);
    if (s_math.scatter_area)
        gtk_widget_queue_draw(s_math.scatter_area);
    if (s_math.stats_label)
        gtk_label_set_text(GTK_LABEL(s_math.stats_label), "Samples: 0");
}

static int active_index(GtkWidget *combo)
{
    if (!combo)
        return -1;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    return (idx >= 0 && idx < s_math.signal_count) ? idx : -1;
}

static void update_selected_indices(void)
{
    s_math.x_idx = active_index(s_math.combo_x);
    s_math.y_idx = active_index(s_math.combo_y);
}

static void append_pair(double x, double y)
{
    gint64 now_us = g_get_monotonic_time();
    if (s_math.start_us == 0)
        s_math.start_us = now_us;

    int idx = s_math.head;
    s_math.samples[idx].t = (double)(now_us - s_math.start_us) / 1000000.0;
    s_math.samples[idx].x = x;
    s_math.samples[idx].y = y;
    s_math.head = (s_math.head + 1) % MATH_MAX_SAMPLES;
    if (s_math.count < MATH_MAX_SAMPLES)
        s_math.count++;
}

static double corr_for_lag(const double *x, const double *y, int n, int lag)
{
    int start_x = lag > 0 ? 0 : -lag;
    int start_y = lag > 0 ? lag : 0;
    int m = n - abs(lag);
    if (m < 3)
        return 0.0;

    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < m; i++) {
        sx += x[start_x + i];
        sy += y[start_y + i];
    }
    double mx = sx / m;
    double my = sy / m;

    double cov = 0.0, vx = 0.0, vy = 0.0;
    for (int i = 0; i < m; i++) {
        double dx = x[start_x + i] - mx;
        double dy = y[start_y + i] - my;
        cov += dx * dy;
        vx += dx * dx;
        vy += dy * dy;
    }
    if (vx <= 0.0 || vy <= 0.0)
        return 0.0;
    return cov / sqrt(vx * vy);
}

static int cmp_rank_item(const void *a, const void *b)
{
    const rank_item_t *ra = a;
    const rank_item_t *rb = b;
    return (ra->v > rb->v) - (ra->v < rb->v);
}

static void compute_ranks(const double *v, int n, double *rank)
{
    rank_item_t *items = g_new(rank_item_t, n);
    for (int i = 0; i < n; i++) {
        items[i].v = v[i];
        items[i].idx = i;
    }
    qsort(items, n, sizeof(*items), cmp_rank_item);

    int i = 0;
    while (i < n) {
        int j = i + 1;
        while (j < n && items[j].v == items[i].v)
            j++;
        double r = ((double)i + (double)j + 1.0) / 2.0;
        for (int k = i; k < j; k++)
            rank[items[k].idx] = r;
        i = j;
    }
    g_free(items);
}

static void collect_visible(double **x_out, double **y_out, double *tmin_out,
                            double *tmax_out, int *n_out)
{
    *x_out = NULL;
    *y_out = NULL;
    *n_out = 0;
    *tmin_out = 0.0;
    *tmax_out = 0.0;
    if (s_math.count == 0)
        return;

    int last_idx = sample_index(s_math.count - 1);
    double tmax = s_math.samples[last_idx].t;
    double window = s_math.window_spin ?
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(s_math.window_spin)) : 20.0;
    double tmin = tmax > window ? tmax - window : 0.0;

    int n = 0;
    for (int i = 0; i < s_math.count; i++) {
        math_pair_t *p = &s_math.samples[sample_index(i)];
        if (p->t >= tmin)
            n++;
    }
    if (n == 0)
        return;

    double *x = g_new(double, n);
    double *y = g_new(double, n);
    int w = 0;
    for (int i = 0; i < s_math.count; i++) {
        math_pair_t *p = &s_math.samples[sample_index(i)];
        if (p->t < tmin)
            continue;
        x[w] = p->x;
        y[w] = p->y;
        w++;
    }

    *x_out = x;
    *y_out = y;
    *tmin_out = tmin;
    *tmax_out = tmax;
    *n_out = n;
}

static math_stats_t compute_stats(const double *x, const double *y, int n)
{
    math_stats_t st;
    memset(&st, 0, sizeof(st));
    st.n = n;
    if (n <= 0)
        return st;

    st.min_x = st.max_x = x[0];
    st.min_y = st.max_y = y[0];
    for (int i = 0; i < n; i++) {
        st.mean_x += x[i];
        st.mean_y += y[i];
        if (x[i] < st.min_x) st.min_x = x[i];
        if (x[i] > st.max_x) st.max_x = x[i];
        if (y[i] < st.min_y) st.min_y = y[i];
        if (y[i] > st.max_y) st.max_y = y[i];
    }
    st.mean_x /= n;
    st.mean_y /= n;

    double sx = 0.0, sy = 0.0, cov_sum = 0.0;
    for (int i = 0; i < n; i++) {
        double dx = x[i] - st.mean_x;
        double dy = y[i] - st.mean_y;
        sx += dx * dx;
        sy += dy * dy;
        cov_sum += dx * dy;
    }

    if (n > 1) {
        st.std_x = sqrt(sx / (n - 1));
        st.std_y = sqrt(sy / (n - 1));
        st.cov = cov_sum / (n - 1);
    }
    if (sx > 0.0 && sy > 0.0)
        st.pearson = cov_sum / sqrt(sx * sy);

    if (n > 2) {
        double *rx = g_new(double, n);
        double *ry = g_new(double, n);
        compute_ranks(x, n, rx);
        compute_ranks(y, n, ry);
        st.spearman = corr_for_lag(rx, ry, n, 0);
        g_free(rx);
        g_free(ry);
    }

    if (sx > 0.0) {
        st.slope = cov_sum / sx;
        st.intercept = st.mean_y - st.slope * st.mean_x;

        double sse = 0.0, mae = 0.0;
        for (int i = 0; i < n; i++) {
            double pred = st.intercept + st.slope * x[i];
            double e = y[i] - pred;
            sse += e * e;
            mae += fabs(e);
        }
        st.mae = mae / n;
        st.rmse = sqrt(sse / n);
        st.r2 = sy > 0.0 ? 1.0 - (sse / sy) : 0.0;
    }

    st.best_lag = 0;
    st.best_lag_corr = st.pearson;
    int max_lag = n / 4;
    if (max_lag > 20)
        max_lag = 20;
    for (int lag = -max_lag; lag <= max_lag; lag++) {
        double c = corr_for_lag(x, y, n, lag);
        if (fabs(c) > fabs(st.best_lag_corr)) {
            st.best_lag_corr = c;
            st.best_lag = lag;
        }
    }
    return st;
}

static void update_stats_label(void)
{
    if (!s_math.stats_label)
        return;

    double *x = NULL, *y = NULL, tmin = 0.0, tmax = 0.0;
    int n = 0;
    collect_visible(&x, &y, &tmin, &tmax, &n);
    (void)tmin;
    (void)tmax;
    math_stats_t st = compute_stats(x, y, n);

    char text[1024];
    if (n < 2 || s_math.x_idx == s_math.y_idx) {
        snprintf(text, sizeof(text),
                 "Samples: %d | select two different signals for analysis", n);
    } else {
        snprintf(text, sizeof(text),
                 "Samples %d | X mean %.4g std %.4g min %.4g max %.4g | "
                 "Y mean %.4g std %.4g min %.4g max %.4g\n"
                 "Cov %.4g | Pearson %.4g | Spearman %.4g | "
                 "best lag %d samples (r %.4g)\n"
                 "Regression y = %.4g + %.4g*x | R2 %.4g | "
                 "MAE %.4g | RMSE %.4g",
                 st.n, st.mean_x, st.std_x, st.min_x, st.max_x,
                 st.mean_y, st.std_y, st.min_y, st.max_y,
                 st.cov, st.pearson, st.spearman,
                 st.best_lag, st.best_lag_corr,
                 st.intercept, st.slope, st.r2, st.mae, st.rmse);
    }
    gtk_label_set_text(GTK_LABEL(s_math.stats_label), text);
    g_free(x);
    g_free(y);
}

static void draw_text(cairo_t *cr, const char *text, double x, double y)
{
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

static void draw_axes(cairo_t *cr, double x0, double y0, double x1, double y1)
{
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
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);
}

static gboolean on_time_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
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

    double x0 = 48, y0 = 18, x1 = W - 12, y1 = H - 30;
    if (x1 <= x0 || y1 <= y0)
        return FALSE;
    draw_axes(cr, x0, y0, x1, y1);

    double *x = NULL, *y = NULL, tmin = 0.0, tmax = 0.0;
    int n = 0;
    collect_visible(&x, &y, &tmin, &tmax, &n);
    if (n == 0) {
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        draw_text(cr, "No samples", x0 + 10, y0 + 20);
        return FALSE;
    }

    double minv = x[0], maxv = x[0];
    for (int i = 0; i < n; i++) {
        if (x[i] < minv) minv = x[i];
        if (x[i] > maxv) maxv = x[i];
        if (y[i] < minv) minv = y[i];
        if (y[i] > maxv) maxv = y[i];
    }
    if (maxv <= minv) {
        minv -= 0.5;
        maxv += 0.5;
    }
    double span = maxv - minv;
    if (tmax <= tmin)
        tmax = tmin + 1.0;

    for (int i = 0; i <= 5; i++) {
        double t = tmin + (tmax - tmin) * i / 5.0;
        char label[24];
        snprintf(label, sizeof(label), "%.1fs", t);
        draw_text(cr, label, x0 + (x1 - x0) * i / 5.0 - 10, H - 10);
    }
    for (int i = 0; i <= 4; i++) {
        double v = minv + span * i / 4.0;
        char label[24];
        snprintf(label, sizeof(label), "%.4g", v);
        draw_text(cr, label, 4, y1 - (y1 - y0) * i / 4.0 + 3);
    }

    for (int series = 0; series < 2; series++) {
        cairo_set_source_rgb(cr, series == 0 ? 0.08 : 0.82,
                             series == 0 ? 0.35 : 0.20,
                             series == 0 ? 0.75 : 0.16);
        cairo_set_line_width(cr, 1.6);
        for (int i = 0; i < n; i++) {
            int src = sample_index(s_math.count - n + i);
            double t = s_math.samples[src].t;
            double v = series == 0 ? x[i] : y[i];
            double px = x0 + (t - tmin) / (tmax - tmin) * (x1 - x0);
            double py = y1 - (v - minv) / span * (y1 - y0);
            if (i == 0) cairo_move_to(cr, px, py);
            else        cairo_line_to(cr, px, py);
        }
        cairo_stroke(cr);
    }
    cairo_set_source_rgb(cr, 0.08, 0.35, 0.75);
    draw_text(cr, "X", x0 + 8, y0 + 14);
    cairo_set_source_rgb(cr, 0.82, 0.20, 0.16);
    draw_text(cr, "Y", x0 + 28, y0 + 14);

    g_free(x);
    g_free(y);
    return FALSE;
}

static gboolean on_scatter_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
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

    double x0 = 54, y0 = 18, x1 = W - 12, y1 = H - 30;
    if (x1 <= x0 || y1 <= y0)
        return FALSE;
    draw_axes(cr, x0, y0, x1, y1);

    double *x = NULL, *y = NULL, tmin = 0.0, tmax = 0.0;
    int n = 0;
    collect_visible(&x, &y, &tmin, &tmax, &n);
    (void)tmin;
    (void)tmax;
    if (n == 0) {
        cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
        draw_text(cr, "No samples", x0 + 10, y0 + 20);
        return FALSE;
    }

    math_stats_t st = compute_stats(x, y, n);
    double xmin = st.min_x, xmax = st.max_x;
    double ymin = st.min_y, ymax = st.max_y;
    if (xmax <= xmin) { xmin -= 0.5; xmax += 0.5; }
    if (ymax <= ymin) { ymin -= 0.5; ymax += 0.5; }

    for (int i = 0; i <= 5; i++) {
        double xv = xmin + (xmax - xmin) * i / 5.0;
        char label[24];
        snprintf(label, sizeof(label), "%.4g", xv);
        draw_text(cr, label, x0 + (x1 - x0) * i / 5.0 - 12, H - 10);
    }
    for (int i = 0; i <= 4; i++) {
        double yv = ymin + (ymax - ymin) * i / 4.0;
        char label[24];
        snprintf(label, sizeof(label), "%.4g", yv);
        draw_text(cr, label, 4, y1 - (y1 - y0) * i / 4.0 + 3);
    }

    cairo_set_source_rgba(cr, 0.08, 0.35, 0.75, 0.60);
    for (int i = 0; i < n; i++) {
        double px = x0 + (x[i] - xmin) / (xmax - xmin) * (x1 - x0);
        double py = y1 - (y[i] - ymin) / (ymax - ymin) * (y1 - y0);
        cairo_arc(cr, px, py, 2.0, 0, 2.0 * G_PI);
        cairo_fill(cr);
    }

    if (n >= 2 && isfinite(st.slope)) {
        double y_a = st.intercept + st.slope * xmin;
        double y_b = st.intercept + st.slope * xmax;
        double px_a = x0;
        double py_a = y1 - (y_a - ymin) / (ymax - ymin) * (y1 - y0);
        double px_b = x1;
        double py_b = y1 - (y_b - ymin) / (ymax - ymin) * (y1 - y0);
        cairo_set_source_rgb(cr, 0.82, 0.20, 0.16);
        cairo_set_line_width(cr, 1.8);
        cairo_move_to(cr, px_a, py_a);
        cairo_line_to(cr, px_b, py_b);
        cairo_stroke(cr);
    }

    g_free(x);
    g_free(y);
    return FALSE;
}

static void refresh_all(void)
{
    update_stats_label();
    if (s_math.time_area)
        gtk_widget_queue_draw(s_math.time_area);
    if (s_math.scatter_area)
        gtk_widget_queue_draw(s_math.scatter_area);
}

static void on_signal_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    (void)data;
    update_selected_indices();
    math_clear_samples();
}

static void on_clear_clicked(GtkWidget *button, gpointer data)
{
    (void)button;
    (void)data;
    math_clear_samples();
}

static void on_window_changed(GtkSpinButton *spin, gpointer data)
{
    (void)spin;
    (void)data;
    refresh_all();
}

void gui_math_add_sample(uint32_t id, int is_ext, int sig_idx, double value)
{
    if (!s_math.signals || s_math.x_idx < 0 || s_math.y_idx < 0 ||
        s_math.x_idx == s_math.y_idx)
        return;

    const math_signal_t *xsig = &s_math.signals[s_math.x_idx];
    const math_signal_t *ysig = &s_math.signals[s_math.y_idx];
    gboolean updated = FALSE;

    if (xsig->id == id && xsig->ext == (is_ext ? 1 : 0) &&
        xsig->sig_idx == sig_idx) {
        s_math.last_x = value;
        s_math.has_x = TRUE;
        updated = TRUE;
    }
    if (ysig->id == id && ysig->ext == (is_ext ? 1 : 0) &&
        ysig->sig_idx == sig_idx) {
        s_math.last_y = value;
        s_math.has_y = TRUE;
        updated = TRUE;
    }

    if (updated && s_math.has_x && s_math.has_y) {
        append_pair(s_math.last_x, s_math.last_y);
        refresh_all();
    }
}

void gui_math_set_database(const dbc_db_t *db)
{
    g_free(s_math.signals);
    s_math.signals = NULL;
    s_math.signal_count = 0;

    if (db && db->signal_count > 0) {
        s_math.signals = g_new0(math_signal_t, db->signal_count);
        int n = 0;
        for (size_t mi = 0; mi < db->message_count; mi++) {
            const dbc_message_t *m = &db->messages[mi];
            for (uint16_t si = 0; si < m->signal_count; si++) {
                const dbc_signal_t *s = &m->signals[si];
                math_signal_t *ms = &s_math.signals[n++];
                snprintf(ms->label, sizeof(ms->label), "%s.%s",
                         m->name, s->name);
                snprintf(ms->unit, sizeof(ms->unit), "%s", s->unit);
                ms->id = m->id;
                ms->ext = m->is_extended;
                ms->sig_idx = si;
            }
        }
        s_math.signal_count = n;
    }

    if (s_math.combo_x) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s_math.combo_x));
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s_math.combo_y));
        for (int i = 0; i < s_math.signal_count; i++) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_math.combo_x),
                                           s_math.signals[i].label);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s_math.combo_y),
                                           s_math.signals[i].label);
        }
        if (s_math.signal_count > 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(s_math.combo_x), 0);
        if (s_math.signal_count > 1)
            gtk_combo_box_set_active(GTK_COMBO_BOX(s_math.combo_y), 1);
    }

    update_selected_indices();
    math_clear_samples();
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

GtkWidget *gui_create_math_view(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("X:"), FALSE, FALSE, 0);
    s_math.combo_x = gtk_combo_box_text_new();
    gtk_widget_set_size_request(s_math.combo_x, 240, -1);
    gtk_box_pack_start(GTK_BOX(bar), s_math.combo_x, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Y:"), FALSE, FALSE, 0);
    s_math.combo_y = gtk_combo_box_text_new();
    gtk_widget_set_size_request(s_math.combo_y, 240, -1);
    gtk_box_pack_start(GTK_BOX(bar), s_math.combo_y, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Window (s):"),
                       FALSE, FALSE, 0);
    s_math.window_spin = gtk_spin_button_new_with_range(2, 300, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_math.window_spin), 20);
    gtk_box_pack_start(GTK_BOX(bar), s_math.window_spin, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    gtk_box_pack_start(GTK_BOX(bar), clear_btn, FALSE, FALSE, 0);

    g_signal_connect(s_math.combo_x, "changed",
                     G_CALLBACK(on_signal_changed), NULL);
    g_signal_connect(s_math.combo_y, "changed",
                     G_CALLBACK(on_signal_changed), NULL);
    g_signal_connect(s_math.window_spin, "value-changed",
                     G_CALLBACK(on_window_changed), NULL);
    g_signal_connect(clear_btn, "clicked",
                     G_CALLBACK(on_clear_clicked), NULL);

    s_math.stats_label = gtk_label_new("Samples: 0");
    gtk_label_set_xalign(GTK_LABEL(s_math.stats_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(s_math.stats_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(s_math.stats_label),
                                 PANGO_WRAP_WORD_CHAR);
    gtk_box_pack_start(GTK_BOX(box), s_math.stats_label, FALSE, FALSE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);
    g_signal_connect(paned, "size-allocate",
                     G_CALLBACK(set_paned_ratio_on_allocate),
                     GINT_TO_POINTER(500));
    gtk_box_pack_start(GTK_BOX(box), paned, TRUE, TRUE, 0);

    GtkWidget *time_frame = gtk_frame_new("Signals vs Time");
    s_math.time_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(s_math.time_area, TRUE);
    gtk_widget_set_vexpand(s_math.time_area, TRUE);
    g_signal_connect(s_math.time_area, "draw", G_CALLBACK(on_time_draw), NULL);
    gtk_container_add(GTK_CONTAINER(time_frame), s_math.time_area);
    gtk_paned_pack1(GTK_PANED(paned), time_frame, TRUE, TRUE);

    GtkWidget *scatter_frame = gtk_frame_new("Y vs X");
    s_math.scatter_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(s_math.scatter_area, TRUE);
    gtk_widget_set_vexpand(s_math.scatter_area, TRUE);
    g_signal_connect(s_math.scatter_area, "draw",
                     G_CALLBACK(on_scatter_draw), NULL);
    gtk_container_add(GTK_CONTAINER(scatter_frame), s_math.scatter_area);
    gtk_paned_pack2(GTK_PANED(paned), scatter_frame, TRUE, TRUE);

    gui_math_set_database(NULL);
    return box;
}
