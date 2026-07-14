/**
 * @file test_bit_analysis.c
 * @brief Focused regression tests for the Bit Analysis engine and DBC promotion.
 *
 * @details
 * Builds a set of deterministic synthetic CAN traces that exercise bit
 * extraction, target filtering, baseline capture, candidate scanning,
 * signedness, Motorola conversion, lag detection, counter/checksum/timestamp
 * heuristics, multiplexer evidence, validation segments, JSON persistence, and
 * DBC round-trips.  The test binary is intentionally separate from the GTK UI
 * so it can run in CI and on development machines without CAN hardware.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../inc/bit_analysis.h"
#include "../inc/dbc.h"

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void put_u64_le(uint8_t out[CANFD_DATA_MAX], uint64_t v)
{
    memset(out, 0, CANFD_DATA_MAX);
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)((v >> (i * 8)) & 0xffu);
}

static void put_motorola(uint8_t out[CANFD_DATA_MAX],
                         uint16_t dbc_start_bit,
                         uint8_t bit_length,
                         uint64_t value)
{
    memset(out, 0, CANFD_DATA_MAX);
    int bit = dbc_start_bit;
    for (uint8_t i = 0; i < bit_length; i++) {
        if ((value >> (bit_length - 1u - i)) & 1u)
            out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        if ((bit & 7) == 0)
            bit += 15;
        else
            bit -= 1;
    }
}

static void put_candidate_raw(uint8_t out[CANFD_DATA_MAX],
                              const ba_candidate_t *candidate,
                              uint64_t value)
{
    memset(out, 0, CANFD_DATA_MAX);
    if (!candidate)
        return;
    if (candidate->byte_order == BA_BYTE_ORDER_INTEL) {
        for (uint8_t i = 0; i < candidate->bit_length; i++) {
            if ((value >> i) & 1u) {
                uint16_t bit = (uint16_t)(candidate->dbc_start_bit + i);
                out[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            }
        }
    } else {
        put_motorola(out, candidate->dbc_start_bit,
                     candidate->bit_length, value);
    }
}

static can_msg_t make_msg(uint32_t id,
                          int is_extended,
                          uint8_t dlc,
                          uint8_t direction,
                          uint64_t seq)
{
    can_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = id;
    msg.is_extended = is_extended ? 1u : 0u;
    msg.dlc = dlc;
    msg.direction = direction;
    msg.seq = seq;
    msg.timestamp.tv_sec = (time_t)(seq / 1000u);
    msg.timestamp.tv_nsec = (long)(seq % 1000u) * 1000000L;
    for (uint8_t i = 0; i < dlc && i < CANFD_DATA_MAX; i++)
        msg.data[i] = (uint8_t)(seq + i);
    return msg;
}

static void extraction_tests(void)
{
    uint8_t data[CANFD_DATA_MAX] = {0xAC, 0x12, 0x34};
    check(ba_extract_unsigned(data, 3, 0, 8, BA_BYTE_ORDER_INTEL) == 0xAC,
          "Intel one-byte extraction");
    check(ba_extract_unsigned(data, 3, 4, 12, BA_BYTE_ORDER_INTEL) == 0x12A,
          "Intel cross-byte extraction");

    uint8_t be[CANFD_DATA_MAX] = {0x12, 0x34};
    check(ba_dbc_to_canonical_start_bit(7, 16, BA_BYTE_ORDER_MOTOROLA, 16) == 0,
          "Motorola canonical conversion");
    check(ba_canonical_to_dbc_start_bit(0, 16, BA_BYTE_ORDER_MOTOROLA, 16) == 7,
          "Motorola DBC conversion");
    check(ba_extract_unsigned(be, 2, 0, 16, BA_BYTE_ORDER_MOTOROLA) == 0x1234,
          "Motorola cross-byte extraction");

    uint8_t be_nibble[CANFD_DATA_MAX] = {0xAB};
    check(ba_extract_unsigned(be_nibble, 1, 0, 4, BA_BYTE_ORDER_MOTOROLA) == 0xB,
          "Motorola non-aligned nibble");

    check(ba_sign_extend(0xff, 8) == -1, "signed 8-bit -1");
    check(ba_sign_extend(0x800, 12) == -2048, "signed 12-bit minimum");
    check(ba_sign_extend(0x7ff, 12) == 2047, "signed 12-bit maximum");
}

static void target_filter_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 3) == 0, "filter session init");
    ba_session_configure_target(&s, 0x123, 0, 8);

    can_msg_t msg = make_msg(0x123, 0, 8, CAN_DIR_RX, 1);
    check(ba_session_push_frame(&s, &msg) == 0,
          "ready session does not capture");

    s.state = BA_SESSION_CAPTURING;
    check(s.include_rx == 1 && s.include_tx == 0 &&
          s.accept_varying_dlc == 0 && s.analyze_fd_payload == 0,
          "default target filter settings");

    check(ba_session_push_frame(&s, &msg) == 1,
          "matching RX frame captured");
    check(s.sample_count == 1 && s.total_target_frames == 1,
          "matching RX frame counted");

    msg = make_msg(0x123, 0, 8, CAN_DIR_TX, 2);
    check(ba_session_push_frame(&s, &msg) == 0,
          "TX frame rejected by default");
    s.include_tx = 1;
    check(ba_session_push_frame(&s, &msg) == 1,
          "TX frame captured after opt-in");

    msg = make_msg(0x123, 0, 7, CAN_DIR_RX, 3);
    check(ba_session_push_frame(&s, &msg) == 0,
          "unexpected DLC rejected by default");
    s.accept_varying_dlc = 1;
    check(ba_session_push_frame(&s, &msg) == 1,
          "varying DLC captured after opt-in");

    msg = make_msg(0x123, 0, 8, CAN_DIR_RX, 4);
    msg.is_error = 1;
    check(ba_session_push_frame(&s, &msg) == 0,
          "error frame rejected");
    msg.is_error = 0;
    msg.is_remote = 1;
    check(ba_session_push_frame(&s, &msg) == 0,
          "RTR frame rejected");

    msg = make_msg(0x124, 0, 8, CAN_DIR_RX, 5);
    check(ba_session_push_frame(&s, &msg) == 0,
          "wrong CAN ID rejected");

    ba_session_configure_target(&s, 0x1abcde, 1, 8);
    s.state = BA_SESSION_CAPTURING;
    s.accept_varying_dlc = 0;
    msg = make_msg(0x1abcde, 0, 8, CAN_DIR_RX, 6);
    check(ba_session_push_frame(&s, &msg) == 0,
          "standard frame rejected for extended target");
    msg = make_msg(0x1abcde, 1, 8, CAN_DIR_RX, 7);
    check(ba_session_push_frame(&s, &msg) == 1,
          "extended target frame captured");

    ba_session_configure_target(&s, 0x456, 0, 12);
    s.state = BA_SESSION_CAPTURING;
    msg = make_msg(0x456, 0, 12, CAN_DIR_RX, 8);
    msg.is_fd = 1;
    check(ba_session_push_frame(&s, &msg) == 0,
          "FD payload rejected when classic analysis is active");
    s.analyze_fd_payload = 1;
    check(ba_session_push_frame(&s, &msg) == 1,
          "FD payload captured after opt-in");

    ba_session_configure_target(&s, 0x555, 0, 8);
    s.state = BA_SESSION_CAPTURING;
    for (uint64_t seq = 10; seq < 15; seq++) {
        msg = make_msg(0x555, 0, 8, CAN_DIR_RX, seq);
        check(ba_session_push_frame(&s, &msg) == 1,
              "ring test frame captured");
    }
    check(s.sample_count == 3, "ring buffer caps sample count");
    check(s.dropped_samples == 2, "ring buffer tracks overwritten samples");
    check(s.total_target_frames == 5, "ring buffer tracks accepted frames");
    check(s.latest_dlc == 8 && s.latest_data[0] == 14,
          "latest payload remains available after overwrite");

    ba_session_destroy(&s);
}

static void validate_candidate_dbc(const ba_candidate_t *candidate)
{
    check(candidate != NULL, "candidate required for DBC validation");
    if (!candidate)
        return;

    char path[] = "/tmp/canoscope-bit-analysis-dbc-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp for validation dbc");
    if (fd >= 0)
        close(fd);

    char err[256] = {0};
    dbc_db_t *db = dbc_create_empty(path);
    check(db != NULL, "create validation dbc");
    if (!db) {
        unlink(path);
        return;
    }

    dbc_message_t *msg = dbc_upsert_message(db, 0x100, 0, 8,
                                            "ReverseEngineeredMsg",
                                            err, sizeof(err));
    check(msg != NULL, "upsert validation dbc message");

    dbc_signal_t sig;
    memset(&sig, 0, sizeof(sig));
    snprintf(sig.name, sizeof(sig.name), "Analog");
    sig.start_bit = candidate->dbc_start_bit;
    sig.length = candidate->bit_length;
    sig.little_endian = candidate->byte_order == BA_BYTE_ORDER_INTEL ? 1u : 0u;
    sig.is_signed = candidate->is_signed;
    sig.factor = candidate->factor;
    sig.offset = candidate->offset;
    sig.min = candidate->physical_min;
    sig.max = candidate->physical_max;
    snprintf(sig.unit, sizeof(sig.unit), "%s", "%");
    const char *expected_comment =
        "Reverse engineered with CANoScope Bit Analysis. "
        "Confidence: \"Validated\".\nEvidence path C:\\canoscope";
    snprintf(sig.comment, sizeof(sig.comment), "%s", expected_comment);

    check(msg && dbc_upsert_signal(db, msg, &sig, NULL,
                                   err, sizeof(err)) == 0,
          "upsert validation dbc signal");
    check(dbc_save_file(db, path, err, sizeof(err)) == 0,
          "save validation dbc");
    dbc_free(db);

    dbc_db_t *loaded = dbc_load_file(path, err, sizeof(err));
    check(loaded != NULL, "reload validation dbc");
    const dbc_message_t *loaded_msg =
        loaded ? dbc_find_message(loaded, 0x100, 0) : NULL;
    check(loaded_msg && loaded_msg->signal_count == 1,
          "find validation dbc message/signal");
    const dbc_signal_t *loaded_sig =
        loaded_msg && loaded_msg->signal_count ? &loaded_msg->signals[0] : NULL;
    check(loaded_sig &&
          strstr(loaded_sig->comment, "CANoScope Bit Analysis") != NULL,
          "loaded validation dbc signal comment");
    check(loaded_sig && strcmp(loaded_sig->comment, expected_comment) == 0,
          "loaded validation dbc signal comment escapes");

    const uint16_t validation_raws[] = {13u, 481u, 913u, 2507u, 4095u};
    for (size_t i = 0; loaded_sig &&
         i < sizeof(validation_raws) / sizeof(validation_raws[0]); i++) {
        uint64_t payload = (uint64_t)validation_raws[i] << 16;
        uint8_t data[CANFD_DATA_MAX];
        put_u64_le(data, payload);
        uint64_t raw = dbc_extract_raw(data, 8, loaded_sig);
        int64_t signed_raw = 0;
        double phys = dbc_decode_physical(loaded_sig, raw, &signed_raw);
        double expected = (double)validation_raws[i] * 0.1 + 2.0;
        check(raw == validation_raws[i],
              "validation DBC raw decode mismatch");
        check(fabs(phys - expected) < 1e-9,
              "validation DBC physical decode mismatch");
    }

    dbc_free(loaded);
    unlink(path);
}

static void validate_candidate_dbc_roundtrip(const ba_candidate_t *candidate,
                                             uint32_t message_id,
                                             int is_extended,
                                             uint8_t dlc,
                                             const uint64_t *raw_values,
                                             size_t raw_count)
{
    check(candidate != NULL, "candidate required for generic DBC validation");
    if (!candidate)
        return;

    char path[] = "/tmp/canoscope-bit-analysis-generic-dbc-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp for generic validation dbc");
    if (fd >= 0)
        close(fd);

    char err[256] = {0};
    dbc_db_t *db = dbc_create_empty(path);
    check(db != NULL, "create generic validation dbc");
    if (!db) {
        unlink(path);
        return;
    }

    dbc_message_t *msg = dbc_upsert_message(db, message_id, is_extended, dlc,
                                            "ReverseEngineeredMsg",
                                            err, sizeof(err));
    check(msg != NULL, "upsert generic validation dbc message");

    dbc_signal_t sig;
    memset(&sig, 0, sizeof(sig));
    snprintf(sig.name, sizeof(sig.name), "%.*s",
             (int)sizeof(sig.name) - 1, candidate->proposed_name);
    sig.start_bit = candidate->dbc_start_bit;
    sig.length = candidate->bit_length;
    sig.little_endian = candidate->byte_order == BA_BYTE_ORDER_INTEL ? 1u : 0u;
    sig.is_signed = candidate->is_signed;
    sig.factor = candidate->factor;
    sig.offset = candidate->offset;
    sig.min = candidate->physical_min;
    sig.max = candidate->physical_max;
    snprintf(sig.unit, sizeof(sig.unit), "%.*s",
             (int)sizeof(sig.unit) - 1, candidate->unit);
    snprintf(sig.comment, sizeof(sig.comment),
             "Reverse engineered with CANoScope Bit Analysis. Evidence round trip.");

    check(msg && dbc_upsert_signal(db, msg, &sig, NULL,
                                   err, sizeof(err)) == 0,
          "upsert generic validation dbc signal");
    check(dbc_save_file(db, path, err, sizeof(err)) == 0,
          "save generic validation dbc");
    dbc_free(db);

    dbc_db_t *loaded = dbc_load_file(path, err, sizeof(err));
    check(loaded != NULL, "reload generic validation dbc");
    const dbc_message_t *loaded_msg =
        loaded ? dbc_find_message(loaded, message_id, is_extended) : NULL;
    check(loaded_msg && loaded_msg->signal_count == 1,
          "find generic validation dbc message/signal");
    const dbc_signal_t *loaded_sig =
        loaded_msg && loaded_msg->signal_count ? &loaded_msg->signals[0] : NULL;
    check(loaded_sig &&
          strstr(loaded_sig->comment, "Evidence round trip") != NULL,
          "loaded generic validation dbc signal comment");

    for (size_t i = 0; loaded_sig && i < raw_count; i++) {
        uint8_t data[CANFD_DATA_MAX];
        put_candidate_raw(data, candidate, raw_values[i]);
        uint64_t raw = dbc_extract_raw(data, dlc, loaded_sig);
        int64_t signed_raw = 0;
        double phys = dbc_decode_physical(loaded_sig, raw, &signed_raw);
        double raw_for_phys = candidate->is_signed ?
            (double)ba_sign_extend(raw_values[i], candidate->bit_length) :
            (double)raw_values[i];
        double expected = candidate->factor * raw_for_phys + candidate->offset;
        check(raw == raw_values[i], "generic validation DBC raw decode mismatch");
        check(fabs(phys - expected) < 1e-9,
              "generic validation DBC physical decode mismatch");
    }

    dbc_free(loaded);
    unlink(path);
}

static void candidate_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 512) == 0, "session init");
    ba_session_configure_target(&s, 0x100, 0, 8);
    s.state = BA_SESSION_CAPTURING;
    check(ba_session_add_segment(&s, BA_SEGMENT_STEP, "step sweep",
                                 0.0, 1, 0) != 0,
          "add segment");

    for (int i = 0; i < 200; i++) {
        uint16_t analog_raw = (uint16_t)(i * 20);     /* bits 16..27 */
        uint8_t counter8 = (uint8_t)i;                /* bits 32..39 */
        uint8_t counter = (uint8_t)(i & 0x0f);        /* bits 52..55 */
        uint64_t payload = ((uint64_t)analog_raw << 16) |
                           ((uint64_t)counter8 << 32) |
                           ((uint64_t)counter << 52);

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x100;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "Analog",
                             (double)analog_raw * 0.1 + 2.0, "%", 1);
        check(ba_session_push_frame(&s, &msg) == 1, "push frame");
    }

    ba_session_end_segment(&s, 0);
    ba_session_capture_baseline(&s);
    ba_session_analyze(&s, 16);

    int found_analog = 0;
    int found_counter = 0;
    int found_counter8 = 0;
    ba_candidate_t analog_candidate;
    memset(&analog_candidate, 0, sizeof(analog_candidate));
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 16 &&
            c->bit_length == 12 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            c->r_squared > 0.999 &&
            fabs(c->factor - 0.1) < 1e-6 &&
            fabs(c->offset - 2.0) < 1e-6) {
            found_analog = 1;
            analog_candidate = *c;
        }
        if (c->canonical_start_bit == 52 &&
            c->bit_length == 4 &&
            c->counter_score > 0.99) {
            found_counter = 1;
        }
        if (c->canonical_start_bit == 32 &&
            c->bit_length == 8 &&
            c->counter_score > 0.99) {
            found_counter8 = 1;
        }
    }

    check(found_analog, "known 12-bit Intel analog candidate not found");
    validate_candidate_dbc(found_analog ? &analog_candidate : NULL);
    check(found_counter, "known 4-bit alive counter candidate not found");
    check(found_counter8, "known 8-bit alive counter candidate not found");
    check(s.bit_stats[18].sample_count == 200, "bit stat sample count");
    check(s.bit_stats[18].entropy_bits > 0.0, "bit entropy was computed");
    check(s.segment_count == 1 && s.segments[0].sample_count == 200,
          "segment frame count");

    if (s.candidate_count > 0) {
        snprintf(s.candidates[0].confidence,
                 sizeof(s.candidates[0].confidence), "Validated");
        snprintf(s.candidates[0].notes, sizeof(s.candidates[0].notes),
                 "round-trip evidence");
    }

    char path[] = "/tmp/canoscope-bit-analysis-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp for session json");
    if (fd >= 0)
        close(fd);

    char err[256] = {0};
    check(ba_session_save_json(&s, path, err, sizeof(err)) == 0,
          "save session json");

    ba_session_t loaded;
    check(ba_session_init(&loaded, 1) == 0, "loaded session init");
    check(ba_session_load_json(&loaded, path, err, sizeof(err)) == 0,
          "load session json");
    check(loaded.can_id == 0x100 && loaded.expected_dlc == 8,
          "loaded target metadata");
    check(loaded.sample_count == 200, "loaded sample count");
    check(loaded.segment_count == 1 &&
          strcmp(loaded.segments[0].label, "step sweep") == 0 &&
          loaded.segments[0].sample_count == 200,
          "loaded segment metadata");
    check(loaded.baseline_valid, "loaded baseline");
    check(loaded.baseline_sample_count == s.baseline_sample_count,
          "loaded baseline sample count");
    check(loaded.candidate_count == s.candidate_count,
          "loaded candidate count");
    if (loaded.candidate_count > 0) {
        check(strcmp(loaded.candidates[0].confidence, "Validated") == 0,
              "loaded candidate confidence");
        check(strcmp(loaded.candidates[0].notes,
                     "round-trip evidence") == 0,
              "loaded candidate notes");
    }
    check(loaded.latest_dlc == 8, "loaded latest payload metadata");
    check(ba_extract_unsigned(loaded.latest_data, loaded.latest_dlc,
                              16, 12, BA_BYTE_ORDER_INTEL) ==
          (uint64_t)(199 * 20),
          "loaded latest payload data");

    ba_session_destroy(&loaded);
    unlink(path);

    ba_session_destroy(&s);
}

static void baseline_segment_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 128) == 0, "baseline segment session init");
    ba_session_configure_target(&s, 0x120, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    check(ba_session_add_segment(&s, BA_SEGMENT_BASELINE,
                                 "stable baseline", 0.0, 1, 0) != 0,
          "add baseline segment");
    for (int i = 0; i < 30; i++) {
        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x120;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, 0);
        ba_session_set_input(&s, BA_INPUT_BOOLEAN, "Switch", 0.0, "", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push baseline segment frame");
    }
    ba_session_end_segment(&s, 0);

    check(ba_session_add_segment(&s, BA_SEGMENT_STEP,
                                 "changed state", 1.0, 1, 0) != 0,
          "add changed segment");
    for (int i = 30; i < 100; i++) {
        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x120;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, 1);
        ba_session_set_input(&s, BA_INPUT_BOOLEAN, "Switch", 1.0, "", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push changed segment frame");
    }
    ba_session_end_segment(&s, 0);

    ba_session_capture_baseline(&s);
    check(s.baseline_valid, "baseline segment capture valid");
    check(s.baseline_sample_count == 30,
          "baseline capture used only baseline segment frames");
    check(s.baseline_bits[0] == 0,
          "baseline segment ignored later changed frames");

    ba_session_analyze(&s, 16);
    check(fabs(s.bit_stats[0].baseline_difference_rate - 0.70) < 1e-9,
          "baseline difference rate uses scoped baseline");

    ba_session_destroy(&s);
}

static void validation_segment_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 256) == 0, "validation session init");
    ba_session_configure_target(&s, 0x141, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    check(ba_session_add_segment(&s, BA_SEGMENT_STEP,
                                 "fit sweep", 0.0, 1, 0) != 0,
          "add validation fit segment");
    for (int i = 0; i < 120; i++) {
        uint16_t raw = (uint16_t)(i * 23u);
        uint64_t payload = (uint64_t)raw << 16;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x141;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "Analog",
                             (double)raw * 0.2 + 5.0, "u", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push validation fit frame");
    }
    ba_session_end_segment(&s, 0);
    ba_session_analyze(&s, 16);

    ba_candidate_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    int found = 0;
    size_t candidate_index = 0;
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 16 &&
            c->bit_length == 12 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            c->r_squared > 0.999) {
            candidate = *c;
            candidate_index = i;
            found = 1;
            break;
        }
    }
    check(found, "validation candidate found from fit segment");

    size_t validation_n = 99;
    double mae = 99.0, maxe = 99.0;
    check(ba_candidate_validate_segments(&s, &candidate, 1e-6,
                                         &validation_n, &mae, &maxe) == 0 &&
          validation_n == 0,
          "validation fails without validation segment");

    s.state = BA_SESSION_CAPTURING;
    check(ba_session_add_segment(&s, BA_SEGMENT_VALIDATION,
                                 "random validation", 0.0, 1, 0) != 0,
          "add validation segment");
    for (int i = 120; i < 180; i++) {
        uint16_t raw = (uint16_t)(((uint32_t)i * 977u + 113u) & 0x0fffu);
        uint64_t payload = (uint64_t)raw << 16;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x141;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "Analog",
                             (double)raw * 0.2 + 5.0, "u", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push validation frame");
    }
    ba_session_end_segment(&s, 0);

    check(ba_candidate_validate_segments(&s, &candidate, 1e-6,
                                         &validation_n, &mae, &maxe) == 1,
          "validation passes on independent segment");
    check(validation_n == 60 && mae < 1e-9 && maxe < 1e-9,
          "validation metrics are exact");
    if (found) {
        ba_candidate_t *stored = &s.candidates[candidate_index];
        snprintf(stored->confidence, sizeof(stored->confidence), "Validated");
        stored->validation_sample_count = (uint32_t)validation_n;
        stored->validation_mean_absolute_error = mae;
        stored->validation_max_absolute_error = maxe;

        char path[] = "/tmp/canoscope-bit-analysis-validation-XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for validation evidence json");
        if (fd >= 0)
            close(fd);
        char err[256] = {0};
        check(ba_session_save_json(&s, path, err, sizeof(err)) == 0,
              "save validation evidence json");

        ba_session_t loaded;
        check(ba_session_init(&loaded, 1) == 0,
              "loaded validation evidence session init");
        check(ba_session_load_json(&loaded, path, err, sizeof(err)) == 0,
              "load validation evidence json");
        unlink(path);

        int loaded_validation = 0;
        for (size_t i = 0; i < loaded.candidate_count; i++) {
            const ba_candidate_t *c = &loaded.candidates[i];
            if (c->canonical_start_bit == 16 &&
                c->bit_length == 12 &&
                c->byte_order == BA_BYTE_ORDER_INTEL &&
                !c->is_signed &&
                strcmp(c->confidence, "Validated") == 0) {
                check(c->validation_sample_count == 60,
                      "loaded validation sample count");
                check(c->validation_mean_absolute_error < 1e-9,
                      "loaded validation MAE");
                check(c->validation_max_absolute_error < 1e-9,
                      "loaded validation max error");
                loaded_validation = 1;
                break;
            }
        }
        check(loaded_validation, "loaded validated candidate found");
        ba_session_destroy(&loaded);
    }

    ba_candidate_t bad = candidate;
    bad.offset += 2.0;
    check(ba_candidate_validate_segments(&s, &bad, 0.5,
                                         &validation_n, &mae, &maxe) == 0 &&
          validation_n == 60 && mae > 1.0,
          "validation rejects bad candidate fit");

    ba_session_destroy(&s);
}

static void transition_match_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 256) == 0, "transition session init");
    ba_session_configure_target(&s, 0x151, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < 200; i++) {
        int state = (i / 20) & 1;
        uint64_t payload = state ? (1ull << 24) : 0ull;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x151;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_BOOLEAN, "Switch",
                             (double)state, "", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push transition frame");
    }

    ba_session_analyze(&s, 8);

    ba_candidate_t transition_candidate;
    memset(&transition_candidate, 0, sizeof(transition_candidate));
    int found = 0;
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 24 &&
            c->bit_length == 1 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed) {
            transition_candidate = *c;
            found = 1;
            break;
        }
    }

    check(found, "known transition bit candidate found");
    if (found) {
        check(transition_candidate.transition_total == 9,
              "transition total counted");
        check(transition_candidate.transition_matches == 9,
              "transition matches counted");
        check(transition_candidate.state_separation > 0.99,
              "transition bit has state separation");

        char path[] = "/tmp/canoscope-bit-analysis-transition-XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for transition session json");
        if (fd >= 0)
            close(fd);
        char err[256] = {0};
        check(ba_session_save_json(&s, path, err, sizeof(err)) == 0,
              "save transition session json");

        ba_session_t loaded;
        check(ba_session_init(&loaded, 1) == 0,
              "loaded transition session init");
        check(ba_session_load_json(&loaded, path, err, sizeof(err)) == 0,
              "load transition session json");
        unlink(path);

        int loaded_transition = 0;
        for (size_t i = 0; i < loaded.candidate_count; i++) {
            const ba_candidate_t *c = &loaded.candidates[i];
            if (c->canonical_start_bit == 24 &&
                c->bit_length == 1 &&
                c->byte_order == BA_BYTE_ORDER_INTEL &&
                !c->is_signed) {
                check(c->transition_total ==
                      transition_candidate.transition_total,
                      "loaded transition total");
                check(c->transition_matches ==
                      transition_candidate.transition_matches,
                      "loaded transition matches");
                loaded_transition = 1;
                break;
            }
        }
        check(loaded_transition, "loaded transition candidate found");
        ba_session_destroy(&loaded);
    }

    ba_session_destroy(&s);
}

static void checksum_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 512) == 0, "checksum session init");
    ba_session_configure_target(&s, 0x321, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < 256; i++) {
        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x321;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        msg.data[0] = (uint8_t)(i * 3u);
        msg.data[1] = (uint8_t)(i * 5u + 7u);
        msg.data[2] = (uint8_t)(i ^ 0xa5u);
        msg.data[3] = (uint8_t)((i >> 1) ^ 0x3cu);
        msg.data[4] = (uint8_t)(i * 11u + 9u);
        msg.data[5] = (uint8_t)(i * 13u + 1u);
        msg.data[6] = (uint8_t)((i * 17u) ^ (i >> 2));
        msg.data[7] = msg.data[0] ^ msg.data[1] ^ msg.data[2] ^
                      msg.data[3] ^ msg.data[4] ^ msg.data[5] ^
                      msg.data[6];
        check(ba_session_push_frame(&s, &msg) == 1, "push checksum frame");
    }

    ba_session_analyze(&s, 16);

    int found_checksum = 0;
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 56 &&
            c->bit_length == 8 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            c->checksum_score >= 0.75) {
            check(strcmp(c->confidence, "Checksum-like") == 0,
                  "checksum confidence is not overclaimed");
            found_checksum = 1;
            break;
        }
    }
    check(found_checksum, "known checksum-like byte not found");

    ba_session_destroy(&s);
}

static void mux_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 256) == 0, "mux session init");
    ba_session_configure_target(&s, 0x410, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < 200; i++) {
        uint8_t mux = (uint8_t)((i / 10) & 1);
        uint8_t mode0_value = (uint8_t)(i * 13u + 7u);
        uint8_t mode1_value = (uint8_t)(i * 17u + 3u);
        uint64_t payload = mux;
        if (mux)
            payload |= (uint64_t)mode1_value << 32;
        else
            payload |= (uint64_t)mode0_value << 16;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x410;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);
        check(ba_session_push_frame(&s, &msg) == 1, "push mux frame");
    }

    ba_session_analyze(&s, 8);

    int found_mux = 0;
    double best_bit0_mux = -1.0;
    double best_bit0_score = -1.0;
    char best_bit0_conf[24] = {0};
    ba_candidate_t mux_candidate;
    memset(&mux_candidate, 0, sizeof(mux_candidate));
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 0 &&
            c->bit_length == 1 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed) {
            best_bit0_mux = c->mux_score;
            best_bit0_score = c->total_score;
            snprintf(best_bit0_conf, sizeof(best_bit0_conf), "%s",
                     c->confidence);
        }
        if (c->canonical_start_bit == 0 &&
            c->bit_length == 1 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            c->mux_score >= 0.75) {
            check(strcmp(c->confidence, "Mux-like") == 0,
                  "mux confidence is classified");
            mux_candidate = *c;
            found_mux = 1;
            break;
        }
    }
    if (!found_mux) {
        fprintf(stderr, "bit0 mux %.6f score %.6f confidence %s candidates %zu\n",
                best_bit0_mux, best_bit0_score, best_bit0_conf,
                s.candidate_count);
    }
    check(found_mux, "known one-bit mux selector not found");
    if (found_mux) {
        check(mux_candidate.mux_unique_values == 2,
              "mux unique selector values tracked");
        check(mux_candidate.mux_min_frames_per_value == 100,
              "mux minimum frames per selector tracked");
        check(mux_candidate.mux_conditionally_active_bits >= 12,
              "mux conditionally active bit count tracked");
        check(strstr(mux_candidate.mux_active_ranges, "16") != NULL &&
              strstr(mux_candidate.mux_active_ranges, "32") != NULL,
              "mux active ranges tracked");

        char path[] = "/tmp/canoscope-bit-analysis-mux-XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for mux session json");
        if (fd >= 0)
            close(fd);
        char err[256] = {0};
        check(ba_session_save_json(&s, path, err, sizeof(err)) == 0,
              "save mux session json");

        ba_session_t loaded;
        check(ba_session_init(&loaded, 1) == 0, "loaded mux session init");
        check(ba_session_load_json(&loaded, path, err, sizeof(err)) == 0,
              "load mux session json");
        unlink(path);

        int loaded_mux = 0;
        for (size_t i = 0; i < loaded.candidate_count; i++) {
            const ba_candidate_t *c = &loaded.candidates[i];
            if (c->canonical_start_bit == 0 &&
                c->bit_length == 1 &&
                c->byte_order == BA_BYTE_ORDER_INTEL &&
                !c->is_signed &&
                c->mux_score >= 0.75) {
                check(c->mux_unique_values == mux_candidate.mux_unique_values,
                      "loaded mux unique values");
                check(c->mux_min_frames_per_value ==
                      mux_candidate.mux_min_frames_per_value,
                      "loaded mux minimum frames");
                check(c->mux_conditionally_active_bits ==
                      mux_candidate.mux_conditionally_active_bits,
                      "loaded mux active bit count");
                check(strcmp(c->mux_active_ranges,
                             mux_candidate.mux_active_ranges) == 0,
                      "loaded mux active ranges");
                loaded_mux = 1;
                break;
            }
        }
        check(loaded_mux, "loaded mux candidate found");
        ba_session_destroy(&loaded);
    }

    ba_session_destroy(&s);
}

static void timestamp_tests(void)
{
    ba_session_t s;
    check(ba_session_init(&s, 256) == 0, "timestamp session init");
    ba_session_configure_target(&s, 0x420, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < 180; i++) {
        uint16_t raw_time = (uint16_t)(i * 5u);
        uint64_t payload = (uint64_t)raw_time << 8;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x420;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push timestamp frame");
    }

    ba_session_analyze(&s, 16);

    int found_timestamp = 0;
    ba_candidate_t timestamp_candidate;
    memset(&timestamp_candidate, 0, sizeof(timestamp_candidate));
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 8 &&
            c->bit_length == 16 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            c->timestamp_score >= 0.85) {
            check(strcmp(c->confidence, "Timestamp-like") == 0,
                  "timestamp confidence is classified");
            check(c->counter_score < 0.10,
                  "timestamp is not treated as unit alive counter");
            check(fabs(c->timestamp_resolution_ms - 2.0) < 0.05,
                  "timestamp resolution estimated");
            check(c->timestamp_wrap_value == 65536,
                  "timestamp wrap value tracked");
            timestamp_candidate = *c;
            found_timestamp = 1;
            break;
        }
    }
    check(found_timestamp, "known timestamp-like field not found");

    if (found_timestamp) {
        char path[] = "/tmp/canoscope-bit-analysis-timestamp-XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for timestamp session json");
        if (fd >= 0)
            close(fd);
        char err[256] = {0};
        check(ba_session_save_json(&s, path, err, sizeof(err)) == 0,
              "save timestamp session json");

        ba_session_t loaded;
        check(ba_session_init(&loaded, 1) == 0,
              "loaded timestamp session init");
        check(ba_session_load_json(&loaded, path, err, sizeof(err)) == 0,
              "load timestamp session json");
        unlink(path);

        int loaded_timestamp = 0;
        for (size_t i = 0; i < loaded.candidate_count; i++) {
            const ba_candidate_t *c = &loaded.candidates[i];
            if (c->canonical_start_bit == 8 &&
                c->bit_length == 16 &&
                c->byte_order == BA_BYTE_ORDER_INTEL &&
                !c->is_signed &&
                c->timestamp_score >= 0.85) {
                check(fabs(c->timestamp_score -
                           timestamp_candidate.timestamp_score) < 1e-12,
                      "loaded timestamp score");
                check(fabs(c->timestamp_resolution_ms -
                           timestamp_candidate.timestamp_resolution_ms) < 1e-12,
                      "loaded timestamp resolution");
                check(c->timestamp_wrap_value ==
                      timestamp_candidate.timestamp_wrap_value,
                      "loaded timestamp wrap value");
                loaded_timestamp = 1;
                break;
            }
        }
        check(loaded_timestamp, "loaded timestamp candidate found");
        ba_session_destroy(&loaded);
    }

    ba_session_destroy(&s);
}

static void signed_candidate_tests(void)
{
    enum { N = 240 };
    ba_session_t s;
    check(ba_session_init(&s, N) == 0, "signed candidate session init");
    ba_session_configure_target(&s, 0x130, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < N; i++) {
        int16_t sv = (int16_t)(-1000 + (2000 * i) / (N - 1));
        uint16_t raw12 = (uint16_t)sv & 0x0fffu;
        uint64_t payload = (uint64_t)raw12 << 20;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x130;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "SignedCrossZero",
                             (double)sv * 0.5 - 3.0, "u", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push signed candidate frame");
    }

    ba_session_analyze(&s, 16);

    int found_signed = 0;
    double signed_score = -1.0;
    double unsigned_score = -1.0;
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 20 &&
            c->bit_length == 12 &&
            c->byte_order == BA_BYTE_ORDER_INTEL) {
            if (c->is_signed) {
                signed_score = c->total_score;
                if (c->r_squared > 0.999 &&
                    fabs(c->factor - 0.5) < 1e-6 &&
                    fabs(c->offset + 3.0) < 1e-6)
                    found_signed = 1;
            } else {
                unsigned_score = c->total_score;
            }
        }
    }

    check(found_signed, "known 12-bit signed candidate not found");
    check(unsigned_score < 0.0 || signed_score > unsigned_score,
          "signed candidate does not outrank unsigned interpretation");

    ba_session_destroy(&s);
}

static void motorola_candidate_tests(void)
{
    enum { N = 220 };
    ba_session_t s;
    check(ba_session_init(&s, N) == 0, "motorola candidate session init");
    ba_session_configure_target(&s, 0x131, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    uint32_t rng = 0xace1u;
    for (int i = 0; i < N; i++) {
        rng ^= rng << 7;
        rng ^= rng >> 9;
        rng ^= rng << 8;
        uint16_t raw = (uint16_t)rng;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x131;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_motorola(msg.data, 23, 16, raw);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "MotorolaAnalog",
                             (double)raw * 0.25 + 1.25, "u", 1);
        check(ba_session_push_frame(&s, &msg) == 1,
              "push motorola candidate frame");
    }

    ba_session_analyze(&s, 16);

    int found_motorola = 0;
    double motorola_score = -1.0;
    double intel_score = -1.0;
    ba_candidate_t motorola_candidate;
    memset(&motorola_candidate, 0, sizeof(motorola_candidate));
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 16 &&
            c->bit_length == 16 &&
            !c->is_signed) {
            if (c->byte_order == BA_BYTE_ORDER_MOTOROLA &&
                c->dbc_start_bit == 23) {
                motorola_score = c->total_score;
                if (c->r_squared > 0.999 &&
                    fabs(c->factor - 0.25) < 1e-6 &&
                    fabs(c->offset - 1.25) < 1e-6) {
                    found_motorola = 1;
                    motorola_candidate = *c;
                }
            } else if (c->byte_order == BA_BYTE_ORDER_INTEL) {
                intel_score = c->total_score;
            }
        }
    }

    check(found_motorola, "known 16-bit Motorola candidate not found");
    if (!(intel_score < 0.0 || motorola_score > intel_score)) {
        fprintf(stderr, "Motorola score %.6f, Intel score %.6f\n",
                motorola_score, intel_score);
    }
    check(intel_score < 0.0 || motorola_score > intel_score,
          "Motorola candidate does not outrank Intel interpretation");
    const uint64_t validation_raws[] = {0x0001u, 0x1234u, 0x8001u, 0xfffeu};
    validate_candidate_dbc_roundtrip(found_motorola ? &motorola_candidate : NULL,
                                     0x131, 0, 8,
                                     validation_raws,
                                     sizeof(validation_raws) /
                                     sizeof(validation_raws[0]));

    ba_session_destroy(&s);
}

static void lag_tests(void)
{
    enum { N = 512, LAG_FRAMES = 3 };
    uint16_t input_raw[N];
    for (int i = 0; i < N; i++)
        input_raw[i] = (uint16_t)(((uint32_t)i * 1103u + 97u) & 0x0fffu);

    ba_session_t s;
    check(ba_session_init(&s, N) == 0, "lag session init");
    ba_session_configure_target(&s, 0x220, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < N; i++) {
        int delayed = i >= LAG_FRAMES ? i - LAG_FRAMES : 0;
        uint16_t payload_raw = input_raw[delayed];
        uint64_t payload = (uint64_t)payload_raw << 8;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x220;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_CONTINUOUS, "DelayedInput",
                             (double)input_raw[i], "", 1);
        check(ba_session_push_frame(&s, &msg) == 1, "push lag frame");
    }

    ba_session_analyze(&s, 16);

    int found_lagged = 0;
    for (size_t i = 0; i < s.candidate_count; i++) {
        const ba_candidate_t *c = &s.candidates[i];
        if (c->canonical_start_bit == 8 &&
            c->bit_length == 12 &&
            c->byte_order == BA_BYTE_ORDER_INTEL &&
            !c->is_signed &&
            fabs(c->best_lag_ms - 30.0) <= 10.1 &&
            fabs(c->pearson) > 0.99 &&
            c->r_squared > 0.999 &&
            fabs(c->factor - 1.0) < 1e-9 &&
            fabs(c->offset) < 1e-6) {
            found_lagged = 1;
            break;
        }
    }
    check(found_lagged, "known 30 ms lagged candidate not found");

    ba_session_destroy(&s);
}

static void bit_lag_tests(void)
{
    enum { N = 240, LAG_FRAMES = 4 };
    uint8_t input_state[N];
    for (int i = 0; i < N; i++)
        input_state[i] = (uint8_t)((i / 24) & 1);

    ba_session_t s;
    check(ba_session_init(&s, N) == 0, "bit lag session init");
    ba_session_configure_target(&s, 0x221, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    for (int i = 0; i < N; i++) {
        int delayed = i >= LAG_FRAMES ? i - LAG_FRAMES : 0;
        uint64_t payload = input_state[delayed] ? (1ull << 24) : 0ull;

        can_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.id = 0x221;
        msg.dlc = 8;
        msg.direction = CAN_DIR_RX;
        msg.timestamp.tv_sec = i / 100;
        msg.timestamp.tv_nsec = (long)(i % 100) * 10000000L;
        msg.seq = (uint64_t)i + 1u;
        put_u64_le(msg.data, payload);

        ba_session_set_input(&s, BA_INPUT_BOOLEAN, "DelayedSwitch",
                             (double)input_state[i], "", 1);
        check(ba_session_push_frame(&s, &msg) == 1, "push bit lag frame");
    }

    ba_session_analyze(&s, 16);
    const ba_bit_stats_t *st = &s.bit_stats[24];
    check(fabs(st->best_lag_ms - 40.0) <= 10.1 &&
          fabs(st->lagged_score) > 0.99,
          "known delayed bit lag not detected");

    ba_session_destroy(&s);
}

static double elapsed_seconds(const struct timespec *a,
                              const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void fast_path_tests(void)
{
    enum { FRAME_COUNT = 20000 };
    ba_session_t s;
    check(ba_session_init(&s, FRAME_COUNT) == 0, "fast path session init");
    ba_session_configure_target(&s, 0x555, 0, 8);
    s.state = BA_SESSION_CAPTURING;

    can_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = 0x555;
    msg.dlc = 8;
    msg.direction = CAN_DIR_RX;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < FRAME_COUNT; i++) {
        msg.seq = (uint64_t)i + 1u;
        msg.timestamp.tv_sec = i / 1000;
        msg.timestamp.tv_nsec = (long)(i % 1000) * 1000000L;
        put_u64_le(msg.data, (uint64_t)i);
        check(ba_session_push_frame(&s, &msg) == 1, "fast path push");
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double seconds = elapsed_seconds(&start, &end);
    check(s.sample_count == FRAME_COUNT, "fast path stored all frames");
    check(seconds < 2.0, "fast path below synthetic 1 kHz budget");

    ba_session_destroy(&s);
}

int main(void)
{
    extraction_tests();
    target_filter_tests();
    candidate_tests();
    baseline_segment_tests();
    validation_segment_tests();
    transition_match_tests();
    checksum_tests();
    mux_tests();
    timestamp_tests();
    signed_candidate_tests();
    motorola_candidate_tests();
    lag_tests();
    bit_lag_tests();
    fast_path_tests();

    if (failures) {
        fprintf(stderr, "%d bit-analysis test(s) failed.\n", failures);
        return 1;
    }
    puts("bit-analysis tests passed");
    return 0;
}
