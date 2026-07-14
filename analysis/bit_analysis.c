/**
 * @file bit_analysis.c
 * @brief GTK-independent CAN payload bit analysis engine.
 */

#include "../inc/bit_analysis.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BA_ANALYZE_MAX_SAMPLES 5000u
#define BA_INVALID_BIT 0xFFFFu
#define BA_TRANSITION_WINDOW_NS 250000000LL

typedef struct {
    double v;
    int idx;
} rank_item_t;

static int sample_index(const ba_session_t *s, size_t logical)
{
    return (int)((s->sample_head + s->sample_capacity - s->sample_count +
                  logical) % s->sample_capacity);
}

static int cmp_candidate_desc(const void *a, const void *b)
{
    const ba_candidate_t *ca = a;
    const ba_candidate_t *cb = b;
    return (cb->total_score > ca->total_score) -
           (cb->total_score < ca->total_score);
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static int cmp_rank_item(const void *a, const void *b)
{
    const rank_item_t *ra = a;
    const rank_item_t *rb = b;
    return (ra->v > rb->v) - (ra->v < rb->v);
}

static void safe_copy(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0)
        return;
    snprintf(dst, dstsz, "%s", src ? src : "");
}

int64_t ba_timespec_to_ns(const struct timespec *ts)
{
    if (!ts)
        return 0;
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

uint8_t ba_get_bit(const uint8_t *data, uint16_t bit)
{
    return data ? (uint8_t)((data[bit >> 3] >> (bit & 7)) & 1u) : 0u;
}

int ba_field_fits(uint16_t start_bit,
                  uint8_t bit_length,
                  ba_byte_order_t order,
                  uint16_t total_bits)
{
    if (bit_length == 0 || start_bit >= total_bits)
        return 0;
    if (order == BA_BYTE_ORDER_INTEL)
        return (uint32_t)start_bit + bit_length <= total_bits;

    int bit = start_bit;
    for (uint8_t i = 0; i < bit_length; i++) {
        if (bit < 0 || bit >= total_bits)
            return 0;
        if ((bit & 7) == 0)
            bit += 15;
        else
            bit -= 1;
    }
    return 1;
}

uint16_t ba_dbc_to_canonical_start_bit(uint16_t dbc_start_bit,
                                       uint8_t bit_length,
                                       ba_byte_order_t order,
                                       uint16_t total_bits)
{
    if (!ba_field_fits(dbc_start_bit, bit_length, order, total_bits))
        return BA_INVALID_BIT;
    if (order == BA_BYTE_ORDER_INTEL)
        return dbc_start_bit;

    uint16_t min_bit = BA_INVALID_BIT;
    int bit = dbc_start_bit;
    for (uint8_t i = 0; i < bit_length; i++) {
        if ((uint16_t)bit < min_bit)
            min_bit = (uint16_t)bit;
        if ((bit & 7) == 0)
            bit += 15;
        else
            bit -= 1;
    }
    return min_bit;
}

uint16_t ba_canonical_to_dbc_start_bit(uint16_t canonical_start_bit,
                                       uint8_t bit_length,
                                       ba_byte_order_t order,
                                       uint16_t total_bits)
{
    if (order == BA_BYTE_ORDER_INTEL) {
        return ba_field_fits(canonical_start_bit, bit_length, order,
                             total_bits) ? canonical_start_bit : BA_INVALID_BIT;
    }

    for (uint16_t bit = 0; bit < total_bits; bit++) {
        uint16_t c = ba_dbc_to_canonical_start_bit(bit, bit_length, order,
                                                   total_bits);
        if (c == canonical_start_bit)
            return bit;
    }
    return BA_INVALID_BIT;
}

uint64_t ba_extract_unsigned_dbc(const uint8_t *data,
                                 uint8_t dlc,
                                 uint16_t dbc_start_bit,
                                 uint8_t bit_length,
                                 ba_byte_order_t order)
{
    if (!data || bit_length == 0 || bit_length > 64)
        return 0;

    uint16_t total_bits = (uint16_t)dlc * 8u;
    if (!ba_field_fits(dbc_start_bit, bit_length, order, total_bits))
        return 0;

    uint64_t value = 0;
    if (order == BA_BYTE_ORDER_INTEL) {
        for (uint8_t i = 0; i < bit_length; i++) {
            uint16_t bit = (uint16_t)(dbc_start_bit + i);
            value |= (uint64_t)ba_get_bit(data, bit) << i;
        }
    } else {
        int bit = dbc_start_bit;
        for (uint8_t i = 0; i < bit_length; i++) {
            value |= (uint64_t)ba_get_bit(data, (uint16_t)bit)
                     << (bit_length - 1u - i);
            if ((bit & 7) == 0)
                bit += 15;
            else
                bit -= 1;
        }
    }
    return value;
}

uint64_t ba_extract_unsigned(const uint8_t *data,
                             uint8_t dlc,
                             uint16_t canonical_start_bit,
                             uint8_t bit_length,
                             ba_byte_order_t order)
{
    uint16_t total_bits = (uint16_t)dlc * 8u;
    uint16_t dbc_start = ba_canonical_to_dbc_start_bit(canonical_start_bit,
                                                       bit_length, order,
                                                       total_bits);
    if (dbc_start == BA_INVALID_BIT)
        return 0;
    return ba_extract_unsigned_dbc(data, dlc, dbc_start, bit_length, order);
}

int64_t ba_sign_extend(uint64_t raw, uint8_t bit_length)
{
    if (bit_length == 0)
        return 0;
    if (bit_length >= 64)
        return (int64_t)raw;

    uint64_t sign_bit = (uint64_t)1 << (bit_length - 1u);
    if (!(raw & sign_bit))
        return (int64_t)raw;
    uint64_t mask = ~(((uint64_t)1 << bit_length) - 1u);
    return (int64_t)(raw | mask);
}

static double entropy_for_probability(double p)
{
    if (p <= 0.0 || p >= 1.0)
        return 0.0;
    return -p * log(p) / log(2.0) - (1.0 - p) * log(1.0 - p) / log(2.0);
}

static double pearson_corr(const double *x, const double *y, size_t n)
{
    if (!x || !y || n < 2)
        return 0.0;

    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < n; i++) {
        sx += x[i];
        sy += y[i];
    }
    double mx = sx / (double)n;
    double my = sy / (double)n;

    double cov = 0.0, vx = 0.0, vy = 0.0;
    for (size_t i = 0; i < n; i++) {
        double dx = x[i] - mx;
        double dy = y[i] - my;
        cov += dx * dy;
        vx += dx * dx;
        vy += dy * dy;
    }
    if (vx <= 0.0 || vy <= 0.0)
        return 0.0;
    return cov / sqrt(vx * vy);
}

static void compute_ranks(const double *v, size_t n, double *rank)
{
    rank_item_t *items = calloc(n, sizeof(*items));
    if (!items)
        return;
    for (size_t i = 0; i < n; i++) {
        items[i].v = v[i];
        items[i].idx = (int)i;
    }
    qsort(items, n, sizeof(*items), cmp_rank_item);

    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && items[j].v == items[i].v)
            j++;
        double r = ((double)i + (double)j + 1.0) / 2.0;
        for (size_t k = i; k < j; k++)
            rank[items[k].idx] = r;
        i = j;
    }
    free(items);
}

static double spearman_corr(const double *x, const double *y, size_t n)
{
    if (n < 2)
        return 0.0;
    double *rx = calloc(n, sizeof(*rx));
    double *ry = calloc(n, sizeof(*ry));
    if (!rx || !ry) {
        free(rx);
        free(ry);
        return 0.0;
    }
    compute_ranks(x, n, rx);
    compute_ranks(y, n, ry);
    double r = pearson_corr(rx, ry, n);
    free(rx);
    free(ry);
    return r;
}

static uint32_t unique_count(double *v, size_t n)
{
    if (!v || n == 0)
        return 0;
    qsort(v, n, sizeof(*v), cmp_double);
    uint32_t count = 1;
    for (size_t i = 1; i < n; i++) {
        if (fabs(v[i] - v[i - 1]) > 1e-9)
            count++;
    }
    return count;
}

static void regression_metrics(const double *raw,
                               const double *input,
                               size_t n,
                               ba_candidate_t *c)
{
    c->factor = 1.0;
    c->offset = 0.0;
    c->pearson = pearson_corr(raw, input, n);
    c->spearman = n <= 2000 ? spearman_corr(raw, input, n) : c->pearson;
    c->monotonicity = fabs(c->spearman);
    if (n < 2)
        return;

    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < n; i++) {
        sx += raw[i];
        sy += input[i];
    }
    double mx = sx / (double)n;
    double my = sy / (double)n;

    double cov = 0.0, vx = 0.0, vy = 0.0;
    for (size_t i = 0; i < n; i++) {
        double dx = raw[i] - mx;
        double dy = input[i] - my;
        cov += dx * dy;
        vx += dx * dx;
        vy += dy * dy;
    }
    if (vx <= 0.0)
        return;

    c->factor = cov / vx;
    c->offset = my - c->factor * mx;

    double sse = 0.0, mae = 0.0, maxe = 0.0;
    for (size_t i = 0; i < n; i++) {
        double pred = c->offset + c->factor * raw[i];
        double e = fabs(input[i] - pred);
        sse += e * e;
        mae += e;
        if (e > maxe)
            maxe = e;
    }
    c->mean_absolute_error = mae / (double)n;
    c->max_absolute_error = maxe;
    c->r_squared = vy > 0.0 ? 1.0 - (sse / vy) : 0.0;
    if (c->r_squared < 0.0)
        c->r_squared = 0.0;
}

int ba_session_init(ba_session_t *s, size_t capacity)
{
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (capacity == 0)
        capacity = BA_DEFAULT_CAPACITY;
    s->samples = calloc(capacity, sizeof(*s->samples));
    if (!s->samples)
        return -1;
    s->sample_capacity = capacity;
    s->include_rx = 1;
    s->include_tx = 0;
    s->expected_dlc = BA_CLASSIC_MAX_BYTES;
    s->input_type = BA_INPUT_CONTINUOUS;
    safe_copy(s->input_name, sizeof(s->input_name), "Input");
    safe_copy(s->input_unit, sizeof(s->input_unit), "");
    s->state = BA_SESSION_IDLE;
    s->next_segment_id = 1;
    s->active_segment = -1;
    return 0;
}

void ba_session_destroy(ba_session_t *s)
{
    if (!s)
        return;
    free(s->samples);
    memset(s, 0, sizeof(*s));
}

void ba_session_reset(ba_session_t *s)
{
    if (!s)
        return;
    ba_sample_t *samples = s->samples;
    size_t cap = s->sample_capacity;
    uint32_t id = s->can_id;
    uint8_t ext = s->is_extended;
    uint8_t dlc = s->expected_dlc;
    uint8_t rx = s->include_rx;
    uint8_t tx = s->include_tx;
    uint8_t vary = s->accept_varying_dlc;
    uint8_t fd = s->analyze_fd_payload;
    ba_input_type_t it = s->input_type;
    double input_value = s->current_input_value;
    uint8_t input_valid = s->current_input_valid;
    char iname[BA_LABEL_MAX], unit[BA_UNIT_MAX];
    safe_copy(iname, sizeof(iname), s->input_name);
    safe_copy(unit, sizeof(unit), s->input_unit);
    memset(s, 0, sizeof(*s));
    s->samples = samples;
    s->sample_capacity = cap;
    s->can_id = id;
    s->is_extended = ext;
    s->expected_dlc = dlc;
    s->include_rx = rx;
    s->include_tx = tx;
    s->accept_varying_dlc = vary;
    s->analyze_fd_payload = fd;
    s->input_type = it;
    s->current_input_value = input_value;
    s->current_input_valid = input_valid;
    safe_copy(s->input_name, sizeof(s->input_name), iname);
    safe_copy(s->input_unit, sizeof(s->input_unit), unit);
    s->state = BA_SESSION_READY;
    s->next_segment_id = 1;
    s->active_segment = -1;
}

void ba_session_configure_target(ba_session_t *s,
                                 uint32_t can_id,
                                 int is_extended,
                                 uint8_t expected_dlc)
{
    if (!s)
        return;
    s->can_id = can_id;
    s->is_extended = is_extended ? 1 : 0;
    s->expected_dlc = expected_dlc > CANFD_DATA_MAX ?
                      CANFD_DATA_MAX : expected_dlc;
    ba_session_reset(s);
}

void ba_session_set_input(ba_session_t *s,
                          ba_input_type_t type,
                          const char *name,
                          double value,
                          const char *unit,
                          int valid)
{
    if (!s)
        return;
    s->input_type = type;
    safe_copy(s->input_name, sizeof(s->input_name), name);
    safe_copy(s->input_unit, sizeof(s->input_unit), unit);
    s->current_input_value = value;
    s->current_input_valid = valid ? 1 : 0;
}

uint32_t ba_session_add_segment(ba_session_t *s,
                                ba_segment_type_t type,
                                const char *label,
                                double input_value,
                                int input_valid,
                                int64_t timestamp_ns)
{
    if (!s || s->segment_count >= BA_MAX_SEGMENTS)
        return 0;
    if (s->active_segment >= 0)
        ba_session_end_segment(s, timestamp_ns);

    ba_segment_t *seg = &s->segments[s->segment_count];
    memset(seg, 0, sizeof(*seg));
    seg->id = s->next_segment_id++;
    seg->type = type;
    safe_copy(seg->label, sizeof(seg->label), label);
    seg->input_value = input_value;
    seg->input_valid = input_valid ? 1 : 0;
    seg->start_ns = timestamp_ns ? timestamp_ns : s->latest_ts_ns;
    seg->first_sample = s->total_target_frames;

    s->active_segment = (int)s->segment_count;
    s->segment_count++;
    s->current_input_value = input_value;
    s->current_input_valid = input_valid ? 1 : 0;
    return seg->id;
}

void ba_session_end_segment(ba_session_t *s, int64_t timestamp_ns)
{
    if (!s || s->active_segment < 0 ||
        (size_t)s->active_segment >= s->segment_count)
        return;
    ba_segment_t *seg = &s->segments[s->active_segment];
    seg->end_ns = timestamp_ns ? timestamp_ns : s->latest_ts_ns;
    if (s->total_target_frames >= seg->first_sample)
        seg->sample_count = s->total_target_frames - seg->first_sample;
    s->active_segment = -1;
}

static int matches_target(const ba_session_t *s, const can_msg_t *msg)
{
    if (!s || !msg || msg->is_error || msg->is_remote)
        return 0;
    if (msg->id != s->can_id ||
        (msg->is_extended ? 1 : 0) != (s->is_extended ? 1 : 0))
        return 0;
    if (msg->direction == CAN_DIR_RX && !s->include_rx)
        return 0;
    if (msg->direction == CAN_DIR_TX && !s->include_tx)
        return 0;
    if (!s->accept_varying_dlc && s->expected_dlc > 0 &&
        msg->dlc != s->expected_dlc)
        return 0;
    if (!s->analyze_fd_payload && msg->dlc > BA_CLASSIC_MAX_BYTES)
        return 0;
    return 1;
}

int ba_session_push_frame(ba_session_t *s, const can_msg_t *msg)
{
    if (!s || !msg || !s->samples || !matches_target(s, msg))
        return 0;
    if (s->state != BA_SESSION_CAPTURING)
        return 0;

    ba_sample_t *dst = &s->samples[s->sample_head];
    memset(dst, 0, sizeof(*dst));
    dst->timestamp_ns = ba_timespec_to_ns(&msg->timestamp);
    dst->sequence = msg->seq;
    dst->dlc = msg->dlc > CANFD_DATA_MAX ? CANFD_DATA_MAX : msg->dlc;
    memcpy(dst->data, msg->data, dst->dlc);
    dst->input_value = s->current_input_value;
    dst->input_valid = s->current_input_valid;
    dst->direction = msg->direction;
    if (s->active_segment >= 0 &&
        (size_t)s->active_segment < s->segment_count)
        dst->segment_id = s->segments[s->active_segment].id;

    if (s->first_ts_ns == 0)
        s->first_ts_ns = dst->timestamp_ns;
    s->latest_ts_ns = dst->timestamp_ns;
    s->latest_dlc = dst->dlc;
    memcpy(s->latest_data, dst->data, dst->dlc);

    s->sample_head = (s->sample_head + 1) % s->sample_capacity;
    if (s->sample_count < s->sample_capacity)
        s->sample_count++;
    else
        s->dropped_samples++;
    s->total_target_frames++;
    return 1;
}

static int session_has_baseline_segment(const ba_session_t *s)
{
    if (!s)
        return 0;
    for (size_t i = 0; i < s->segment_count; i++) {
        if (s->segments[i].type == BA_SEGMENT_BASELINE)
            return 1;
    }
    return 0;
}

static int sample_is_in_baseline_segment(const ba_session_t *s,
                                         const ba_sample_t *sample)
{
    if (!s || !sample || sample->segment_id == 0)
        return 0;
    for (size_t i = 0; i < s->segment_count; i++) {
        if (s->segments[i].type == BA_SEGMENT_BASELINE &&
            s->segments[i].id == sample->segment_id)
            return 1;
    }
    return 0;
}

void ba_session_capture_baseline(ba_session_t *s)
{
    if (!s || s->sample_count == 0)
        return;

    uint16_t total_bits = (uint16_t)(s->expected_dlc ? s->expected_dlc :
                          s->latest_dlc) * 8u;
    if (total_bits > BA_FD_MAX_BITS)
        total_bits = BA_FD_MAX_BITS;

    uint32_t ones[BA_FD_MAX_BITS] = {0};
    size_t baseline_n = 0;
    int use_segments = session_has_baseline_segment(s);
    for (size_t i = 0; i < s->sample_count; i++) {
        const ba_sample_t *sample = &s->samples[sample_index(s, i)];
        if (use_segments && !sample_is_in_baseline_segment(s, sample))
            continue;
        for (uint16_t bit = 0; bit < total_bits; bit++)
            ones[bit] += ba_get_bit(sample->data, bit);
        baseline_n++;
    }
    if (baseline_n == 0)
        return;

    memset(s->baseline_data, 0, sizeof(s->baseline_data));
    memset(s->baseline_bits, 0, sizeof(s->baseline_bits));
    for (uint16_t bit = 0; bit < total_bits; bit++) {
        uint8_t v = (uint64_t)ones[bit] * 2u >= baseline_n ? 1u : 0u;
        s->baseline_bits[bit] = v;
        if (v)
            s->baseline_data[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    }
    s->baseline_valid = 1;
    s->baseline_sample_count = baseline_n;
}

static size_t collect_analysis_samples(const ba_session_t *s,
                                       const ba_sample_t **out,
                                       size_t max_out)
{
    if (!s || !out || max_out == 0)
        return 0;

    size_t n = s->sample_count;
    size_t start = 0;
    if (n > max_out) {
        start = n - max_out;
        n = max_out;
    }
    for (size_t i = 0; i < n; i++)
        out[i] = &s->samples[sample_index(s, start + i)];
    return n;
}

static double compute_best_lag_ms(const ba_sample_t **samples,
                                  const double *raw,
                                  size_t n,
                                  double *scratch_raw,
                                  double *scratch_input,
                                  double *best_corr_out);

static void compute_bit_stats(ba_session_t *s,
                              const ba_sample_t **samples,
                              size_t n,
                              uint16_t total_bits)
{
    memset(s->bit_stats, 0, sizeof(s->bit_stats));
    if (n == 0)
        return;

    double *raw_series = calloc(n, sizeof(*raw_series));
    double *lag_raw = calloc(n, sizeof(*lag_raw));
    double *lag_input = calloc(n, sizeof(*lag_input));

    double min_input = 0.0, max_input = 0.0;
    int have_input = 0;
    for (size_t i = 0; i < n; i++) {
        if (!samples[i]->input_valid)
            continue;
        if (!have_input) {
            min_input = max_input = samples[i]->input_value;
            have_input = 1;
        } else {
            if (samples[i]->input_value < min_input)
                min_input = samples[i]->input_value;
            if (samples[i]->input_value > max_input)
                max_input = samples[i]->input_value;
        }
    }
    double split = (min_input + max_input) / 2.0;

    double duration_s = 0.0;
    if (n > 1 && samples[n - 1]->timestamp_ns > samples[0]->timestamp_ns)
        duration_s = (double)(samples[n - 1]->timestamp_ns -
                              samples[0]->timestamp_ns) / 1e9;

    for (uint16_t bit = 0; bit < total_bits; bit++) {
        ba_bit_stats_t *st = &s->bit_stats[bit];
        uint8_t prev = ba_get_bit(samples[0]->data, bit);
        uint64_t baseline_diff = 0;
        double xb[BA_ANALYZE_MAX_SAMPLES];
        double yi[BA_ANALYZE_MAX_SAMPLES];
        size_t corr_n = 0;
        uint64_t group_count[2] = {0, 0};
        uint64_t group_ones[2] = {0, 0};

        for (size_t i = 0; i < n; i++) {
            uint8_t v = ba_get_bit(samples[i]->data, bit);
            st->one_count += v ? 1u : 0u;
            if (i > 0 && v != prev)
                st->flip_count++;
            prev = v;
            if (s->baseline_valid && v != s->baseline_bits[bit])
                baseline_diff++;
            if (samples[i]->input_valid && corr_n < BA_ANALYZE_MAX_SAMPLES) {
                xb[corr_n] = v ? 1.0 : 0.0;
                yi[corr_n] = samples[i]->input_value;
                corr_n++;
                if (have_input) {
                    int g = samples[i]->input_value > split ? 1 : 0;
                    group_count[g]++;
                    group_ones[g] += v ? 1u : 0u;
                }
            }
        }

        st->sample_count = n;
        st->zero_count = n - st->one_count;
        st->probability_one = (double)st->one_count / (double)n;
        st->entropy_bits = entropy_for_probability(st->probability_one);
        st->baseline_value = s->baseline_valid ? s->baseline_bits[bit] : 0u;
        st->current_value = ba_get_bit(samples[n - 1]->data, bit);
        st->is_constant = st->flip_count == 0;
        st->flip_rate_hz = duration_s > 0.0 ?
                           (double)st->flip_count / duration_s : 0.0;
        st->baseline_difference_rate = s->baseline_valid ?
            (double)baseline_diff / (double)n : 0.0;
        st->pearson_correlation = pearson_corr(xb, yi, corr_n);
        if (raw_series && lag_raw && lag_input && corr_n >= 3 &&
            st->flip_count > 0) {
            for (size_t i = 0; i < n; i++)
                raw_series[i] = ba_get_bit(samples[i]->data, bit) ? 1.0 : 0.0;
            double best_corr = 0.0;
            st->best_lag_ms = compute_best_lag_ms(samples, raw_series, n,
                                                  lag_raw, lag_input,
                                                  &best_corr);
            st->lagged_score = best_corr;
        }
        if (group_count[0] > 0 && group_count[1] > 0) {
            double p0 = (double)group_ones[0] / (double)group_count[0];
            double p1 = (double)group_ones[1] / (double)group_count[1];
            st->state_separation = fabs(p1 - p0);
        }
    }

    free(raw_series);
    free(lag_raw);
    free(lag_input);
}

static double counter_score_for_values(const double *raw,
                                       size_t n,
                                       uint8_t bit_length)
{
    if (!raw || n < 32 || bit_length == 0 || bit_length > 16)
        return 0.0;
    uint64_t mod = (uint64_t)1 << bit_length;
    uint64_t ok = 0, tested = 0;
    for (size_t i = 1; i < n; i++) {
        uint64_t prev = (uint64_t)llround(raw[i - 1]);
        uint64_t cur = (uint64_t)llround(raw[i]);
        uint64_t expect = (prev + 1u) % mod;
        if (cur == expect)
            ok++;
        tested++;
    }
    return tested ? (double)ok / (double)tested : 0.0;
}

static double estimate_lag_step_ns(const ba_sample_t **samples, size_t n)
{
    if (!samples || n < 2)
        return 10000000.0;
    int64_t dt = samples[n - 1]->timestamp_ns - samples[0]->timestamp_ns;
    if (dt <= 0)
        return 10000000.0;
    double step = (double)dt / (double)(n - 1);
    if (step < 1000000.0)
        step = 1000000.0;
    if (step > 50000000.0)
        step = 50000000.0;
    return step;
}

static double lagged_corr_for_values(const ba_sample_t **samples,
                                     const double *raw,
                                     size_t n,
                                     int64_t lag_ns,
                                     double *scratch_raw,
                                     double *scratch_input)
{
    if (!samples || !raw || !scratch_raw || !scratch_input || n < 3)
        return 0.0;

    size_t m = 0;
    size_t input_idx = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t target_ns = samples[i]->timestamp_ns - lag_ns;
        if (target_ns < samples[0]->timestamp_ns)
            continue;
        while (input_idx + 1 < n &&
               samples[input_idx + 1]->timestamp_ns <= target_ns) {
            input_idx++;
        }
        if (!samples[input_idx]->input_valid)
            continue;
        scratch_raw[m] = raw[i];
        scratch_input[m] = samples[input_idx]->input_value;
        m++;
    }
    return m >= 3 ? pearson_corr(scratch_raw, scratch_input, m) : 0.0;
}

static size_t collect_lagged_pairs(const ba_sample_t **samples,
                                   const double *raw,
                                   size_t n,
                                   int64_t lag_ns,
                                   double *out_raw,
                                   double *out_input)
{
    if (!samples || !raw || !out_raw || !out_input || n < 3)
        return 0;

    size_t m = 0;
    size_t input_idx = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t target_ns = samples[i]->timestamp_ns - lag_ns;
        if (target_ns < samples[0]->timestamp_ns)
            continue;
        while (input_idx + 1 < n &&
               samples[input_idx + 1]->timestamp_ns <= target_ns) {
            input_idx++;
        }
        if (!samples[input_idx]->input_valid)
            continue;
        out_raw[m] = raw[i];
        out_input[m] = samples[input_idx]->input_value;
        m++;
    }
    return m;
}

static double compute_best_lag_ms(const ba_sample_t **samples,
                                  const double *raw,
                                  size_t n,
                                  double *scratch_raw,
                                  double *scratch_input,
                                  double *best_corr_out)
{
    if (best_corr_out)
        *best_corr_out = 0.0;
    if (!samples || !raw || n < 3)
        return 0.0;

    double step_ns = estimate_lag_step_ns(samples, n);
    int max_steps = (int)ceil(500000000.0 / step_ns);
    if (max_steps < 0)
        max_steps = 0;
    if (max_steps > 500)
        max_steps = 500;

    double best_lag_ms = 0.0;
    double best_corr = 0.0;
    for (int step = 0; step <= max_steps; step++) {
        int64_t lag_ns = (int64_t)llround((double)step * step_ns);
        double corr = lagged_corr_for_values(samples, raw, n, lag_ns,
                                             scratch_raw, scratch_input);
        if (step == 0 || fabs(corr) > fabs(best_corr)) {
            best_corr = corr;
            best_lag_ms = (double)lag_ns / 1e6;
        }
    }
    if (best_corr_out)
        *best_corr_out = best_corr;
    return best_lag_ms;
}

static double checksum_like_score(const ba_candidate_t *c,
                                  const ba_session_t *s)
{
    if (!c || !s)
        return 0.0;
    if (!(c->bit_length == 4 || c->bit_length == 8 ||
          c->bit_length == 16))
        return 0.0;

    double entropy = 0.0;
    int bits = 0;
    uint16_t total_bits = (uint16_t)(s->expected_dlc ? s->expected_dlc :
                          s->latest_dlc) * 8u;
    for (uint8_t i = 0; i < c->bit_length; i++) {
        uint16_t bit = (uint16_t)(c->canonical_start_bit + i);
        if (bit >= total_bits)
            continue;
        entropy += s->bit_stats[bit].entropy_bits;
        bits++;
    }
    if (bits == 0)
        return 0.0;
    entropy /= (double)bits;
    double low_corr = 1.0 - fabs(c->pearson);
    if (low_corr < 0.0)
        low_corr = 0.0;
    return entropy * low_corr * (1.0 - fmin(c->counter_score, 1.0));
}

static double timestamp_score_for_values(ba_candidate_t *c,
                                         const ba_sample_t **samples,
                                         const double *raw,
                                         size_t n)
{
    if (!c || !samples || !raw || n < 16 || c->is_signed ||
        c->bit_length < 8 || c->unique_raw_values < 8 ||
        samples[n - 1]->timestamp_ns <= samples[0]->timestamp_ns)
        return 0.0;

    double first_t = (double)samples[0]->timestamp_ns / 1e9;
    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < n; i++) {
        sx += (double)samples[i]->timestamp_ns / 1e9 - first_t;
        sy += raw[i];
    }
    double mx = sx / (double)n;
    double my = sy / (double)n;

    double cov = 0.0, vx = 0.0, vy = 0.0;
    uint32_t monotonic_steps = 0;
    uint32_t tested_steps = 0;
    for (size_t i = 0; i < n; i++) {
        double x = (double)samples[i]->timestamp_ns / 1e9 - first_t;
        double dx = x - mx;
        double dy = raw[i] - my;
        cov += dx * dy;
        vx += dx * dx;
        vy += dy * dy;
        if (i > 0 && samples[i]->timestamp_ns > samples[i - 1]->timestamp_ns) {
            tested_steps++;
            if (raw[i] >= raw[i - 1])
                monotonic_steps++;
        }
    }
    if (vx <= 0.0 || vy <= 0.0 || tested_steps == 0)
        return 0.0;

    double slope = cov / vx; /* raw counts per second */
    if (slope <= 0.0)
        return 0.0;

    double corr = cov / sqrt(vx * vy);
    double r2 = corr * corr;
    double monotonic = (double)monotonic_steps / (double)tested_steps;
    double low_input_dependency = c->sample_count >= 3 &&
                                  c->unique_input_levels >= 2 ?
        fmax(0.0, 1.0 - fabs(c->pearson)) : 1.0;
    double counter_penalty = fmax(0.0, 1.0 - c->counter_score);

    c->timestamp_resolution_ms = 1000.0 / slope;
    if (c->bit_length < 64)
        c->timestamp_wrap_value = 1ull << c->bit_length;
    else
        c->timestamp_wrap_value = 0;

    double score = r2 * monotonic * low_input_dependency * counter_penalty;
    if (score > 1.0)
        score = 1.0;
    return score;
}

static int candidate_contains_bit(const ba_candidate_t *c, uint16_t bit)
{
    if (!c)
        return 0;
    if (c->byte_order == BA_BYTE_ORDER_INTEL)
        return bit >= c->canonical_start_bit &&
               bit < (uint16_t)(c->canonical_start_bit + c->bit_length);

    int walk = c->dbc_start_bit;
    for (uint8_t i = 0; i < c->bit_length; i++) {
        if ((uint16_t)walk == bit)
            return 1;
        if ((walk & 7) == 0)
            walk += 15;
        else
            walk -= 1;
    }
    return 0;
}

static void append_bit_range(char *buf, size_t bufsz,
                             uint16_t start, uint16_t end)
{
    if (!buf || bufsz == 0)
        return;
    size_t used = strlen(buf);
    if (used >= bufsz - 1)
        return;
    int n;
    if (start == end) {
        n = snprintf(buf + used, bufsz - used, "%s%u",
                     used ? "," : "", start);
    } else {
        n = snprintf(buf + used, bufsz - used, "%s%u-%u",
                     used ? "," : "", start, end);
    }
    if (n < 0 || (size_t)n >= bufsz - used)
        buf[bufsz - 1] = '\0';
}

static double mux_score_for_values(ba_candidate_t *c,
                                   const ba_session_t *s,
                                   const ba_sample_t **samples,
                                   const double *raw,
                                   size_t n)
{
    if (!c || !s || !samples || !raw || n < 40 || c->is_signed ||
        c->bit_length == 0 || c->bit_length > 8)
        return 0.0;

    c->mux_unique_values = 0;
    c->mux_min_frames_per_value = 0;
    c->mux_conditionally_active_bits = 0;
    c->mux_active_ranges[0] = '\0';

    double values[16];
    size_t counts[16] = {0};
    int group_idx[BA_ANALYZE_MAX_SAMPLES];
    size_t groups = 0;

    for (size_t i = 0; i < n; i++) {
        int found = -1;
        for (size_t g = 0; g < groups; g++) {
            if (fabs(values[g] - raw[i]) < 0.5) {
                found = (int)g;
                break;
            }
        }
        if (found < 0) {
            if (groups >= 16)
                return 0.0;
            values[groups] = raw[i];
            found = (int)groups++;
        }
        counts[found]++;
        group_idx[i] = found;
    }

    if (groups < 2 || groups > 16)
        return 0.0;
    c->mux_unique_values = (uint32_t)groups;

    size_t min_count = counts[0];
    for (size_t g = 1; g < groups; g++)
        if (counts[g] < min_count)
            min_count = counts[g];
    c->mux_min_frames_per_value = (uint32_t)min_count;

    size_t min_needed = n >= groups * 20u ? 20u : n / (groups * 2u);
    if (min_needed < 8)
        min_needed = 8;
    for (size_t g = 0; g < groups; g++)
        if (counts[g] < min_needed)
            return 0.0;

    uint16_t bytes = s->analyze_fd_payload ?
        (uint16_t)(s->expected_dlc ? s->expected_dlc : s->latest_dlc) :
        BA_CLASSIC_MAX_BYTES;
    if (bytes == 0)
        bytes = s->latest_dlc ? s->latest_dlc : BA_CLASSIC_MAX_BYTES;
    if (!s->analyze_fd_payload && bytes > BA_CLASSIC_MAX_BYTES)
        bytes = BA_CLASSIC_MAX_BYTES;
    if (bytes > BA_FD_MAX_BYTES)
        bytes = BA_FD_MAX_BYTES;
    uint16_t total_bits = (uint16_t)bytes * 8u;

    double separated = 0.0;
    double strength = 0.0;
    uint8_t active_bits[BA_FD_MAX_BITS] = {0};
    for (uint16_t bit = 0; bit < total_bits; bit++) {
        if (candidate_contains_bit(c, bit))
            continue;

        double ones[16] = {0.0};
        for (size_t i = 0; i < n; i++)
            ones[group_idx[i]] += ba_get_bit(samples[i]->data, bit) ? 1.0 : 0.0;

        double min_p = 1.0;
        double max_p = 0.0;
        for (size_t g = 0; g < groups; g++) {
            double p = counts[g] ? ones[g] / (double)counts[g] : 0.0;
            if (p < min_p) min_p = p;
            if (p > max_p) max_p = p;
        }

        double spread = max_p - min_p;
        if (spread >= 0.45) {
            separated += 1.0;
            strength += spread;
            active_bits[bit] = 1u;
        }
    }

    c->mux_conditionally_active_bits = (uint32_t)separated;
    for (uint16_t bit = 0; bit < total_bits; bit++) {
        if (!active_bits[bit])
            continue;
        uint16_t start = bit;
        while (bit + 1u < total_bits && active_bits[bit + 1u])
            bit++;
        append_bit_range(c->mux_active_ranges, sizeof(c->mux_active_ranges),
                         start, bit);
    }

    if (separated < 4.0)
        return 0.0;

    double density = fmin(separated / 12.0, 1.0);
    double avg_strength = strength / separated;
    double score = 0.70 * density + 0.30 * avg_strength;
    if (score > 1.0)
        score = 1.0;
    return score;
}

static double transition_score_for_values(const ba_sample_t **samples,
                                          const double *raw,
                                          size_t n,
                                          double best_lag_ms,
                                          uint32_t *match_count_out,
                                          uint32_t *transition_count_out)
{
    if (match_count_out)
        *match_count_out = 0;
    if (transition_count_out)
        *transition_count_out = 0;
    if (!samples || !raw || n < 2)
        return 0.0;

    double min_input = 0.0, max_input = 0.0;
    int have_input = 0;
    for (size_t i = 0; i < n; i++) {
        if (!samples[i]->input_valid)
            continue;
        if (!have_input) {
            min_input = max_input = samples[i]->input_value;
            have_input = 1;
        } else {
            if (samples[i]->input_value < min_input)
                min_input = samples[i]->input_value;
            if (samples[i]->input_value > max_input)
                max_input = samples[i]->input_value;
        }
    }
    if (!have_input || fabs(max_input - min_input) < 1e-9)
        return 0.0;

    double split = (min_input + max_input) / 2.0;
    double state_sum[2] = {0.0, 0.0};
    size_t state_count[2] = {0, 0};
    for (size_t i = 0; i < n; i++) {
        if (!samples[i]->input_valid)
            continue;
        int state = samples[i]->input_value > split ? 1 : 0;
        state_sum[state] += raw[i];
        state_count[state]++;
    }
    if (!state_count[0] || !state_count[1])
        return 0.0;

    double state_mean[2] = {
        state_sum[0] / (double)state_count[0],
        state_sum[1] / (double)state_count[1]
    };
    if (fabs(state_mean[1] - state_mean[0]) < 1e-9)
        return 0.0;

    int64_t lag_ns = fabs(best_lag_ms) > 0.001 ?
        (int64_t)llround(best_lag_ms * 1e6) : 0;
    uint32_t transitions = 0;
    uint32_t matches = 0;
    size_t search_start = 1;

    for (size_t i = 1; i < n; i++) {
        if (!samples[i - 1]->input_valid || !samples[i]->input_valid)
            continue;
        int prev_state = samples[i - 1]->input_value > split ? 1 : 0;
        int next_state = samples[i]->input_value > split ? 1 : 0;
        if (prev_state == next_state)
            continue;

        transitions++;
        double expected_delta =
            state_mean[next_state] - state_mean[prev_state];
        int64_t target_ns = samples[i]->timestamp_ns + lag_ns;
        int found = 0;
        while (search_start < n &&
               samples[search_start]->timestamp_ns <
               target_ns - BA_TRANSITION_WINDOW_NS)
            search_start++;
        for (size_t j = search_start; j < n; j++) {
            int64_t dt = samples[j]->timestamp_ns - target_ns;
            if (dt > BA_TRANSITION_WINDOW_NS)
                break;
            double raw_delta = raw[j] - raw[j - 1];
            if (fabs(raw_delta) < 1e-9)
                continue;
            if ((expected_delta > 0.0 && raw_delta > 0.0) ||
                (expected_delta < 0.0 && raw_delta < 0.0)) {
                found = 1;
                break;
            }
        }
        if (found)
            matches++;
    }

    if (match_count_out)
        *match_count_out = matches;
    if (transition_count_out)
        *transition_count_out = transitions;
    return transitions ? (double)matches / (double)transitions : 0.0;
}

static void fill_confidence(ba_candidate_t *c)
{
    if (c->counter_score >= 0.95) {
        safe_copy(c->confidence, sizeof(c->confidence), "Counter");
    } else if (c->timestamp_score >= 0.85) {
        safe_copy(c->confidence, sizeof(c->confidence), "Timestamp-like");
    } else if (c->checksum_score >= 0.75) {
        safe_copy(c->confidence, sizeof(c->confidence), "Checksum-like");
    } else if (c->mux_score >= 0.75) {
        safe_copy(c->confidence, sizeof(c->confidence), "Mux-like");
    } else if (c->total_score >= 0.85) {
        safe_copy(c->confidence, sizeof(c->confidence), "Probable");
    } else if (c->total_score >= 0.55) {
        safe_copy(c->confidence, sizeof(c->confidence), "Candidate");
    } else {
        safe_copy(c->confidence, sizeof(c->confidence), "Unknown");
    }
}

static double candidate_retention_score(const ba_candidate_t *c)
{
    if (!c)
        return 0.0;
    double score = c->total_score;
    if (c->counter_score >= 0.90)
        score += 0.10;
    if (c->timestamp_score >= 0.85)
        score += 0.10;
    if (c->checksum_score >= 0.75) {
        score += 0.08;
        if ((c->bit_length == 8 && c->canonical_start_bit % 8u == 0) ||
            (c->bit_length == 16 && c->canonical_start_bit % 8u == 0) ||
            (c->bit_length == 4 && c->canonical_start_bit % 4u == 0))
            score += 0.04;
    }
    if (c->mux_score >= 0.75)
        score += 0.10;
    score -= (double)c->bit_length / 2048.0;
    return score;
}

static void append_candidate(ba_session_t *s, const ba_candidate_t *src)
{
    if (s->candidate_count < BA_MAX_CANDIDATES) {
        s->candidates[s->candidate_count++] = *src;
        return;
    }

    size_t worst = 0;
    double worst_score = candidate_retention_score(&s->candidates[0]);
    for (size_t i = 1; i < s->candidate_count; i++) {
        double score = candidate_retention_score(&s->candidates[i]);
        if (score < worst_score) {
            worst = i;
            worst_score = score;
        }
    }
    if (candidate_retention_score(src) > worst_score)
        s->candidates[worst] = *src;
}

static void evaluate_candidate(ba_session_t *s,
                               const ba_sample_t **samples,
                               size_t n,
                               uint16_t canonical_start,
                               uint16_t dbc_start,
                               uint8_t len,
                               ba_byte_order_t order,
                               int is_signed)
{
    double *raw = calloc(n, sizeof(*raw));
    double *input = calloc(n, sizeof(*input));
    double *raw_unique = calloc(n, sizeof(*raw_unique));
    double *input_unique = calloc(n, sizeof(*input_unique));
    double *raw_series = calloc(n, sizeof(*raw_series));
    double *lag_raw = calloc(n, sizeof(*lag_raw));
    double *lag_input = calloc(n, sizeof(*lag_input));
    if (!raw || !input || !raw_unique || !input_unique ||
        !raw_series || !lag_raw || !lag_input) {
        free(raw);
        free(input);
        free(raw_unique);
        free(input_unique);
        free(raw_series);
        free(lag_raw);
        free(lag_input);
        return;
    }

    ba_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.canonical_start_bit = canonical_start;
    c.dbc_start_bit = dbc_start;
    c.bit_length = len;
    c.byte_order = order;
    c.is_signed = is_signed ? 1u : 0u;
    c.raw_min_unsigned = UINT64_MAX;
    c.raw_min_signed = INT64_MAX;
    c.raw_max_signed = INT64_MIN;

    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t u = ba_extract_unsigned_dbc(samples[i]->data,
                                             samples[i]->dlc,
                                             dbc_start, len, order);
        int64_t sv = ba_sign_extend(u, len);
        double rv = is_signed ? (double)sv : (double)u;

        if (u < c.raw_min_unsigned) c.raw_min_unsigned = u;
        if (u > c.raw_max_unsigned) c.raw_max_unsigned = u;
        if (sv < c.raw_min_signed) c.raw_min_signed = sv;
        if (sv > c.raw_max_signed) c.raw_max_signed = sv;
        if (rv < c.physical_min || i == 0) c.physical_min = rv;
        if (rv > c.physical_max || i == 0) c.physical_max = rv;

        raw[i] = rv;
        raw_series[i] = rv;
        raw_unique[i] = rv;
        if (samples[i]->input_valid) {
            input[m] = samples[i]->input_value;
            raw[m] = rv;
            input_unique[m] = samples[i]->input_value;
            m++;
        }
    }

    c.sample_count = (uint32_t)(m ? m : n);
    c.counter_score = counter_score_for_values(raw_unique, n, len);
    c.unique_raw_values = unique_count(raw_unique, n);
    c.unique_input_levels = m ? unique_count(input_unique, m) : 0u;

    if (m >= 2 && c.unique_raw_values >= 2 && c.unique_input_levels >= 2) {
        regression_metrics(raw, input, m, &c);
        double best_corr = 0.0;
        c.best_lag_ms = compute_best_lag_ms(samples, raw_series, n,
                                            lag_raw, lag_input, &best_corr);
        if (fabs(best_corr) > fabs(c.pearson) &&
            fabs(c.best_lag_ms) > 0.001) {
            int64_t lag_ns = (int64_t)llround(c.best_lag_ms * 1e6);
            size_t lag_m = collect_lagged_pairs(samples, raw_series, n,
                                                lag_ns, lag_raw, lag_input);
            if (lag_m >= 3) {
                double best_lag_ms = c.best_lag_ms;
                regression_metrics(lag_raw, lag_input, lag_m, &c);
                c.best_lag_ms = best_lag_ms;
                c.sample_count = (uint32_t)lag_m;
            }
        } else if (fabs(best_corr) > fabs(c.pearson)) {
            c.pearson = best_corr;
        }
        c.physical_min = c.offset + c.factor * (is_signed ?
            (double)c.raw_min_signed : (double)c.raw_min_unsigned);
        c.physical_max = c.offset + c.factor * (is_signed ?
            (double)c.raw_max_signed : (double)c.raw_max_unsigned);
    }

    double transition_score = 0.0;
    if (m >= 2 && s->input_type == BA_INPUT_BOOLEAN) {
        double min_i = input[0], max_i = input[0];
        for (size_t i = 1; i < m; i++) {
            if (input[i] < min_i) min_i = input[i];
            if (input[i] > max_i) max_i = input[i];
        }
        double split = (min_i + max_i) / 2.0;
        double sum[2] = {0.0, 0.0};
        size_t cnt[2] = {0, 0};
        for (size_t i = 0; i < m; i++) {
            int g = input[i] > split ? 1 : 0;
            sum[g] += raw[i];
            cnt[g]++;
        }
        if (cnt[0] && cnt[1]) {
            double mean0 = sum[0] / (double)cnt[0];
            double mean1 = sum[1] / (double)cnt[1];
            double span = fabs((double)c.raw_max_signed -
                               (double)c.raw_min_signed);
            if (!is_signed)
                span = (double)(c.raw_max_unsigned - c.raw_min_unsigned);
            c.state_separation = span > 0.0 ?
                fmin(fabs(mean1 - mean0) / span, 1.0) :
                (fabs(mean1 - mean0) > 0.0 ? 1.0 : 0.0);
        }
        if (c.unique_raw_values <= 4) {
            transition_score = transition_score_for_values(samples, raw_series, n,
                                                           c.best_lag_ms,
                                                           &c.transition_matches,
                                                           &c.transition_total);
        }
    }

    c.checksum_score = checksum_like_score(&c, s);
    c.timestamp_score = timestamp_score_for_values(&c, samples,
                                                   raw_series, n);
    c.mux_score = mux_score_for_values(&c, s, samples, raw_series, n);

    double corr_score = fmax(fabs(c.pearson), fabs(c.spearman));
    double r2 = c.r_squared < 0.0 ? 0.0 : c.r_squared;
    double complexity = (double)len / 64.0;
    if (canonical_start % 8u == 0)
        complexity *= 0.75;
    if (len == 1 || len == 2 || len == 4 || len == 8 || len == 12 ||
        len == 16 || len == 24 || len == 32)
        complexity *= 0.80;

    if (m >= 2) {
        c.total_score =
            0.25 * corr_score +
            0.20 * fabs(c.spearman) +
            0.20 * r2 +
            0.15 * c.monotonicity +
            0.15 * c.state_separation +
            0.10 * transition_score -
            0.08 * complexity -
            0.18 * c.counter_score -
            0.08 * c.timestamp_score -
            0.08 * c.checksum_score +
            0.08 * c.mux_score;
    } else {
        double entropy = 0.0;
        for (uint8_t i = 0; i < len; i++) {
            uint16_t bit = (uint16_t)(canonical_start + i);
            if (bit < BA_FD_MAX_BITS)
                entropy += s->bit_stats[bit].entropy_bits;
        }
        entropy /= (double)len;
        c.total_score = 0.40 * entropy + 0.35 * c.counter_score +
                        0.30 * c.timestamp_score +
                        0.25 * c.checksum_score + 0.25 * c.mux_score -
                        0.05 * complexity;
    }
    if (c.total_score < 0.0)
        c.total_score = 0.0;
    if (c.counter_score >= 0.90 && c.total_score < c.counter_score * 0.92)
        c.total_score = c.counter_score * 0.92;
    if (c.timestamp_score >= 0.85 &&
        c.total_score < c.timestamp_score * 0.86)
        c.total_score = c.timestamp_score * 0.86;
    if (c.checksum_score >= 0.75) {
        double floor_score = c.checksum_score * 0.80;
        if ((len == 8 && canonical_start % 8u == 0) ||
            (len == 16 && canonical_start % 8u == 0) ||
            (len == 4 && canonical_start % 4u == 0))
            floor_score += 0.08;
        if (c.total_score < floor_score)
            c.total_score = floor_score;
    }
    if (c.mux_score >= 0.75 && c.total_score < c.mux_score * 0.88)
        c.total_score = c.mux_score * 0.88;
    if (c.total_score > 1.0)
        c.total_score = 1.0;

    snprintf(c.proposed_name, sizeof(c.proposed_name),
             "Signal_%u_%u", (unsigned)canonical_start, (unsigned)len);
    fill_confidence(&c);
    append_candidate(s, &c);

    free(raw);
    free(input);
    free(raw_unique);
    free(input_unique);
    free(raw_series);
    free(lag_raw);
    free(lag_input);
}

void ba_session_analyze(ba_session_t *s, uint8_t max_field_len)
{
    if (!s || !s->samples)
        return;
    s->state = BA_SESSION_ANALYZING;
    s->candidate_count = 0;

    const ba_sample_t **samples = calloc(BA_ANALYZE_MAX_SAMPLES,
                                        sizeof(*samples));
    if (!samples) {
        s->state = BA_SESSION_REVIEW;
        return;
    }

    size_t n = collect_analysis_samples(s, samples, BA_ANALYZE_MAX_SAMPLES);
    uint16_t bytes = s->analyze_fd_payload ?
        (uint16_t)(s->expected_dlc ? s->expected_dlc : s->latest_dlc) :
        BA_CLASSIC_MAX_BYTES;
    if (bytes == 0)
        bytes = s->latest_dlc ? s->latest_dlc : BA_CLASSIC_MAX_BYTES;
    if (!s->analyze_fd_payload && bytes > BA_CLASSIC_MAX_BYTES)
        bytes = BA_CLASSIC_MAX_BYTES;
    if (bytes > BA_FD_MAX_BYTES)
        bytes = BA_FD_MAX_BYTES;
    uint16_t total_bits = (uint16_t)bytes * 8u;

    compute_bit_stats(s, samples, n, total_bits);
    if (n < 2 || total_bits == 0) {
        free(samples);
        s->state = BA_SESSION_REVIEW;
        return;
    }

    if (max_field_len == 0)
        max_field_len = 16;
    if (max_field_len > 32)
        max_field_len = 32;
    if (max_field_len > total_bits)
        max_field_len = (uint8_t)total_bits;

    for (uint8_t len = 1; len <= max_field_len; len++) {
        for (uint16_t start = 0; start + len <= total_bits; start++) {
            evaluate_candidate(s, samples, n, start, start, len,
                               BA_BYTE_ORDER_INTEL, 0);
            if (len > 1)
                evaluate_candidate(s, samples, n, start, start, len,
                                   BA_BYTE_ORDER_INTEL, 1);
        }

        for (uint16_t dbc_start = 0; dbc_start < total_bits; dbc_start++) {
            if (!ba_field_fits(dbc_start, len, BA_BYTE_ORDER_MOTOROLA,
                               total_bits))
                continue;
            uint16_t canonical = ba_dbc_to_canonical_start_bit(
                dbc_start, len, BA_BYTE_ORDER_MOTOROLA, total_bits);
            if (canonical == BA_INVALID_BIT)
                continue;
            evaluate_candidate(s, samples, n, canonical, dbc_start, len,
                               BA_BYTE_ORDER_MOTOROLA, 0);
            if (len > 1)
                evaluate_candidate(s, samples, n, canonical, dbc_start, len,
                                   BA_BYTE_ORDER_MOTOROLA, 1);
        }
    }

    qsort(s->candidates, s->candidate_count, sizeof(s->candidates[0]),
          cmp_candidate_desc);
    free(samples);
    s->state = BA_SESSION_REVIEW;
}

static int sample_is_in_segment_type(const ba_session_t *s,
                                     const ba_sample_t *sample,
                                     ba_segment_type_t type)
{
    if (!s || !sample || sample->segment_id == 0)
        return 0;
    for (size_t i = 0; i < s->segment_count; i++) {
        if (s->segments[i].type == type &&
            s->segments[i].id == sample->segment_id)
            return 1;
    }
    return 0;
}

static const ba_sample_t *lag_aligned_input_sample(const ba_session_t *s,
                                                   size_t logical_sample,
                                                   int64_t lag_ns,
                                                   size_t *input_logical)
{
    if (!s || s->sample_count == 0 || logical_sample >= s->sample_count)
        return NULL;

    const ba_sample_t *sample =
        &s->samples[sample_index(s, logical_sample)];
    if (lag_ns <= 0)
        return sample->input_valid ? sample : NULL;

    int64_t target_ns = sample->timestamp_ns - lag_ns;
    const ba_sample_t *first = &s->samples[sample_index(s, 0)];
    if (target_ns < first->timestamp_ns || !input_logical)
        return NULL;

    while (*input_logical + 1 < s->sample_count) {
        const ba_sample_t *next =
            &s->samples[sample_index(s, *input_logical + 1)];
        if (next->timestamp_ns > target_ns)
            break;
        (*input_logical)++;
    }

    const ba_sample_t *input =
        &s->samples[sample_index(s, *input_logical)];
    return input->input_valid ? input : NULL;
}

int ba_candidate_validate_segments(const ba_session_t *s,
                                   const ba_candidate_t *c,
                                   double max_mean_absolute_error,
                                   size_t *sample_count_out,
                                   double *mean_absolute_error_out,
                                   double *max_absolute_error_out)
{
    if (sample_count_out) *sample_count_out = 0;
    if (mean_absolute_error_out) *mean_absolute_error_out = 0.0;
    if (max_absolute_error_out) *max_absolute_error_out = 0.0;
    if (!s || !c || !s->samples || s->sample_count == 0)
        return 0;

    int64_t lag_ns = fabs(c->best_lag_ms) > 0.001 ?
        (int64_t)llround(c->best_lag_ms * 1e6) : 0;
    size_t input_logical = 0;
    size_t count = 0;
    double sum_abs = 0.0;
    double max_abs = 0.0;

    for (size_t i = 0; i < s->sample_count; i++) {
        const ba_sample_t *sample = &s->samples[sample_index(s, i)];
        if (!sample_is_in_segment_type(s, sample, BA_SEGMENT_VALIDATION))
            continue;

        const ba_sample_t *input_sample =
            lag_aligned_input_sample(s, i, lag_ns, &input_logical);
        if (!input_sample)
            continue;

        uint64_t raw = ba_extract_unsigned_dbc(sample->data, sample->dlc,
                                               c->dbc_start_bit,
                                               c->bit_length,
                                               c->byte_order);
        double raw_value = c->is_signed ?
            (double)ba_sign_extend(raw, c->bit_length) : (double)raw;
        double predicted = c->offset + c->factor * raw_value;
        double err = fabs(predicted - input_sample->input_value);
        sum_abs += err;
        if (err > max_abs)
            max_abs = err;
        count++;
    }

    double mae = count ? sum_abs / (double)count : 0.0;
    if (sample_count_out) *sample_count_out = count;
    if (mean_absolute_error_out) *mean_absolute_error_out = mae;
    if (max_absolute_error_out) *max_absolute_error_out = max_abs;
    if (count < 3)
        return 0;

    double tolerance = max_mean_absolute_error;
    if (tolerance < 0.0) {
        double span = fabs(c->physical_max - c->physical_min);
        tolerance = fmax(c->mean_absolute_error * 3.0, span * 0.02);
        if (tolerance < 1e-6)
            tolerance = 1e-6;
    }

    return mae <= tolerance && max_abs <= fmax(tolerance * 4.0, tolerance);
}

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    if (!err || errsz == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static void json_write_escaped(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(s ? s : "");
         *p; p++) {
        switch (*p) {
        case '\\': fputs("\\\\", fp); break;
        case '"':  fputs("\\\"", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (*p < 0x20)
                fprintf(fp, "\\u%04x", *p);
            else
                fputc(*p, fp);
            break;
        }
    }
    fputc('"', fp);
}

static void json_write_hex(FILE *fp, const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    fputc('"', fp);
    for (size_t i = 0; i < len; i++) {
        fputc(hex[data[i] >> 4], fp);
        fputc(hex[data[i] & 0x0f], fp);
    }
    fputc('"', fp);
}

static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *json_skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"')
        return p;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return end;
}

static const char *json_find_key(const char *start,
                                 const char *end,
                                 const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = start; p < end; p++) {
        if (*p != '"')
            continue;
        if ((size_t)(end - p) < klen + 2)
            break;
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') {
            p = json_skip_string(p, end) - 1;
            continue;
        }
        const char *q = json_skip_ws(p + klen + 2, end);
        if (q < end && *q == ':')
            return json_skip_ws(q + 1, end);
    }
    return NULL;
}

static int json_get_range(const char *start,
                          const char *end,
                          const char *key,
                          char open,
                          char close,
                          const char **out_start,
                          const char **out_end)
{
    const char *p = json_find_key(start, end, key);
    if (!p || *p != open)
        return 0;

    int depth = 0;
    for (const char *q = p; q < end; q++) {
        if (*q == '"') {
            q = json_skip_string(q, end) - 1;
            continue;
        }
        if (*q == open)
            depth++;
        else if (*q == close) {
            depth--;
            if (depth == 0) {
                *out_start = p + 1;
                *out_end = q;
                return 1;
            }
        }
    }
    return 0;
}

static int json_next_object(const char **cursor,
                            const char *end,
                            const char **obj_start,
                            const char **obj_end)
{
    const char *p = *cursor;
    while (p < end && *p != '{') {
        if (*p == '"')
            p = json_skip_string(p, end);
        else
            p++;
    }
    if (p >= end)
        return 0;

    int depth = 0;
    for (const char *q = p; q < end; q++) {
        if (*q == '"') {
            q = json_skip_string(q, end) - 1;
            continue;
        }
        if (*q == '{')
            depth++;
        else if (*q == '}') {
            depth--;
            if (depth == 0) {
                *obj_start = p + 1;
                *obj_end = q;
                *cursor = q + 1;
                return 1;
            }
        }
    }
    return 0;
}

static int json_get_u64(const char *start,
                        const char *end,
                        const char *key,
                        uint64_t *out)
{
    const char *p = json_find_key(start, end, key);
    if (!p)
        return 0;
    errno = 0;
    char *stop = NULL;
    unsigned long long v = strtoull(p, &stop, 10);
    if (errno || stop == p)
        return 0;
    *out = (uint64_t)v;
    return 1;
}

static int json_get_i64(const char *start,
                        const char *end,
                        const char *key,
                        int64_t *out)
{
    const char *p = json_find_key(start, end, key);
    if (!p)
        return 0;
    errno = 0;
    char *stop = NULL;
    long long v = strtoll(p, &stop, 10);
    if (errno || stop == p)
        return 0;
    *out = (int64_t)v;
    return 1;
}

static int json_get_double(const char *start,
                           const char *end,
                           const char *key,
                           double *out)
{
    const char *p = json_find_key(start, end, key);
    if (!p)
        return 0;
    errno = 0;
    char *stop = NULL;
    double v = strtod(p, &stop);
    if (errno || stop == p)
        return 0;
    *out = v;
    return 1;
}

static int json_get_bool(const char *start,
                         const char *end,
                         const char *key,
                         uint8_t *out)
{
    const char *p = json_find_key(start, end, key);
    if (!p)
        return 0;
    if ((size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if ((size_t)(end - p) >= 5 && strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    uint64_t v = 0;
    if (json_get_u64(start, end, key, &v)) {
        *out = v ? 1u : 0u;
        return 1;
    }
    return 0;
}

static int json_get_string(const char *start,
                           const char *end,
                           const char *key,
                           char *out,
                           size_t outsz)
{
    const char *p = json_find_key(start, end, key);
    if (!p || *p != '"' || !out || outsz == 0)
        return 0;
    p++;
    size_t n = 0;
    while (p < end) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[n] = '\0';
            return 1;
        }
        if (ch == '\\' && p < end) {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"': case '\\': case '/': break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                if ((size_t)(end - p) >= 4)
                    p += 4;
                ch = '?';
                break;
            default:
                break;
            }
        }
        if (n + 1 < outsz)
            out[n++] = (char)ch;
    }
    out[n] = '\0';
    return 0;
}

static int hex_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static size_t parse_hex_bytes(const char *hex, uint8_t *out, size_t outsz)
{
    size_t n = 0;
    if (!hex || !out)
        return 0;
    while (hex[0] && hex[1] && n < outsz) {
        int hi = hex_value((unsigned char)hex[0]);
        int lo = hex_value((unsigned char)hex[1]);
        if (hi < 0 || lo < 0)
            break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static void parse_segment_object(const char *start,
                                 const char *end,
                                 ba_segment_t *seg)
{
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0.0;
    if (json_get_u64(start, end, "id", &u))
        seg->id = (uint32_t)u;
    if (json_get_u64(start, end, "type", &u))
        seg->type = (ba_segment_type_t)u;
    json_get_string(start, end, "label", seg->label, sizeof(seg->label));
    if (json_get_double(start, end, "input_value", &d))
        seg->input_value = d;
    json_get_bool(start, end, "input_valid", &seg->input_valid);
    if (json_get_i64(start, end, "start_ns", &i))
        seg->start_ns = i;
    if (json_get_i64(start, end, "end_ns", &i))
        seg->end_ns = i;
    if (json_get_u64(start, end, "first_sample", &u))
        seg->first_sample = (size_t)u;
    if (json_get_u64(start, end, "sample_count", &u))
        seg->sample_count = (size_t)u;
}

static void parse_candidate_object(const char *start,
                                   const char *end,
                                   ba_candidate_t *c)
{
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0.0;
    if (json_get_u64(start, end, "canonical_start_bit", &u))
        c->canonical_start_bit = (uint16_t)u;
    if (json_get_u64(start, end, "dbc_start_bit", &u))
        c->dbc_start_bit = (uint16_t)u;
    if (json_get_u64(start, end, "bit_length", &u))
        c->bit_length = (uint8_t)u;
    if (json_get_u64(start, end, "byte_order", &u))
        c->byte_order = (ba_byte_order_t)u;
    json_get_bool(start, end, "is_signed", &c->is_signed);
    if (json_get_i64(start, end, "raw_min_signed", &i))
        c->raw_min_signed = i;
    if (json_get_i64(start, end, "raw_max_signed", &i))
        c->raw_max_signed = i;
    if (json_get_u64(start, end, "raw_min_unsigned", &u))
        c->raw_min_unsigned = u;
    if (json_get_u64(start, end, "raw_max_unsigned", &u))
        c->raw_max_unsigned = u;
    if (json_get_double(start, end, "factor", &d)) c->factor = d;
    if (json_get_double(start, end, "offset", &d)) c->offset = d;
    if (json_get_double(start, end, "physical_min", &d)) c->physical_min = d;
    if (json_get_double(start, end, "physical_max", &d)) c->physical_max = d;
    if (json_get_double(start, end, "pearson", &d)) c->pearson = d;
    if (json_get_double(start, end, "spearman", &d)) c->spearman = d;
    if (json_get_double(start, end, "r_squared", &d)) c->r_squared = d;
    if (json_get_double(start, end, "mean_absolute_error", &d))
        c->mean_absolute_error = d;
    if (json_get_double(start, end, "max_absolute_error", &d))
        c->max_absolute_error = d;
    if (json_get_double(start, end, "validation_mean_absolute_error", &d))
        c->validation_mean_absolute_error = d;
    if (json_get_double(start, end, "validation_max_absolute_error", &d))
        c->validation_max_absolute_error = d;
    if (json_get_double(start, end, "monotonicity", &d)) c->monotonicity = d;
    if (json_get_double(start, end, "best_lag_ms", &d)) c->best_lag_ms = d;
    if (json_get_double(start, end, "counter_score", &d)) c->counter_score = d;
    if (json_get_double(start, end, "timestamp_score", &d))
        c->timestamp_score = d;
    if (json_get_double(start, end, "timestamp_resolution_ms", &d))
        c->timestamp_resolution_ms = d;
    if (json_get_double(start, end, "checksum_score", &d))
        c->checksum_score = d;
    if (json_get_double(start, end, "mux_score", &d)) c->mux_score = d;
    if (json_get_double(start, end, "total_score", &d)) c->total_score = d;
    if (json_get_double(start, end, "state_separation", &d))
        c->state_separation = d;
    if (json_get_u64(start, end, "sample_count", &u))
        c->sample_count = (uint32_t)u;
    if (json_get_u64(start, end, "validation_sample_count", &u))
        c->validation_sample_count = (uint32_t)u;
    if (json_get_u64(start, end, "unique_raw_values", &u))
        c->unique_raw_values = (uint32_t)u;
    if (json_get_u64(start, end, "unique_input_levels", &u))
        c->unique_input_levels = (uint32_t)u;
    if (json_get_u64(start, end, "transition_matches", &u))
        c->transition_matches = (uint32_t)u;
    if (json_get_u64(start, end, "transition_total", &u))
        c->transition_total = (uint32_t)u;
    if (json_get_u64(start, end, "mux_unique_values", &u))
        c->mux_unique_values = (uint32_t)u;
    if (json_get_u64(start, end, "mux_min_frames_per_value", &u))
        c->mux_min_frames_per_value = (uint32_t)u;
    if (json_get_u64(start, end, "mux_conditionally_active_bits", &u))
        c->mux_conditionally_active_bits = (uint32_t)u;
    if (json_get_u64(start, end, "timestamp_wrap_value", &u))
        c->timestamp_wrap_value = u;
    json_get_string(start, end, "proposed_name",
                    c->proposed_name, sizeof(c->proposed_name));
    json_get_string(start, end, "unit", c->unit, sizeof(c->unit));
    json_get_string(start, end, "notes", c->notes, sizeof(c->notes));
    json_get_string(start, end, "mux_active_ranges",
                    c->mux_active_ranges, sizeof(c->mux_active_ranges));
    json_get_string(start, end, "confidence",
                    c->confidence, sizeof(c->confidence));
}

static int parse_sample_object(const char *start,
                               const char *end,
                               ba_sample_t *sample)
{
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0.0;
    char hex[CANFD_DATA_MAX * 2 + 1];

    memset(sample, 0, sizeof(*sample));
    if (json_get_i64(start, end, "timestamp_ns", &i))
        sample->timestamp_ns = i;
    if (json_get_u64(start, end, "sequence", &u))
        sample->sequence = u;
    if (json_get_u64(start, end, "dlc", &u))
        sample->dlc = (uint8_t)(u > CANFD_DATA_MAX ? CANFD_DATA_MAX : u);
    if (json_get_u64(start, end, "segment_id", &u))
        sample->segment_id = (uint32_t)u;
    if (json_get_double(start, end, "input_value", &d))
        sample->input_value = d;
    json_get_bool(start, end, "input_valid", &sample->input_valid);
    if (json_get_u64(start, end, "direction", &u))
        sample->direction = (uint8_t)u;
    if (json_get_string(start, end, "data", hex, sizeof(hex)))
        parse_hex_bytes(hex, sample->data, sample->dlc);
    return 1;
}

int ba_session_save_json(const ba_session_t *s,
                         const char *path,
                         char *err,
                         size_t errsz)
{
    if (!s || !path || !*path) {
        set_err(err, errsz, "invalid session or path");
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        set_err(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"format\": \"canoscope-bit-analysis\",\n");
    fprintf(fp, "  \"version\": 1,\n");
    fprintf(fp, "  \"target\": {\n");
    fprintf(fp, "    \"id\": %" PRIu32 ",\n", s->can_id);
    fprintf(fp, "    \"extended\": %s,\n",
            s->is_extended ? "true" : "false");
    fprintf(fp, "    \"dlc\": %u,\n", s->expected_dlc);
    fprintf(fp, "    \"include_rx\": %s,\n",
            s->include_rx ? "true" : "false");
    fprintf(fp, "    \"include_tx\": %s,\n",
            s->include_tx ? "true" : "false");
    fprintf(fp, "    \"accept_varying_dlc\": %s,\n",
            s->accept_varying_dlc ? "true" : "false");
    fprintf(fp, "    \"analyze_fd_payload\": %s\n",
            s->analyze_fd_payload ? "true" : "false");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"input\": {\n");
    fprintf(fp, "    \"type\": %d,\n", (int)s->input_type);
    fprintf(fp, "    \"name\": ");
    json_write_escaped(fp, s->input_name);
    fprintf(fp, ",\n    \"unit\": ");
    json_write_escaped(fp, s->input_unit);
    fprintf(fp, ",\n    \"current_value\": %.17g,\n",
            s->current_input_value);
    fprintf(fp, "    \"current_valid\": %s\n",
            s->current_input_valid ? "true" : "false");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"capture\": {\n");
    fprintf(fp, "    \"state\": %d,\n", (int)s->state);
    fprintf(fp, "    \"state_name\": ");
    json_write_escaped(fp, ba_session_state_name(s->state));
    fprintf(fp, ",\n    \"sample_capacity\": %zu,\n", s->sample_capacity);
    fprintf(fp, "    \"sample_count\": %zu,\n", s->sample_count);
    fprintf(fp, "    \"sample_head\": %zu,\n", s->sample_head);
    fprintf(fp, "    \"total_target_frames\": %" PRIu64 ",\n",
            s->total_target_frames);
    fprintf(fp, "    \"dropped_samples\": %" PRIu64 ",\n",
            s->dropped_samples);
    fprintf(fp, "    \"first_ts_ns\": %" PRId64 ",\n", s->first_ts_ns);
    fprintf(fp, "    \"latest_ts_ns\": %" PRId64 ",\n", s->latest_ts_ns);
    fprintf(fp, "    \"latest_dlc\": %u,\n", s->latest_dlc);
    fprintf(fp, "    \"latest_data\": ");
    json_write_hex(fp, s->latest_data, CANFD_DATA_MAX);
    fprintf(fp, "\n  },\n");

    fprintf(fp, "  \"baseline\": {\n");
    fprintf(fp, "    \"valid\": %s,\n",
            s->baseline_valid ? "true" : "false");
    fprintf(fp, "    \"sample_count\": %zu,\n", s->baseline_sample_count);
    fprintf(fp, "    \"data\": ");
    json_write_hex(fp, s->baseline_data, CANFD_DATA_MAX);
    fprintf(fp, "\n  },\n");

    fprintf(fp, "  \"segments\": [\n");
    for (size_t i = 0; i < s->segment_count; i++) {
        const ba_segment_t *seg = &s->segments[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"id\": %" PRIu32 ",\n", seg->id);
        fprintf(fp, "      \"type\": %d,\n", (int)seg->type);
        fprintf(fp, "      \"type_name\": ");
        json_write_escaped(fp, ba_segment_type_name(seg->type));
        fprintf(fp, ",\n      \"label\": ");
        json_write_escaped(fp, seg->label);
        fprintf(fp, ",\n      \"input_value\": %.17g,\n",
                seg->input_value);
        fprintf(fp, "      \"input_valid\": %s,\n",
                seg->input_valid ? "true" : "false");
        fprintf(fp, "      \"start_ns\": %" PRId64 ",\n", seg->start_ns);
        fprintf(fp, "      \"end_ns\": %" PRId64 ",\n", seg->end_ns);
        fprintf(fp, "      \"first_sample\": %zu,\n", seg->first_sample);
        fprintf(fp, "      \"sample_count\": %zu\n", seg->sample_count);
        fprintf(fp, "    }%s\n", i + 1 < s->segment_count ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"candidates\": [\n");
    for (size_t i = 0; i < s->candidate_count; i++) {
        const ba_candidate_t *c = &s->candidates[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"canonical_start_bit\": %u,\n",
                c->canonical_start_bit);
        fprintf(fp, "      \"dbc_start_bit\": %u,\n", c->dbc_start_bit);
        fprintf(fp, "      \"bit_length\": %u,\n", c->bit_length);
        fprintf(fp, "      \"byte_order\": %d,\n", (int)c->byte_order);
        fprintf(fp, "      \"is_signed\": %s,\n",
                c->is_signed ? "true" : "false");
        fprintf(fp, "      \"raw_min_signed\": %" PRId64 ",\n",
                c->raw_min_signed);
        fprintf(fp, "      \"raw_max_signed\": %" PRId64 ",\n",
                c->raw_max_signed);
        fprintf(fp, "      \"raw_min_unsigned\": %" PRIu64 ",\n",
                c->raw_min_unsigned);
        fprintf(fp, "      \"raw_max_unsigned\": %" PRIu64 ",\n",
                c->raw_max_unsigned);
        fprintf(fp, "      \"factor\": %.17g,\n", c->factor);
        fprintf(fp, "      \"offset\": %.17g,\n", c->offset);
        fprintf(fp, "      \"physical_min\": %.17g,\n",
                c->physical_min);
        fprintf(fp, "      \"physical_max\": %.17g,\n",
                c->physical_max);
        fprintf(fp, "      \"pearson\": %.17g,\n", c->pearson);
        fprintf(fp, "      \"spearman\": %.17g,\n", c->spearman);
        fprintf(fp, "      \"r_squared\": %.17g,\n", c->r_squared);
        fprintf(fp, "      \"mean_absolute_error\": %.17g,\n",
                c->mean_absolute_error);
        fprintf(fp, "      \"max_absolute_error\": %.17g,\n",
                c->max_absolute_error);
        fprintf(fp, "      \"validation_mean_absolute_error\": %.17g,\n",
                c->validation_mean_absolute_error);
        fprintf(fp, "      \"validation_max_absolute_error\": %.17g,\n",
                c->validation_max_absolute_error);
        fprintf(fp, "      \"monotonicity\": %.17g,\n",
                c->monotonicity);
        fprintf(fp, "      \"best_lag_ms\": %.17g,\n",
                c->best_lag_ms);
        fprintf(fp, "      \"counter_score\": %.17g,\n",
                c->counter_score);
        fprintf(fp, "      \"timestamp_score\": %.17g,\n",
                c->timestamp_score);
        fprintf(fp, "      \"timestamp_resolution_ms\": %.17g,\n",
                c->timestamp_resolution_ms);
        fprintf(fp, "      \"checksum_score\": %.17g,\n",
                c->checksum_score);
        fprintf(fp, "      \"mux_score\": %.17g,\n", c->mux_score);
        fprintf(fp, "      \"total_score\": %.17g,\n", c->total_score);
        fprintf(fp, "      \"state_separation\": %.17g,\n",
                c->state_separation);
        fprintf(fp, "      \"sample_count\": %" PRIu32 ",\n",
                c->sample_count);
        fprintf(fp, "      \"validation_sample_count\": %" PRIu32 ",\n",
                c->validation_sample_count);
        fprintf(fp, "      \"unique_raw_values\": %" PRIu32 ",\n",
                c->unique_raw_values);
        fprintf(fp, "      \"unique_input_levels\": %" PRIu32 ",\n",
                c->unique_input_levels);
        fprintf(fp, "      \"transition_matches\": %" PRIu32 ",\n",
                c->transition_matches);
        fprintf(fp, "      \"transition_total\": %" PRIu32 ",\n",
                c->transition_total);
        fprintf(fp, "      \"mux_unique_values\": %" PRIu32 ",\n",
                c->mux_unique_values);
        fprintf(fp, "      \"mux_min_frames_per_value\": %" PRIu32 ",\n",
                c->mux_min_frames_per_value);
        fprintf(fp, "      \"mux_conditionally_active_bits\": %" PRIu32 ",\n",
                c->mux_conditionally_active_bits);
        fprintf(fp, "      \"timestamp_wrap_value\": %" PRIu64 ",\n",
                c->timestamp_wrap_value);
        fprintf(fp, "      \"proposed_name\": ");
        json_write_escaped(fp, c->proposed_name);
        fprintf(fp, ",\n      \"unit\": ");
        json_write_escaped(fp, c->unit);
        fprintf(fp, ",\n      \"notes\": ");
        json_write_escaped(fp, c->notes);
        fprintf(fp, ",\n      \"mux_active_ranges\": ");
        json_write_escaped(fp, c->mux_active_ranges);
        fprintf(fp, ",\n      \"confidence\": ");
        json_write_escaped(fp, c->confidence);
        fprintf(fp, "\n    }%s\n",
                i + 1 < s->candidate_count ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"samples\": [\n");
    for (size_t i = 0; i < s->sample_count; i++) {
        const ba_sample_t *sample = &s->samples[sample_index(s, i)];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"timestamp_ns\": %" PRId64 ",\n",
                sample->timestamp_ns);
        fprintf(fp, "      \"sequence\": %" PRIu64 ",\n",
                sample->sequence);
        fprintf(fp, "      \"dlc\": %u,\n", sample->dlc);
        fprintf(fp, "      \"direction\": %u,\n", sample->direction);
        fprintf(fp, "      \"input_value\": %.17g,\n",
                sample->input_value);
        fprintf(fp, "      \"input_valid\": %s,\n",
                sample->input_valid ? "true" : "false");
        fprintf(fp, "      \"segment_id\": %" PRIu32 ",\n",
                sample->segment_id);
        fprintf(fp, "      \"data\": ");
        json_write_hex(fp, sample->data, sample->dlc);
        fprintf(fp, "\n    }%s\n", i + 1 < s->sample_count ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    if (fclose(fp) != 0) {
        set_err(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int ba_session_load_json(ba_session_t *s,
                         const char *path,
                         char *err,
                         size_t errsz)
{
    if (!s || !path || !*path) {
        set_err(err, errsz, "invalid session or path");
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        set_err(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        set_err(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    long fsz = ftell(fp);
    if (fsz < 0) {
        fclose(fp);
        set_err(err, errsz, "%s: %s", path, strerror(errno));
        return -1;
    }
    rewind(fp);

    char *json = calloc((size_t)fsz + 1u, 1u);
    if (!json) {
        fclose(fp);
        set_err(err, errsz, "out of memory reading %s", path);
        return -1;
    }
    size_t got = fread(json, 1, (size_t)fsz, fp);
    fclose(fp);
    if (got != (size_t)fsz) {
        free(json);
        set_err(err, errsz, "short read from %s", path);
        return -1;
    }

    const char *root_start = json;
    const char *root_end = json + got;
    char format[64];
    if (!json_get_string(root_start, root_end, "format",
                         format, sizeof(format)) ||
        strcmp(format, "canoscope-bit-analysis") != 0) {
        free(json);
        set_err(err, errsz, "%s is not a CANoScope Bit Analysis session",
                path);
        return -1;
    }

    const char *target_s = NULL, *target_e = NULL;
    const char *input_s = NULL, *input_e = NULL;
    const char *capture_s = NULL, *capture_e = NULL;
    const char *baseline_s = NULL, *baseline_e = NULL;
    const char *segments_s = NULL, *segments_e = NULL;
    const char *samples_s = NULL, *samples_e = NULL;
    const char *candidates_s = NULL, *candidates_e = NULL;
    if (!json_get_range(root_start, root_end, "target", '{', '}',
                        &target_s, &target_e) ||
        !json_get_range(root_start, root_end, "capture", '{', '}',
                        &capture_s, &capture_e)) {
        free(json);
        set_err(err, errsz, "%s is missing target or capture data", path);
        return -1;
    }
    json_get_range(root_start, root_end, "input", '{', '}',
                   &input_s, &input_e);
    json_get_range(root_start, root_end, "baseline", '{', '}',
                   &baseline_s, &baseline_e);
    json_get_range(root_start, root_end, "segments", '[', ']',
                   &segments_s, &segments_e);
    json_get_range(root_start, root_end, "samples", '[', ']',
                   &samples_s, &samples_e);
    json_get_range(root_start, root_end, "candidates", '[', ']',
                   &candidates_s, &candidates_e);

    uint64_t cap_u = BA_DEFAULT_CAPACITY;
    uint64_t count_u = 0;
    json_get_u64(capture_s, capture_e, "sample_capacity", &cap_u);
    json_get_u64(capture_s, capture_e, "sample_count", &count_u);
    if (cap_u < count_u)
        cap_u = count_u;
    if (cap_u == 0)
        cap_u = 1;

    ba_session_t tmp;
    if (ba_session_init(&tmp, (size_t)cap_u) != 0) {
        free(json);
        set_err(err, errsz, "out of memory loading session");
        return -1;
    }

    uint64_t u = 0;
    int64_t i64 = 0;
    double d = 0.0;
    if (json_get_u64(target_s, target_e, "id", &u))
        tmp.can_id = (uint32_t)u;
    json_get_bool(target_s, target_e, "extended", &tmp.is_extended);
    if (json_get_u64(target_s, target_e, "dlc", &u))
        tmp.expected_dlc = (uint8_t)(u > CANFD_DATA_MAX ? CANFD_DATA_MAX : u);
    json_get_bool(target_s, target_e, "include_rx", &tmp.include_rx);
    json_get_bool(target_s, target_e, "include_tx", &tmp.include_tx);
    json_get_bool(target_s, target_e, "accept_varying_dlc",
                  &tmp.accept_varying_dlc);
    json_get_bool(target_s, target_e, "analyze_fd_payload",
                  &tmp.analyze_fd_payload);

    if (input_s) {
        if (json_get_u64(input_s, input_e, "type", &u))
            tmp.input_type = (ba_input_type_t)u;
        json_get_string(input_s, input_e, "name",
                        tmp.input_name, sizeof(tmp.input_name));
        json_get_string(input_s, input_e, "unit",
                        tmp.input_unit, sizeof(tmp.input_unit));
        if (json_get_double(input_s, input_e, "current_value", &d))
            tmp.current_input_value = d;
        json_get_bool(input_s, input_e, "current_valid",
                      &tmp.current_input_valid);
    }

    if (json_get_u64(capture_s, capture_e, "state", &u)) {
        tmp.state = (ba_session_state_t)u;
        if (tmp.state == BA_SESSION_CAPTURING ||
            tmp.state == BA_SESSION_ANALYZING)
            tmp.state = BA_SESSION_PAUSED;
    }
    json_get_u64(capture_s, capture_e, "total_target_frames",
                 &tmp.total_target_frames);
    json_get_u64(capture_s, capture_e, "dropped_samples",
                 &tmp.dropped_samples);
    if (json_get_i64(capture_s, capture_e, "first_ts_ns", &i64))
        tmp.first_ts_ns = i64;
    if (json_get_i64(capture_s, capture_e, "latest_ts_ns", &i64))
        tmp.latest_ts_ns = i64;
    if (json_get_u64(capture_s, capture_e, "latest_dlc", &u))
        tmp.latest_dlc = (uint8_t)(u > CANFD_DATA_MAX ? CANFD_DATA_MAX : u);
    char hex[CANFD_DATA_MAX * 2 + 1];
    if (json_get_string(capture_s, capture_e, "latest_data",
                        hex, sizeof(hex)))
        parse_hex_bytes(hex, tmp.latest_data, CANFD_DATA_MAX);

    if (baseline_s) {
        json_get_bool(baseline_s, baseline_e, "valid",
                      &tmp.baseline_valid);
        if (json_get_u64(baseline_s, baseline_e, "sample_count", &u))
            tmp.baseline_sample_count = (size_t)u;
        if (json_get_string(baseline_s, baseline_e, "data",
                            hex, sizeof(hex))) {
            parse_hex_bytes(hex, tmp.baseline_data, CANFD_DATA_MAX);
            for (uint16_t bit = 0; bit < BA_FD_MAX_BITS; bit++)
                tmp.baseline_bits[bit] = ba_get_bit(tmp.baseline_data, bit);
        }
    }

    if (segments_s) {
        const char *cur = segments_s;
        const char *os = NULL, *oe = NULL;
        while (tmp.segment_count < BA_MAX_SEGMENTS &&
               json_next_object(&cur, segments_e, &os, &oe)) {
            ba_segment_t *seg = &tmp.segments[tmp.segment_count];
            memset(seg, 0, sizeof(*seg));
            parse_segment_object(os, oe, seg);
            if (seg->id >= tmp.next_segment_id)
                tmp.next_segment_id = seg->id + 1u;
            tmp.segment_count++;
        }
    }

    if (samples_s) {
        const char *cur = samples_s;
        const char *os = NULL, *oe = NULL;
        while (tmp.sample_count < tmp.sample_capacity &&
               json_next_object(&cur, samples_e, &os, &oe)) {
            ba_sample_t *sample = &tmp.samples[tmp.sample_count];
            parse_sample_object(os, oe, sample);
            tmp.sample_count++;
        }
        tmp.sample_head = tmp.sample_count == tmp.sample_capacity ?
                          0 : tmp.sample_count;
        if (tmp.total_target_frames < tmp.sample_count)
            tmp.total_target_frames = tmp.sample_count;
        if (tmp.sample_count > 0) {
            const ba_sample_t *first = &tmp.samples[0];
            const ba_sample_t *last = &tmp.samples[tmp.sample_count - 1];
            if (tmp.first_ts_ns == 0)
                tmp.first_ts_ns = first->timestamp_ns;
            tmp.latest_ts_ns = last->timestamp_ns;
            tmp.latest_dlc = last->dlc;
            memcpy(tmp.latest_data, last->data, last->dlc);
        }
    }

    if (tmp.sample_count >= 2)
        ba_session_analyze(&tmp, 16);

    if (candidates_s) {
        tmp.candidate_count = 0;
        const char *cur = candidates_s;
        const char *os = NULL, *oe = NULL;
        while (tmp.candidate_count < BA_MAX_CANDIDATES &&
               json_next_object(&cur, candidates_e, &os, &oe)) {
            ba_candidate_t *c = &tmp.candidates[tmp.candidate_count];
            memset(c, 0, sizeof(*c));
            parse_candidate_object(os, oe, c);
            tmp.candidate_count++;
        }
    }

    ba_session_destroy(s);
    *s = tmp;
    free(json);
    return 0;
}

const char *ba_byte_order_name(ba_byte_order_t order)
{
    return order == BA_BYTE_ORDER_MOTOROLA ? "Motorola" : "Intel";
}

const char *ba_segment_type_name(ba_segment_type_t type)
{
    switch (type) {
    case BA_SEGMENT_BASELINE: return "Baseline";
    case BA_SEGMENT_STATIC_STATE: return "Static";
    case BA_SEGMENT_TOGGLE: return "Toggle";
    case BA_SEGMENT_STEP: return "Step";
    case BA_SEGMENT_RAMP: return "Ramp";
    case BA_SEGMENT_BOUNDARY: return "Boundary";
    case BA_SEGMENT_VALIDATION: return "Validation";
    case BA_SEGMENT_EVENT: return "Event";
    default: return "Unknown";
    }
}

const char *ba_input_type_name(ba_input_type_t type)
{
    switch (type) {
    case BA_INPUT_BOOLEAN: return "Boolean";
    case BA_INPUT_ENUMERATED: return "Enumerated";
    case BA_INPUT_CONTINUOUS: return "Continuous";
    case BA_INPUT_EVENT_ONLY: return "Event";
    default: return "Unknown";
    }
}

const char *ba_session_state_name(ba_session_state_t state)
{
    switch (state) {
    case BA_SESSION_IDLE: return "Idle";
    case BA_SESSION_READY: return "Ready";
    case BA_SESSION_CAPTURING: return "Capturing";
    case BA_SESSION_PAUSED: return "Paused";
    case BA_SESSION_ANALYZING: return "Analyzing";
    case BA_SESSION_REVIEW: return "Review";
    default: return "Unknown";
    }
}
