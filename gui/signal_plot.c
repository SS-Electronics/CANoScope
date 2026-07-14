/**
 * @file signal_plot.c
 * @brief Signal Analysis Viewer — detachable multi-window signal-over-time graphs.
 *
 * @details
 * Implements a Vector-CANalyzer "Graphics"-style analysis workspace.  Instead of
 * a single fixed graph, the *Signal Analysis Viewer* tab is a launcher: each
 * **Add Analysis Window** click opens an independent, detached top-level window
 * containing its own oscilloscope-style graph.  Within a window the user adds
 * signals through a **searchable dropdown** (type to filter, pick, *Add*); each
 * selected signal is plotted with its own colour.
 *
 * Decoding feeds a single **central sample store**: every decoded sample
 * (delivered from the Signal Analysis decode path via @ref gui_plot_add_sample)
 * is appended to a per-signal ring buffer, independent of which windows exist.
 * A freshly opened window therefore immediately shows the recent history, and
 * several windows can plot the same signal without duplicating storage.
 *
 * Each window renders with Cairo: signals are normalised to their DBC
 * `[min,max]` range so disparate physical ranges share the vertical space, and
 * every plotted signal draws its **own colour-matched Y axis** on the left with
 * real physical-value tick labels and its engineering unit (rather than a single
 * shared 0-100 % scale).  The graph itself carries **no overlaid legend** —
 * hovering the cursor over the plot snaps to
 * the nearest sample and shows a tooltip with the signal name, its value/unit and
 * the time, while a **side pane** lists the colour swatch, name and live value of
 * every selected signal.  A single redraw timer repaints all open windows at
 * ~30 fps and refreshes their side-pane values.
 *
 * All sample appends and draws happen on the GTK main thread (the decode path
 * runs in a GLib idle callback), so no locking is required.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "../inc/gui.h"
#include "../inc/dbc.h"

#define PLOT_RING       30000   /**< Samples kept per signal (~30 s @ 1 kHz).  */
#define PLOT_REDRAW_MS  33      /**< Redraw period (~30 fps) while windows open.*/
#define PLOT_MAX_SERIES 64      /**< Max signals plotted in one window.        */
#define PLOT_HOVER_PX   18.0    /**< Cursor snap radius for hover tooltip (px).*/

/** @brief Resolve a bundled asset path (defined in main_window.c). */
extern const char *gui_find_asset(const char *name);

/** @brief Distinct trace colours (RGB 0..1), cycled across a window's series. */
static const double PALETTE[][3] = {
    { 0.20, 0.60, 0.86 }, { 0.91, 0.30, 0.24 }, { 0.18, 0.80, 0.44 },
    { 0.95, 0.61, 0.07 }, { 0.61, 0.35, 0.71 }, { 0.10, 0.74, 0.61 },
    { 0.90, 0.49, 0.13 }, { 0.52, 0.58, 0.59 }, { 0.83, 0.33, 0.64 },
    { 0.16, 0.50, 0.73 }, { 0.86, 0.78, 0.13 }, { 0.40, 0.76, 0.65 },
};
#define PALETTE_N ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

/* ------------------------------------------------------------------ */
/* Central sample store                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief One plottable signal with its rolling sample history.
 *
 * Lives in the global store, populated from the active DBC and fed by
 * @ref gui_plot_add_sample regardless of which windows are open.
 */
typedef struct {
    char     label[2 * DBC_NAME_MAX]; /**< "Message.Signal".               */
    char     unit[DBC_UNIT_MAX];      /**< Engineering unit.               */
    uint32_t id;                      /**< Raw CAN identifier.             */
    uint8_t  ext;                     /**< Extended-ID flag.               */
    int      sig_idx;                 /**< Signal index within its message.*/
    double   dmin, dmax;              /**< DBC range (for normalisation).  */

    double   t[PLOT_RING];            /**< Sample times (s since origin).  */
    double   v[PLOT_RING];            /**< Sample values.                  */
    int      head;                    /**< Next write index.               */
    int      count;                   /**< Valid sample count.             */
    double   last;                    /**< Most recent value.              */
    gboolean has_last;                /**< Whether @ref last is valid.     */
} plot_signal_t;

static struct {
    plot_signal_t *sig;        /**< Per-signal store (one per DBC signal). */
    int            n;          /**< Number of stored signals.             */
    gint64         start_us;   /**< Monotonic origin for sample times.     */
} s_store;

/** @brief List of open @ref plot_window_t analysis windows. */
static GList *s_windows;

/** @brief Shared ~30 fps redraw timer (runs while any window is open). */
static guint s_timer;

/** @brief Tab-level label reporting the number of open analysis windows. */
static GtkWidget *s_count_lbl;

/** @brief Seconds elapsed since the store's monotonic origin. */
static double plot_now(void)
{
    return (double)(g_get_monotonic_time() - s_store.start_us) / 1e6;
}

/* ------------------------------------------------------------------ */
/* Per-window model                                                     */
/* ------------------------------------------------------------------ */

/** @brief One signal selected inside a window (references the store). */
typedef struct {
    int        sig;        /**< Index into the central signal store.       */
    double     col[3];     /**< Trace colour for this window.             */
    GtkWidget *row;        /**< Side-pane legend row (container).         */
    GtkWidget *val_label;  /**< Side-pane live-value label.               */
} win_series_t;

/** @brief One detached analysis window with its own graph + side legend. */
typedef struct {
    GtkWidget   *win;        /**< Detached top-level window.              */
    GtkWidget   *area;       /**< Cairo drawing area.                     */
    GtkWidget   *legend_box; /**< Side-pane container for legend rows.    */
    GtkWidget   *combo;      /**< Searchable signal dropdown (with entry).*/
    int          idx;        /**< Window number (for the title).          */

    win_series_t series[PLOT_MAX_SERIES];
    int          n_series;
    int          color_next; /**< Next palette slot to assign.            */

    double       window_sec; /**< Visible time-window width (seconds).    */
    gboolean     paused;     /**< Freeze the view when TRUE.              */
    double       frozen_now; /**< "now" captured at pause time.           */

    gboolean     hover_on;   /**< Cursor is inside the plot.              */
    double       hover_x;    /**< Cursor X within the drawing area.       */
    double       hover_y;    /**< Cursor Y within the drawing area.       */
} plot_window_t;

/** @brief The effective "now" for a window (frozen while paused). */
static double win_now(const plot_window_t *pw)
{
    return pw->paused ? pw->frozen_now : plot_now();
}

/* ------------------------------------------------------------------ */
/* Store management                                                     */
/* ------------------------------------------------------------------ */

/** @brief Locate a stored signal by message/signal identity, or -1. */
static int store_find(uint32_t id, int ext, int sig_idx)
{
    for (int i = 0; i < s_store.n; i++) {
        const plot_signal_t *ps = &s_store.sig[i];
        if (ps->id == id && ps->ext == (ext ? 1 : 0) && ps->sig_idx == sig_idx)
            return i;
    }
    return -1;
}

void gui_plot_add_sample(uint32_t id, int is_ext, int sig_idx, double value)
{
    if (!s_store.sig)
        return;
    int i = store_find(id, is_ext, sig_idx);
    if (i < 0)
        return;

    plot_signal_t *ps = &s_store.sig[i];
    ps->t[ps->head] = plot_now();
    ps->v[ps->head] = value;
    ps->head = (ps->head + 1) % PLOT_RING;
    if (ps->count < PLOT_RING)
        ps->count++;
    ps->last     = value;
    ps->has_last = TRUE;
}

/* ------------------------------------------------------------------ */
/* Window side-pane (legend) helpers                                    */
/* ------------------------------------------------------------------ */

/** @brief Forward declaration: rebuild a window's dropdown from the store. */
static void win_populate_combo(plot_window_t *pw);

/** @brief Remove series @p s from window @p pw and drop its legend row. */
static void win_remove_series(plot_window_t *pw, int s)
{
    if (s < 0 || s >= pw->n_series)
        return;
    if (pw->series[s].row)
        gtk_widget_destroy(pw->series[s].row);
    for (int k = s; k < pw->n_series - 1; k++)
        pw->series[k] = pw->series[k + 1];
    pw->n_series--;
    if (pw->area)
        gtk_widget_queue_draw(pw->area);
}

/** @brief "×" remove-button handler for a side-pane legend row. */
static void on_legend_remove(GtkWidget *btn, gpointer data)
{
    (void)btn;
    plot_window_t *pw = data;
    int store_idx = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(btn), "store-idx"));
    for (int s = 0; s < pw->n_series; s++) {
        if (pw->series[s].sig == store_idx) {
            win_remove_series(pw, s);
            return;
        }
    }
}

/** @brief Add stored signal @p store_idx to window @p pw (idempotent). */
static void win_add_series(plot_window_t *pw, int store_idx)
{
    if (store_idx < 0 || store_idx >= s_store.n)
        return;
    if (pw->n_series >= PLOT_MAX_SERIES)
        return;
    for (int s = 0; s < pw->n_series; s++)
        if (pw->series[s].sig == store_idx)
            return; /* already shown */

    win_series_t *ws = &pw->series[pw->n_series];
    ws->sig    = store_idx;
    ws->col[0] = PALETTE[pw->color_next % PALETTE_N][0];
    ws->col[1] = PALETTE[pw->color_next % PALETTE_N][1];
    ws->col[2] = PALETTE[pw->color_next % PALETTE_N][2];
    pw->color_next++;

    const plot_signal_t *sig = &s_store.sig[store_idx];

    /* Side-pane legend row: [■ name] ............ [value] [×] */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    char namemk[2 * DBC_NAME_MAX + 96];
    snprintf(namemk, sizeof(namemk),
             "<span foreground=\"#%02x%02x%02x\">\xe2\x96\x88</span> %s",
             (int)(ws->col[0] * 255), (int)(ws->col[1] * 255),
             (int)(ws->col[2] * 255), sig->label);
    GtkWidget *name = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(name), namemk);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);

    GtkWidget *val = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(val), 1.0f);
    gtk_widget_set_size_request(val, 96, -1);
    gtk_box_pack_start(GTK_BOX(row), val, FALSE, FALSE, 0);

    GtkWidget *rm = gtk_button_new_with_label("\xc3\x97"); /* × */
    gtk_widget_set_tooltip_text(rm, "Remove this signal");
    g_object_set_data(G_OBJECT(rm), "store-idx", GINT_TO_POINTER(store_idx));
    g_signal_connect(rm, "clicked", G_CALLBACK(on_legend_remove), pw);
    gtk_box_pack_start(GTK_BOX(row), rm, FALSE, FALSE, 0);

    ws->row       = row;
    ws->val_label = val;
    pw->n_series++;

    gtk_box_pack_start(GTK_BOX(pw->legend_box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(row);
    if (pw->area)
        gtk_widget_queue_draw(pw->area);
}

/** @brief Update every side-pane row's live value for window @p pw. */
static void win_refresh_legend(plot_window_t *pw)
{
    for (int s = 0; s < pw->n_series; s++) {
        const plot_signal_t *sig = &s_store.sig[pw->series[s].sig];
        char txt[64];
        if (sig->has_last)
            snprintf(txt, sizeof(txt), "%.4g %s", sig->last, sig->unit);
        else
            snprintf(txt, sizeof(txt), "—");
        gtk_label_set_text(GTK_LABEL(pw->series[s].val_label), txt);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

/** @brief Map a normalised value (0..1) to a Y pixel within the plot rect. */
static double norm_to_y(double norm, double y0, double y1)
{
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    return y1 - norm * (y1 - y0);
}

/** @brief Compute a series' normalisation base/span (auto-scale fallback). */
static void series_scale(const plot_signal_t *sig, double tmin,
                         double *base, double *span)
{
    *base = sig->dmin;
    *span = sig->dmax - sig->dmin;
    if (*span > 0.0)
        return;
    /* Invalid/empty DBC range — auto-scale from the visible samples. */
    double mn = 1e300, mx = -1e300;
    for (int k = 0; k < sig->count; k++) {
        int idx = (sig->head - sig->count + k + PLOT_RING) % PLOT_RING;
        if (sig->t[idx] < tmin) continue;
        if (sig->v[idx] < mn) mn = sig->v[idx];
        if (sig->v[idx] > mx) mx = sig->v[idx];
    }
    if (mx > mn)      { *base = mn; *span = mx - mn; }
    else if (mx == mn) { *base = mn - 0.5; *span = 1.0; } /* flat line */
    else              { *base = 0.0; *span = 1.0; }       /* no samples yet */
}

/**
 * @brief Cairo draw handler for a window's graph (no legend; hover tooltip).
 */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    plot_window_t *pw = data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    const double W = a.width, H = a.height;

    /* Background. */
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_fill(cr);

    const double right = 12, top = 18, bottom = 24;

    /* One Y axis per signal, stacked on the left; widen the margin to fit. */
    const double axis_w = 56.0;
    int n_axes   = pw->n_series;
    int max_axes = (int)((W * 0.5) / axis_w);
    if (max_axes < 1) max_axes = 1;
    if (n_axes > max_axes) n_axes = max_axes;
    const double left = pw->n_series > 0 ? 6.0 + n_axes * axis_w : 50.0;

    const double x0 = left, x1 = W - right, y0 = top, y1 = H - bottom;
    if (x1 <= x0 + 10 || y1 <= y0 + 10)
        return FALSE;

    const double now  = win_now(pw);
    const double winw = pw->window_sec;
    const double tmin = now - winw;

    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_line_width(cr, 1.0);

    /* Per-series normalisation (shared by the Y axes and the traces below). */
    double s_base[PLOT_MAX_SERIES], s_span[PLOT_MAX_SERIES];
    for (int s = 0; s < pw->n_series; s++)
        series_scale(&s_store.sig[pw->series[s].sig],
                     tmin, &s_base[s], &s_span[s]);

    /* Horizontal grid (no shared % labels — each signal carries its own axis). */
    for (int i = 0; i <= 10; i++) {
        double yy = y0 + (y1 - y0) * i / 10.0;
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
        cairo_move_to(cr, x0, yy);
        cairo_line_to(cr, x1, yy);
        cairo_stroke(cr);
    }

    /* Coloured per-signal Y axes with real physical-value tick labels. */
    cairo_set_font_size(cr, 9);
    for (int c = 0; c < n_axes; c++) {
        const win_series_t  *ws  = &pw->series[c];
        const plot_signal_t *sig = &s_store.sig[ws->sig];
        double ax = 2.0 + (double)(c + 1) * axis_w - 8.0; /* axis line x */

        cairo_set_source_rgb(cr, ws->col[0], ws->col[1], ws->col[2]);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, ax, y0);
        cairo_line_to(cr, ax, y1);
        cairo_stroke(cr);

        for (int i = 0; i <= 4; i++) {
            double frac = i / 4.0;
            double yy   = y1 - frac * (y1 - y0);
            double val  = s_base[c] + frac * s_span[c];
            cairo_move_to(cr, ax - 3, yy);
            cairo_line_to(cr, ax, yy);
            cairo_stroke(cr);

            char lbl[24];
            snprintf(lbl, sizeof(lbl), "%.4g", val);
            cairo_text_extents_t te;
            cairo_text_extents(cr, lbl, &te);
            double lx = ax - 5 - te.width;
            if (lx < 1) lx = 1;
            double ly = yy + 3;
            if (i == 0)      ly = yy - 1;  /* bottom label: lift inside */
            else if (i == 4) ly = yy + 8;  /* top label: drop inside    */
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, lbl);
        }

        /* Unit header above the axis (in the signal's colour). */
        if (sig->unit[0]) {
            cairo_text_extents_t ue;
            cairo_text_extents(cr, sig->unit, &ue);
            double ux = ax - ue.width;
            if (ux < 1) ux = 1;
            cairo_move_to(cr, ux, y0 - 6);
            cairo_show_text(cr, sig->unit);
        }
    }
    cairo_set_font_size(cr, 10);

    /* Vertical grid + X labels (relative seconds). */
    int xdiv = 10;
    for (int i = 0; i <= xdiv; i++) {
        double xx = x0 + (x1 - x0) * i / (double)xdiv;
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
        cairo_move_to(cr, xx, y0);
        cairo_line_to(cr, xx, y1);
        cairo_stroke(cr);
        char lbl[16];
        double rel = -winw + winw * i / (double)xdiv;
        snprintf(lbl, sizeof(lbl), "%.0fs", rel);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.65);
        cairo_move_to(cr, xx - 8, y1 + 16);
        cairo_show_text(cr, lbl);
    }

    /* Plot frame. */
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.45);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);

    if (pw->n_series == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.75);
        cairo_set_font_size(cr, 13);
        cairo_move_to(cr, x0 + 14, y0 + 24);
        cairo_show_text(cr,
            "Add signals from the dropdown above to plot them over time.");
        return FALSE;
    }

    /* Hover hit-test (nearest visible sample across all series). */
    gboolean have_hit = FALSE;
    int    hit_series = -1;
    double hit_px = 0, hit_py = 0, hit_t = 0, hit_v = 0, hit_best = 1e300;
    const gboolean want_hover =
        pw->hover_on &&
        pw->hover_x >= x0 && pw->hover_x <= x1 &&
        pw->hover_y >= y0 && pw->hover_y <= y1;

    /* Traces. */
    for (int s = 0; s < pw->n_series; s++) {
        win_series_t        *ws  = &pw->series[s];
        const plot_signal_t *sig = &s_store.sig[ws->sig];
        if (sig->count == 0)
            continue;

        double base = s_base[s], span = s_span[s];

        /* Find first visible sample + count. */
        int visible = 0, first = -1;
        for (int k = 0; k < sig->count; k++) {
            int idx = (sig->head - sig->count + k + PLOT_RING) % PLOT_RING;
            if (sig->t[idx] >= tmin) { if (first < 0) first = k; visible++; }
        }
        if (visible == 0)
            continue;

        /* Stride-decimate to ~2 segments per horizontal pixel. */
        int max_pts = (int)((x1 - x0) * 2.0);
        int stride  = visible > max_pts ? visible / max_pts : 1;

        cairo_set_source_rgb(cr, ws->col[0], ws->col[1], ws->col[2]);
        cairo_set_line_width(cr, 1.5);
        gboolean started = FALSE;
        for (int k = first; k < sig->count; k += stride) {
            int idx = (sig->head - sig->count + k + PLOT_RING) % PLOT_RING;
            double tt = sig->t[idx];
            if (tt < tmin) continue;
            double x = x0 + (tt - tmin) / winw * (x1 - x0);
            double y = norm_to_y((sig->v[idx] - base) / span, y0, y1);
            if (!started) { cairo_move_to(cr, x, y); started = TRUE; }
            else            cairo_line_to(cr, x, y);

            if (want_hover) {
                double dx = x - pw->hover_x, dy = y - pw->hover_y;
                double d2 = dx * dx + dy * dy;
                if (d2 < hit_best) {
                    hit_best   = d2;
                    hit_series = s;
                    hit_px = x; hit_py = y;
                    hit_t  = tt; hit_v = sig->v[idx];
                    have_hit = TRUE;
                }
            }
        }
        cairo_stroke(cr);
    }

    /* Hover crosshair + tooltip. */
    if (have_hit && hit_best <= PLOT_HOVER_PX * PLOT_HOVER_PX) {
        win_series_t *ws = &pw->series[hit_series];
        const plot_signal_t *sig = &s_store.sig[ws->sig];

        /* Crosshair. */
        double dash[] = { 3.0, 3.0 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgba(cr, 0.8, 0.8, 0.85, 0.6);
        cairo_move_to(cr, hit_px, y0); cairo_line_to(cr, hit_px, y1);
        cairo_move_to(cr, x0, hit_py); cairo_line_to(cr, x1, hit_py);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);

        /* Marker dot in the series colour. */
        cairo_set_source_rgb(cr, ws->col[0], ws->col[1], ws->col[2]);
        cairo_arc(cr, hit_px, hit_py, 3.5, 0, 2 * G_PI);
        cairo_fill(cr);

        /* Tooltip text lines. */
        char l1[2 * DBC_NAME_MAX + 16];
        char l2[64];
        char l3[32];
        snprintf(l1, sizeof(l1), "%s", sig->label);
        snprintf(l2, sizeof(l2), "%.5g %s", hit_v, sig->unit);
        snprintf(l3, sizeof(l3), "t = %.2fs", hit_t - now);

        cairo_set_font_size(cr, 11);
        cairo_text_extents_t e1, e2, e3;
        cairo_text_extents(cr, l1, &e1);
        cairo_text_extents(cr, l2, &e2);
        cairo_text_extents(cr, l3, &e3);
        double tw = e1.width;
        if (e2.width > tw) tw = e2.width;
        if (e3.width > tw) tw = e3.width;
        double pad = 6, lh = 15;
        double bw = tw + 2 * pad, bh = 3 * lh + pad;
        double bx = hit_px + 12, by = hit_py + 12;
        if (bx + bw > x1) bx = hit_px - bw - 12;
        if (by + bh > y1) by = hit_py - bh - 12;
        if (bx < x0) bx = x0 + 2;
        if (by < y0) by = y0 + 2;

        cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 0.92);
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, ws->col[0], ws->col[1], ws->col[2]);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, ws->col[0], ws->col[1], ws->col[2]);
        cairo_move_to(cr, bx + pad, by + pad + 9);
        cairo_show_text(cr, l1);
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.95);
        cairo_move_to(cr, bx + pad, by + pad + 9 + lh);
        cairo_show_text(cr, l2);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.75);
        cairo_move_to(cr, bx + pad, by + pad + 9 + 2 * lh);
        cairo_show_text(cr, l3);
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Window event handlers                                                */
/* ------------------------------------------------------------------ */

/** @brief Track the cursor for hover tooltips. */
static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    plot_window_t *pw = data;
    pw->hover_on = TRUE;
    pw->hover_x  = e->x;
    pw->hover_y  = e->y;
    gtk_widget_queue_draw(w);
    return FALSE;
}

/** @brief Drop the hover tooltip when the cursor leaves the plot. */
static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer data)
{
    (void)e;
    plot_window_t *pw = data;
    pw->hover_on = FALSE;
    gtk_widget_queue_draw(w);
    return FALSE;
}

/** @brief "Add" — add the dropdown's current signal to the window. */
static void on_add_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    plot_window_t *pw = data;
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(pw->combo));
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!text || !*text)
        return;
    for (int i = 0; i < s_store.n; i++) {
        if (g_strcmp0(s_store.sig[i].label, text) == 0) {
            win_add_series(pw, i);
            gtk_entry_set_text(GTK_ENTRY(entry), "");
            return;
        }
    }
    gui_status_message("No signal named \"%s\" in the database.", text);
}

/** @brief Enter in the dropdown entry behaves like clicking "Add". */
static void on_combo_activate(GtkEntry *entry, gpointer data)
{
    (void)entry;
    on_add_clicked(NULL, data);
}

/** @brief Per-window pause toggle. */
static void on_pause(GtkToggleButton *btn, gpointer data)
{
    plot_window_t *pw = data;
    pw->paused = gtk_toggle_button_get_active(btn);
    if (pw->paused)
        pw->frozen_now = plot_now();
    if (pw->area)
        gtk_widget_queue_draw(pw->area);
}

/** @brief Per-window time-window width spin. */
static void on_window_changed(GtkSpinButton *btn, gpointer data)
{
    plot_window_t *pw = data;
    pw->window_sec = gtk_spin_button_get_value(btn);
    if (pw->area)
        gtk_widget_queue_draw(pw->area);
}

/** @brief "Clear" — remove every signal from this window. */
static void on_clear_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    plot_window_t *pw = data;
    while (pw->n_series > 0)
        win_remove_series(pw, pw->n_series - 1);
    pw->color_next = 0;
}

/** @brief Refresh the tab-level "N analysis windows open" label. */
static void update_count_label(void)
{
    if (!s_count_lbl)
        return;
    int n = g_list_length(s_windows);
    char txt[64];
    snprintf(txt, sizeof(txt),
             "%d analysis window%s open", n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(s_count_lbl), txt);
}

/** @brief A window was closed — unlink it from the global list and free. */
static void on_window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    plot_window_t *pw = data;
    s_windows = g_list_remove(s_windows, pw);
    g_free(pw);
    update_count_label();
    if (!s_windows && s_timer) {
        g_source_remove(s_timer);
        s_timer = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Dropdown population + DBC changes                                    */
/* ------------------------------------------------------------------ */

/** @brief (Re)build a window's dropdown list from the current store. */
static void win_populate_combo(plot_window_t *pw)
{
    GtkComboBox *combo = GTK_COMBO_BOX(pw->combo);
    GtkListStore *ls = GTK_LIST_STORE(gtk_combo_box_get_model(combo));
    gtk_list_store_clear(ls);
    for (int i = 0; i < s_store.n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(ls, &it);
        gtk_list_store_set(ls, &it, 0, s_store.sig[i].label, -1);
    }
}

void gui_plot_set_database(const dbc_db_t *db)
{
    /* Rebuild the central store. */
    g_free(s_store.sig);
    s_store.sig = NULL;
    s_store.n   = 0;

    if (db && db->signal_count > 0) {
        s_store.sig = g_malloc0(sizeof(plot_signal_t) * db->signal_count);
        int n = 0;
        for (size_t mi = 0; mi < db->message_count; mi++) {
            const dbc_message_t *m = &db->messages[mi];
            for (uint16_t si = 0; si < m->signal_count; si++) {
                const dbc_signal_t *s = &m->signals[si];
                plot_signal_t *ps = &s_store.sig[n];
                snprintf(ps->label, sizeof(ps->label), "%s.%s",
                         m->name, s->name);
                snprintf(ps->unit, sizeof(ps->unit), "%s", s->unit);
                ps->id      = m->id;
                ps->ext     = m->is_extended;
                ps->sig_idx = si;
                ps->dmin    = s->min;
                ps->dmax    = s->max;
                n++;
            }
        }
        s_store.n = n;
    }

    /* The signal set changed: clear every window's selection and refresh its
     * dropdown so it cannot reference stale store indices. */
    for (GList *l = s_windows; l; l = l->next) {
        plot_window_t *pw = l->data;
        while (pw->n_series > 0)
            win_remove_series(pw, pw->n_series - 1);
        pw->color_next = 0;
        win_populate_combo(pw);
        if (pw->area)
            gtk_widget_queue_draw(pw->area);
    }
}

/* ------------------------------------------------------------------ */
/* Shared redraw timer                                                  */
/* ------------------------------------------------------------------ */

/** @brief Repaint every open window and refresh its side-pane values. */
static gboolean plot_tick(gpointer data)
{
    (void)data;
    for (GList *l = s_windows; l; l = l->next) {
        plot_window_t *pw = l->data;
        win_refresh_legend(pw);
        if (pw->area && gtk_widget_get_mapped(pw->area))
            gtk_widget_queue_draw(pw->area);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Analysis-window construction                                         */
/* ------------------------------------------------------------------ */

/** @brief Build and show one detached analysis window. */
static void create_analysis_window(void)
{
    static int counter = 0;
    plot_window_t *pw = g_malloc0(sizeof(plot_window_t));
    pw->idx        = ++counter;
    pw->window_sec = 20.0;

    pw->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[48];
    snprintf(title, sizeof(title), "Analysis Window %d", pw->idx);
    gtk_window_set_title(GTK_WINDOW(pw->win), title);
    gtk_window_set_default_size(GTK_WINDOW(pw->win), 820, 460);
    if (g_gui.window)
        gtk_window_set_transient_for(GTK_WINDOW(pw->win),
                                     GTK_WINDOW(g_gui.window));
    /* Detached, not always-on-top: independent, movable alongside the main. */
    gtk_window_set_destroy_with_parent(GTK_WINDOW(pw->win), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(pw->win), vbox);

    /* --- Toolbar: searchable signal dropdown + Add + controls --- */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Signal:"),
                       FALSE, FALSE, 0);

    GtkListStore *ls = gtk_list_store_new(1, G_TYPE_STRING);
    pw->combo = gtk_combo_box_new_with_model_and_entry(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    gtk_combo_box_set_entry_text_column(GTK_COMBO_BOX(pw->combo), 0);
    gtk_widget_set_size_request(pw->combo, 260, -1);

    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(pw->combo));
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "type to search…");
    /* Search-as-you-type completion over the same model. */
    GtkEntryCompletion *comp = gtk_entry_completion_new();
    gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(ls));
    gtk_entry_completion_set_text_column(comp, 0);
    gtk_entry_completion_set_inline_completion(comp, TRUE);
    gtk_entry_completion_set_popup_completion(comp, TRUE);
    gtk_entry_set_completion(GTK_ENTRY(entry), comp);
    g_object_unref(comp);
    g_signal_connect(entry, "activate", G_CALLBACK(on_combo_activate), pw);
    gtk_box_pack_start(GTK_BOX(bar), pw->combo, FALSE, FALSE, 0);

    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), pw);
    gtk_box_pack_start(GTK_BOX(bar), add_btn, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_clicked), pw);
    gtk_box_pack_start(GTK_BOX(bar), clear_btn, FALSE, FALSE, 0);

    GtkWidget *pause_btn = gtk_toggle_button_new_with_label("Pause");
    g_signal_connect(pause_btn, "toggled", G_CALLBACK(on_pause), pw);
    gtk_box_pack_start(GTK_BOX(bar), pause_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Window (s):"),
                       FALSE, FALSE, 4);
    GtkWidget *win_spin = gtk_spin_button_new_with_range(2, 120, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win_spin), pw->window_sec);
    g_signal_connect(win_spin, "value-changed",
                     G_CALLBACK(on_window_changed), pw);
    gtk_box_pack_start(GTK_BOX(bar), win_spin, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* --- Paned: graph | side legend --- */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    pw->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(pw->area, 400, 200);
    gtk_widget_add_events(pw->area,
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(pw->area, "draw", G_CALLBACK(on_draw), pw);
    g_signal_connect(pw->area, "motion-notify-event",
                     G_CALLBACK(on_motion), pw);
    g_signal_connect(pw->area, "leave-notify-event",
                     G_CALLBACK(on_leave), pw);
    gtk_paned_pack1(GTK_PANED(paned), pw->area, TRUE, TRUE);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(side, 6);
    gtk_widget_set_margin_end(side, 6);
    gtk_widget_set_margin_top(side, 6);
    GtkWidget *side_hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(side_hdr), "<b>Signals</b>");
    gtk_label_set_xalign(GTK_LABEL(side_hdr), 0.0f);
    gtk_box_pack_start(GTK_BOX(side), side_hdr, FALSE, FALSE, 0);

    pw->legend_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *side_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(side_scroll), pw->legend_box);
    gtk_box_pack_start(GTK_BOX(side), side_scroll, TRUE, TRUE, 0);

    GtkWidget *side_frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(side_frame), side);
    gtk_widget_set_size_request(side_frame, 240, -1);
    gtk_paned_pack2(GTK_PANED(paned), side_frame, FALSE, TRUE);

    g_signal_connect(pw->win, "destroy",
                     G_CALLBACK(on_window_destroy), pw);

    s_windows = g_list_append(s_windows, pw);
    win_populate_combo(pw);

    if (!s_timer)
        s_timer = g_timeout_add(PLOT_REDRAW_MS, plot_tick, NULL);

    gtk_widget_show_all(pw->win);
    update_count_label();
}

/* ------------------------------------------------------------------ */
/* Tab (launcher) construction                                          */
/* ------------------------------------------------------------------ */

/** @brief "Add Analysis Window" — open a new detached graph window. */
static void on_add_window(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    create_analysis_window();
}

/** @brief "Load DBC…" — choose a database after launch (delegates). */
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

/** @brief Free the redraw timer if the tab is destroyed (app shutdown). */
static void on_tab_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (s_timer) {
        g_source_remove(s_timer);
        s_timer = 0;
    }
}

GtkWidget *gui_create_signal_plot(void)
{
    if (!s_store.start_us)
        s_store.start_us = g_get_monotonic_time();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    GtkWidget *add_btn = gtk_button_new_with_label("➕ Add Analysis Window");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_window), NULL);
    gtk_box_pack_start(GTK_BOX(bar), add_btn, FALSE, FALSE, 0);

    GtkWidget *load_btn = gtk_button_new_with_label("Load DBC…");
    g_signal_connect(load_btn, "clicked", G_CALLBACK(on_load_dbc), NULL);
    gtk_box_pack_start(GTK_BOX(bar), load_btn, FALSE, FALSE, 0);

    s_count_lbl = gtk_label_new("0 analysis windows open");
    gtk_label_set_xalign(GTK_LABEL(s_count_lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(bar), s_count_lbl, TRUE, TRUE, 8);

    gtk_box_pack_start(GTK_BOX(box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* Help / explanation panel. */
    GtkWidget *help = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(help),
        "<big><b>Signal Analysis Viewer</b></big>\n\n"
        "Click <b>Add Analysis Window</b> to open a detached graph window.\n"
        "In each window:\n"
        "  • pick signals from the <b>searchable dropdown</b> and press "
        "<b>Add</b>;\n"
        "  • the <b>side pane</b> lists each signal's colour and live value;\n"
        "  • <b>hover</b> over a plot to read that point's value and signal;\n"
        "  • adjust the <b>time window</b> or <b>pause</b> to inspect.\n\n"
        "Load a different database any time with <b>Load DBC…</b> "
        "(or the Database menu).");
    gtk_label_set_xalign(GTK_LABEL(help), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(help), 0.0f);
    gtk_widget_set_margin_start(help, 16);
    gtk_widget_set_margin_top(help, 16);
    gtk_box_pack_start(GTK_BOX(box), help, TRUE, TRUE, 0);

    g_signal_connect(box, "destroy", G_CALLBACK(on_tab_destroy), NULL);
    return box;
}
