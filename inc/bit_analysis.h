/**
 * @file bit_analysis.h
 * @brief GTK-independent engine for CAN payload bit analysis.
 *
 * @details
 * Provides the data model and analysis API behind the Bit Analysis tab.  The
 * engine stores selected target frames in a preallocated ring buffer, tracks
 * experiment segments and manual input values, computes per-bit statistics,
 * scans candidate fields in Intel and Motorola byte order, estimates
 * factor/offset, classifies protocol metadata, validates candidates on
 * independent segments, and saves/loads analysis sessions as JSON.
 */
#ifndef BIT_ANALYSIS_H
#define BIT_ANALYSIS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "can_message.h"

#define BA_CLASSIC_MAX_BYTES 8
#define BA_CLASSIC_MAX_BITS  64
#define BA_FD_MAX_BYTES      CANFD_DATA_MAX
#define BA_FD_MAX_BITS       (CANFD_DATA_MAX * 8)
#define BA_LABEL_MAX         64
#define BA_UNIT_MAX          24
#define BA_NOTE_MAX          256
#define BA_DEFAULT_CAPACITY  200000u
#define BA_MAX_CANDIDATES    512u
#define BA_MAX_SEGMENTS      256u

typedef enum {
    BA_SESSION_IDLE = 0,
    BA_SESSION_READY,
    BA_SESSION_CAPTURING,
    BA_SESSION_PAUSED,
    BA_SESSION_ANALYZING,
    BA_SESSION_REVIEW
} ba_session_state_t;

typedef enum {
    BA_SEGMENT_BASELINE = 0,
    BA_SEGMENT_STATIC_STATE,
    BA_SEGMENT_TOGGLE,
    BA_SEGMENT_STEP,
    BA_SEGMENT_RAMP,
    BA_SEGMENT_BOUNDARY,
    BA_SEGMENT_VALIDATION,
    BA_SEGMENT_EVENT
} ba_segment_type_t;

typedef enum {
    BA_INPUT_BOOLEAN = 0,
    BA_INPUT_ENUMERATED,
    BA_INPUT_CONTINUOUS,
    BA_INPUT_EVENT_ONLY
} ba_input_type_t;

typedef enum {
    BA_BYTE_ORDER_INTEL = 0,
    BA_BYTE_ORDER_MOTOROLA
} ba_byte_order_t;

typedef struct {
    int64_t  timestamp_ns;
    uint64_t sequence;
    uint8_t  dlc;
    uint8_t  data[CANFD_DATA_MAX];
    double   input_value;
    uint32_t segment_id;
    uint8_t  input_valid;
    uint8_t  direction;
} ba_sample_t;

typedef struct {
    uint32_t id;
    ba_segment_type_t type;
    char label[BA_LABEL_MAX];
    double input_value;
    int64_t start_ns;
    int64_t end_ns;
    size_t first_sample;
    size_t sample_count;
    uint8_t input_valid;
} ba_segment_t;

typedef struct {
    uint64_t sample_count;
    uint64_t one_count;
    uint64_t zero_count;
    uint64_t flip_count;

    double probability_one;
    double flip_rate_hz;
    double entropy_bits;

    double baseline_difference_rate;
    double state_separation;
    double pearson_correlation;
    double best_lag_ms;
    double lagged_score;

    uint8_t current_value;
    uint8_t baseline_value;
    uint8_t is_constant;
} ba_bit_stats_t;

typedef struct {
    uint16_t canonical_start_bit;
    uint16_t dbc_start_bit;
    uint8_t bit_length;
    ba_byte_order_t byte_order;
    uint8_t is_signed;

    int64_t raw_min_signed;
    int64_t raw_max_signed;
    uint64_t raw_min_unsigned;
    uint64_t raw_max_unsigned;

    double factor;
    double offset;
    double physical_min;
    double physical_max;

    double pearson;
    double spearman;
    double r_squared;
    double mean_absolute_error;
    double max_absolute_error;
    double validation_mean_absolute_error;
    double validation_max_absolute_error;
    double monotonicity;
    double best_lag_ms;

    double counter_score;
    double timestamp_score;
    double timestamp_resolution_ms;
    double checksum_score;
    double mux_score;
    double total_score;
    double state_separation;

    uint32_t sample_count;
    uint32_t validation_sample_count;
    uint32_t unique_raw_values;
    uint32_t unique_input_levels;
    uint32_t transition_matches;
    uint32_t transition_total;
    uint32_t mux_unique_values;
    uint32_t mux_min_frames_per_value;
    uint32_t mux_conditionally_active_bits;
    uint64_t timestamp_wrap_value;

    char proposed_name[BA_LABEL_MAX];
    char unit[BA_UNIT_MAX];
    char notes[BA_NOTE_MAX];
    char mux_active_ranges[BA_NOTE_MAX];
    char confidence[24];
} ba_candidate_t;

typedef struct {
    uint32_t can_id;
    uint8_t is_extended;
    uint8_t expected_dlc;
    uint8_t include_rx;
    uint8_t include_tx;
    uint8_t accept_varying_dlc;
    uint8_t analyze_fd_payload;

    ba_input_type_t input_type;
    char input_name[BA_LABEL_MAX];
    char input_unit[BA_UNIT_MAX];
    double current_input_value;
    uint8_t current_input_valid;

    ba_session_state_t state;

    ba_sample_t *samples;
    size_t sample_capacity;
    size_t sample_count;
    size_t sample_head;
    uint64_t total_target_frames;
    uint64_t dropped_samples;

    ba_segment_t segments[BA_MAX_SEGMENTS];
    size_t segment_count;
    uint32_t next_segment_id;
    int active_segment;

    uint8_t baseline_data[CANFD_DATA_MAX];
    uint8_t baseline_bits[BA_FD_MAX_BITS];
    uint8_t baseline_valid;
    size_t baseline_sample_count;

    ba_bit_stats_t bit_stats[BA_FD_MAX_BITS];
    ba_candidate_t candidates[BA_MAX_CANDIDATES];
    size_t candidate_count;

    uint8_t latest_data[CANFD_DATA_MAX];
    uint8_t latest_dlc;
    int64_t first_ts_ns;
    int64_t latest_ts_ns;
} ba_session_t;

/**
 * @brief Initialise a Bit Analysis session and allocate its sample ring.
 * @param s         Session object to initialise.
 * @param capacity  Ring capacity in target frames; 0 uses #BA_DEFAULT_CAPACITY.
 * @return 0 on success, -1 on allocation or argument failure.
 */
int ba_session_init(ba_session_t *s, size_t capacity);

/**
 * @brief Release memory owned by a Bit Analysis session.
 * @param s  Session to destroy; may be NULL.
 */
void ba_session_destroy(ba_session_t *s);

/**
 * @brief Reset samples, markers, baseline, and analysis results for a target.
 * @param s  Session to reset while preserving allocated storage and settings.
 */
void ba_session_reset(ba_session_t *s);

/**
 * @brief Select the CAN identifier and DLC that the session should capture.
 * @param s             Session to configure.
 * @param can_id        Standard or extended CAN identifier.
 * @param is_extended   Non-zero for a 29-bit identifier.
 * @param expected_dlc  Expected payload length in bytes.
 */
void ba_session_configure_target(ba_session_t *s,
                                 uint32_t can_id,
                                 int is_extended,
                                 uint8_t expected_dlc);

/**
 * @brief Filter and append one CAN frame to the target-frame ring buffer.
 * @param s    Active session.
 * @param msg  CAN/CAN FD frame to evaluate.
 * @return 1 when the frame was accepted and copied, otherwise 0.
 */
int ba_session_push_frame(ba_session_t *s, const can_msg_t *msg);

/**
 * @brief Set the current manual input value associated with subsequent frames.
 * @param s      Active session.
 * @param type   Input channel type.
 * @param name   Human-readable input name.
 * @param value  Numeric input value.
 * @param unit   Engineering unit.
 * @param valid  Non-zero when @p value is meaningful for fitting.
 */
void ba_session_set_input(ba_session_t *s,
                          ba_input_type_t type,
                          const char *name,
                          double value,
                          const char *unit,
                          int valid);

/**
 * @brief Start a new experiment segment at a timestamp.
 * @param s             Active session.
 * @param type          Segment type.
 * @param label         Optional user-visible label.
 * @param input_value   Input value assigned to the segment.
 * @param input_valid   Non-zero if @p input_value should be used.
 * @param timestamp_ns  Segment start timestamp in nanoseconds.
 * @return Non-zero segment id on success, 0 when no segment slot is available.
 */
uint32_t ba_session_add_segment(ba_session_t *s,
                                ba_segment_type_t type,
                                const char *label,
                                double input_value,
                                int input_valid,
                                int64_t timestamp_ns);

/**
 * @brief Close the active experiment segment.
 * @param s             Active session.
 * @param timestamp_ns  End timestamp in nanoseconds; 0 uses latest sample time.
 */
void ba_session_end_segment(ba_session_t *s, int64_t timestamp_ns);

/**
 * @brief Build a statistical baseline from baseline segments or all samples.
 * @param s  Active session with target samples.
 */
void ba_session_capture_baseline(ba_session_t *s);

/**
 * @brief Compute bit statistics and ranked field candidates.
 * @param s              Active session.
 * @param max_field_len  Maximum contiguous field length to scan in bits.
 */
void ba_session_analyze(ba_session_t *s, uint8_t max_field_len);

/**
 * @brief Validate a candidate against independent validation segments.
 * @param s                             Session containing validation samples.
 * @param c                             Candidate to validate.
 * @param max_mean_absolute_error       Negative for automatic tolerance.
 * @param sample_count_out              Optional validated sample count output.
 * @param mean_absolute_error_out       Optional validation MAE output.
 * @param max_absolute_error_out        Optional validation max-error output.
 * @return 1 when validation passes, otherwise 0.
 */
int ba_candidate_validate_segments(const ba_session_t *s,
                                   const ba_candidate_t *c,
                                   double max_mean_absolute_error,
                                   size_t *sample_count_out,
                                   double *mean_absolute_error_out,
                                   double *max_absolute_error_out);

/**
 * @brief Save a complete analysis session as JSON.
 * @param s      Session to write.
 * @param path   Destination file path.
 * @param err    Optional error buffer.
 * @param errsz  Size of @p err.
 * @return 0 on success, -1 on failure.
 */
int ba_session_save_json(const ba_session_t *s,
                         const char *path,
                         char *err,
                         size_t errsz);

/**
 * @brief Load an analysis session from JSON.
 * @param s      Destination session; replaced on success.
 * @param path   Source file path.
 * @param err    Optional error buffer.
 * @param errsz  Size of @p err.
 * @return 0 on success, -1 on failure.
 */
int ba_session_load_json(ba_session_t *s,
                         const char *path,
                         char *err,
                         size_t errsz);

/** @brief Convert a POSIX timespec to nanoseconds. */
int64_t ba_timespec_to_ns(const struct timespec *ts);

/** @brief Return one canonical LSB0 bit from a payload buffer. */
uint8_t ba_get_bit(const uint8_t *data, uint16_t bit);

/**
 * @brief Extract a raw unsigned value from canonical coordinates.
 * @param data                 Payload bytes.
 * @param dlc                  Payload length in bytes.
 * @param canonical_start_bit  Byte0/bit0 absolute start coordinate.
 * @param bit_length           Field length in bits.
 * @param order                Intel or Motorola byte order.
 * @return Extracted raw value, or 0 when the field does not fit.
 */
uint64_t ba_extract_unsigned(const uint8_t *data,
                             uint8_t dlc,
                             uint16_t canonical_start_bit,
                             uint8_t bit_length,
                             ba_byte_order_t order);

/**
 * @brief Extract a raw unsigned value from DBC start-bit coordinates.
 */
uint64_t ba_extract_unsigned_dbc(const uint8_t *data,
                                 uint8_t dlc,
                                 uint16_t dbc_start_bit,
                                 uint8_t bit_length,
                                 ba_byte_order_t order);

/** @brief Sign-extend an unsigned raw value as two's-complement. */
int64_t ba_sign_extend(uint64_t raw, uint8_t bit_length);

/** @brief Convert a canonical start bit to the corresponding DBC start bit. */
uint16_t ba_canonical_to_dbc_start_bit(uint16_t canonical_start_bit,
                                       uint8_t bit_length,
                                       ba_byte_order_t order,
                                       uint16_t total_bits);

/** @brief Convert a DBC start bit to the canonical absolute start bit. */
uint16_t ba_dbc_to_canonical_start_bit(uint16_t dbc_start_bit,
                                       uint8_t bit_length,
                                       ba_byte_order_t order,
                                       uint16_t total_bits);

/** @brief Return non-zero when a field fits inside the selected bit range. */
int ba_field_fits(uint16_t start_bit,
                  uint8_t bit_length,
                  ba_byte_order_t order,
                  uint16_t total_bits);

/** @brief Human-readable byte-order label. */
const char *ba_byte_order_name(ba_byte_order_t order);

/** @brief Human-readable segment-type label. */
const char *ba_segment_type_name(ba_segment_type_t type);

/** @brief Human-readable input-type label. */
const char *ba_input_type_name(ba_input_type_t type);

/** @brief Human-readable session-state label. */
const char *ba_session_state_name(ba_session_state_t state);

#endif /* BIT_ANALYSIS_H */
