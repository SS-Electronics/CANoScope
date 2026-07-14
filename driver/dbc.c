/**
 * @file dbc.c
 * @brief DBC (CAN database) parser and signal-bit decoder.
 *
 * @details
 * Implements @ref dbc.h.  The parser is a single-pass, line-oriented reader for
 * the `BO_` / `SG_` records of the Vector DBC grammar plus signal comments.
 * Anything it does not understand (value tables, attributes, node lists, …) is ignored, so
 * it loads real-world databases without choking while extracting exactly the
 * structure the Signal Analysis view needs.
 *
 * The decoder handles both bit layouts:
 *   - Intel (little-endian): bits are consumed LSB-first, ascending.
 *   - Motorola (big-endian): bits are consumed MSB-first using the classic
 *     "sawtooth" traversal across the byte-wise LSB0 numbering.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include "../inc/dbc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DBC encodes an extended (29-bit) identifier by setting bit 31 of the BO_ id. */
#define DBC_EXT_FLAG  0x80000000u

static void dbc_set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz > 0)
        snprintf(err, errsz, "%s", msg ? msg : "DBC error.");
}

static void dbc_set_errno(char *err, size_t errsz,
                          const char *prefix, const char *path)
{
    if (err && errsz > 0) {
        snprintf(err, errsz, "%s '%s': %s",
                 prefix ? prefix : "I/O error",
                 path ? path : "",
                 strerror(errno));
    }
}

/**
 * @brief Append an empty message slot to the database, growing as needed.
 * @param db  Database.
 * @return Pointer to the new (zeroed) message, or NULL on allocation failure.
 */
static dbc_message_t *dbc_push_message(dbc_db_t *db)
{
    if (db->message_count >= db->message_cap) {
        size_t ncap = db->message_cap ? db->message_cap * 2 : 16;
        dbc_message_t *nm = realloc(db->messages, ncap * sizeof(*nm));
        if (!nm)
            return NULL;
        db->messages    = nm;
        db->message_cap = ncap;
    }
    dbc_message_t *m = &db->messages[db->message_count++];
    memset(m, 0, sizeof(*m));
    return m;
}

dbc_db_t *dbc_create_empty(const char *path)
{
    dbc_db_t *db = calloc(1, sizeof(*db));
    if (!db)
        return NULL;
    if (path)
        strncpy(db->path, path, sizeof(db->path) - 1);
    return db;
}

/**
 * @brief Parse a `BO_` message-definition line.
 * @param line  Line text (starting at "BO_").
 * @param db    Database to append to.
 * @return Pointer to the new message (the current parse context), or NULL.
 */
static dbc_message_t *parse_bo(const char *line, dbc_db_t *db)
{
    /* BO_ <id> <name>: <dlc> <transmitter> */
    unsigned long raw_id = 0;
    char name[DBC_NAME_MAX] = {0};
    unsigned int dlc = 0;

    if (sscanf(line, "BO_ %lu %63[^:]: %u", &raw_id, name, &dlc) < 2)
        return NULL;

    /* Trim trailing whitespace from the captured name. */
    size_t n = strlen(name);
    while (n > 0 && isspace((unsigned char)name[n - 1]))
        name[--n] = '\0';

    dbc_message_t *m = dbc_push_message(db);
    if (!m)
        return NULL;

    m->is_extended = (raw_id & DBC_EXT_FLAG) ? 1 : 0;
    m->id          = (uint32_t)(raw_id & ~DBC_EXT_FLAG);
    m->dlc         = (uint8_t)(dlc > 64 ? 64 : dlc);
    snprintf(m->name, sizeof(m->name), "%s", name);
    return m;
}

/**
 * @brief Parse a `SG_` signal line and append it to the current message.
 * @param line  Line text (leading whitespace already skipped, starts at "SG_").
 * @param m     Current message (parse context).
 * @return 0 on success, -1 on parse failure or when no message is open.
 */
static int parse_sg(const char *line, dbc_message_t *m)
{
    if (!m || m->signal_count >= DBC_MAX_SIGNALS_PER_MSG)
        return -1;

    /* SG_ <name> : <start>|<len>@<order><sign> (<factor>,<offset>) [<min>|<max>] "<unit>" <recv> */
    char     name[DBC_NAME_MAX] = {0};
    unsigned start = 0, len = 0;
    int      order = 1;       /* 1 = Intel/LE, 0 = Motorola/BE */
    char     sign  = '+';
    double   factor = 1.0, offset = 0.0;
    double   minv = 0.0, maxv = 0.0;
    char     unit[DBC_UNIT_MAX] = {0};

    int got = sscanf(line,
        " SG_ %63s : %u | %u @ %d %c ( %lf , %lf ) [ %lf | %lf ] \"%23[^\"]\"",
        name, &start, &len, &order, &sign, &factor, &offset,
        &minv, &maxv, unit);

    /* Require at least up to the byte order; the rest may be absent in terse
     * databases.  (Multiplexed `SG_ name m0 :` forms are skipped — `name`
     * would capture the multiplex token and the field count falls short.) */
    if (got < 5)
        return -1;

    if (len == 0 || len > 64)
        return -1;

    dbc_signal_t *s = &m->signals[m->signal_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->start_bit     = (uint16_t)start;
    s->length        = (uint16_t)len;
    s->little_endian = order ? 1 : 0;
    s->is_signed     = (sign == '-') ? 1 : 0;
    s->factor        = factor;
    s->offset        = offset;
    s->min           = minv;
    s->max           = maxv;
    snprintf(s->unit, sizeof(s->unit), "%s", unit);

    m->signal_count++;
    return 0;
}

static dbc_message_t *find_message_by_raw_dbc_id(dbc_db_t *db,
                                                 unsigned long raw_id)
{
    if (!db)
        return NULL;
    return dbc_find_message_mut(db,
                                (uint32_t)(raw_id & ~DBC_EXT_FLAG),
                                (raw_id & DBC_EXT_FLAG) ? 1 : 0);
}

static dbc_signal_t *find_signal_mut(dbc_message_t *m, const char *name)
{
    if (!m || !name)
        return NULL;
    for (uint16_t i = 0; i < m->signal_count; i++)
        if (strcmp(m->signals[i].name, name) == 0)
            return &m->signals[i];
    return NULL;
}

static void unescape_dbc_string(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;

    size_t n = 0;
    for (const char *p = src; *p && n + 1 < dstsz; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': dst[n++] = '\n'; break;
            case 'r': dst[n++] = '\r'; break;
            case 't': dst[n++] = '\t'; break;
            default:  dst[n++] = *p; break;
            }
        } else {
            dst[n++] = *p;
        }
    }
    dst[n] = '\0';
}

static void write_escaped_dbc_string(FILE *fp, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         *p; p++) {
        switch (*p) {
        case '"':
        case '\\':
            fputc('\\', fp);
            fputc(*p, fp);
            break;
        case '\n':
            fputs("\\n", fp);
            break;
        case '\r':
            fputs("\\r", fp);
            break;
        case '\t':
            fputs("\\t", fp);
            break;
        default:
            if (*p >= 0x20)
                fputc(*p, fp);
            break;
        }
    }
}

static int parse_dbc_quoted_string(const char *p, char *dst, size_t dstsz)
{
    if (!p || !dst || dstsz == 0)
        return -1;
    if (*p != '"')
        return -1;

    p++;
    size_t n = 0;
    int closed = 0;
    for (; *p; p++) {
        if (*p == '\\' && p[1]) {
            if (n + 2 < dstsz) {
                dst[n++] = *p;
                dst[n++] = p[1];
            }
            p++;
            continue;
        }
        if (*p == '"') {
            closed = 1;
            break;
        }
        if (n + 1 < dstsz)
            dst[n++] = *p;
    }
    dst[n] = '\0';
    return closed ? 0 : -1;
}

static int parse_cm_sg(const char *line, dbc_db_t *db)
{
    char name[DBC_NAME_MAX] = {0};
    char comment[DBC_COMMENT_MAX * 2] = {0};

    const char *p = line;
    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "CM_", 3) != 0)
        return -1;
    p += 3;
    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "SG_", 3) != 0)
        return -1;
    p += 3;
    while (isspace((unsigned char)*p))
        p++;

    char *end = NULL;
    unsigned long raw_id = strtoul(p, &end, 10);
    if (end == p)
        return -1;
    p = end;
    while (isspace((unsigned char)*p))
        p++;

    size_t ni = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (ni + 1 < sizeof(name))
            name[ni++] = *p;
        p++;
    }
    name[ni] = '\0';
    while (isspace((unsigned char)*p))
        p++;
    if (!name[0] || parse_dbc_quoted_string(p, comment, sizeof(comment)) != 0)
        return -1;

    dbc_signal_t *sig =
        find_signal_mut(find_message_by_raw_dbc_id(db, raw_id), name);
    if (!sig)
        return -1;

    unescape_dbc_string(sig->comment, sizeof(sig->comment), comment);
    return 0;
}

dbc_db_t *dbc_load_file(const char *path, char *err, size_t errsz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (err) snprintf(err, errsz, "Cannot open '%s'.", path);
        return NULL;
    }

    dbc_db_t *db = calloc(1, sizeof(*db));
    if (!db) {
        fclose(fp);
        if (err) snprintf(err, errsz, "Out of memory.");
        return NULL;
    }
    strncpy(db->path, path, sizeof(db->path) - 1);

    char line[1024];
    dbc_message_t *cur = NULL;
    while (fgets(line, sizeof(line), fp)) {
        const char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;

        if (strncmp(p, "BO_ ", 4) == 0) {
            cur = parse_bo(p, db);
        } else if (strncmp(p, "SG_ ", 4) == 0) {
            if (parse_sg(p, cur) == 0)
                db->signal_count++;
        } else if (strncmp(p, "CM_ SG_ ", 8) == 0) {
            parse_cm_sg(p, db);
        } else if (*p == '\0' || strncmp(p, "BO_TX_BU_", 9) == 0) {
            /* Blank line or transmitter list — keep the current message. */
        } else if (!isspace((unsigned char)line[0])) {
            /* A new top-level (non-indented) record ends the current BO_. */
            cur = NULL;
        }
    }
    fclose(fp);

    if (db->message_count == 0) {
        if (err) snprintf(err, errsz,
                          "No CAN messages (BO_) found in '%s'.", path);
        dbc_free(db);
        return NULL;
    }
    return db;
}

void dbc_free(dbc_db_t *db)
{
    if (!db)
        return;
    free(db->messages);
    free(db);
}

const dbc_message_t *dbc_find_message(const dbc_db_t *db,
                                      uint32_t id, int is_extended)
{
    if (!db)
        return NULL;
    for (size_t i = 0; i < db->message_count; i++) {
        const dbc_message_t *m = &db->messages[i];
        if (m->id == id && (int)m->is_extended == (is_extended ? 1 : 0))
            return m;
    }
    return NULL;
}

dbc_message_t *dbc_find_message_mut(dbc_db_t *db,
                                    uint32_t id, int is_extended)
{
    if (!db)
        return NULL;
    for (size_t i = 0; i < db->message_count; i++) {
        dbc_message_t *m = &db->messages[i];
        if (m->id == id && (int)m->is_extended == (is_extended ? 1 : 0))
            return m;
    }
    return NULL;
}

dbc_message_t *dbc_upsert_message(dbc_db_t *db,
                                  uint32_t id, int is_extended,
                                  uint8_t dlc, const char *name,
                                  char *err, size_t errsz)
{
    if (!db) {
        dbc_set_err(err, errsz, "No database is open.");
        return NULL;
    }
    if (!name || !*name) {
        dbc_set_err(err, errsz, "Message name is empty.");
        return NULL;
    }

    dbc_message_t *m = dbc_find_message_mut(db, id, is_extended);
    if (!m) {
        m = dbc_push_message(db);
        if (!m) {
            dbc_set_err(err, errsz, "Out of memory while adding message.");
            return NULL;
        }
        m->id          = id;
        m->is_extended = is_extended ? 1 : 0;
    }

    m->dlc = dlc > 64 ? 64 : dlc;
    snprintf(m->name, sizeof(m->name), "%s", name);
    return m;
}

int dbc_upsert_signal(dbc_db_t *db, dbc_message_t *msg,
                      const dbc_signal_t *sig, int *replaced,
                      char *err, size_t errsz)
{
    if (replaced)
        *replaced = 0;
    if (!db || !msg || !sig) {
        dbc_set_err(err, errsz, "Invalid database, message, or signal.");
        return -1;
    }
    if (!sig->name[0]) {
        dbc_set_err(err, errsz, "Signal name is empty.");
        return -1;
    }
    if (sig->length == 0 || sig->length > 64) {
        dbc_set_err(err, errsz, "Signal length must be 1..64 bits.");
        return -1;
    }

    for (uint16_t i = 0; i < msg->signal_count; i++) {
        if (strcmp(msg->signals[i].name, sig->name) == 0) {
            msg->signals[i] = *sig;
            if (replaced)
                *replaced = 1;
            return 0;
        }
    }

    if (msg->signal_count >= DBC_MAX_SIGNALS_PER_MSG) {
        dbc_set_err(err, errsz, "Message already has the maximum number of signals.");
        return -1;
    }

    msg->signals[msg->signal_count++] = *sig;
    db->signal_count++;
    return 0;
}

int dbc_save_file(const dbc_db_t *db, const char *path,
                  char *err, size_t errsz)
{
    if (!db || !path || !*path) {
        dbc_set_err(err, errsz, "No database path was selected.");
        return -1;
    }

    char tmp[sizeof(db->path) + 16];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        dbc_set_err(err, errsz, "Database path is too long.");
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        dbc_set_errno(err, errsz, "Cannot write", path);
        return -1;
    }

    fprintf(fp, "VERSION \"CANoScope generated database\"\n\n\n");
    fprintf(fp, "NS_ :\n");
    fprintf(fp, "\tBA_\n");
    fprintf(fp, "\tBA_DEF_\n");
    fprintf(fp, "\tBA_DEF_DEF_\n");
    fprintf(fp, "\tBO_TX_BU_\n");
    fprintf(fp, "\tCM_\n");
    fprintf(fp, "\tSG_MUL_VAL_\n");
    fprintf(fp, "\tVAL_\n");
    fprintf(fp, "\tVAL_TABLE_\n\n");
    fprintf(fp, "BS_:\n\n");
    fprintf(fp, "BU_: Vector__XXX\n\n");

    for (size_t mi = 0; mi < db->message_count; mi++) {
        const dbc_message_t *m = &db->messages[mi];
        unsigned long raw_id = (unsigned long)m->id;
        if (m->is_extended)
            raw_id |= DBC_EXT_FLAG;

        fprintf(fp, "BO_ %lu %s: %u Vector__XXX\n",
                raw_id, m->name, (unsigned)m->dlc);

        for (uint16_t si = 0; si < m->signal_count; si++) {
            const dbc_signal_t *s = &m->signals[si];
            char unit[DBC_UNIT_MAX * 2];
            size_t up = 0;
            for (size_t i = 0; s->unit[i] && up + 1 < sizeof(unit); i++) {
                unsigned char c = (unsigned char)s->unit[i];
                if (c == '"' || c == '\\')
                    continue;
                unit[up++] = (char)c;
            }
            unit[up] = '\0';

            fprintf(fp,
                    " SG_ %s : %u|%u@%u%c (%.15g,%.15g) [%.15g|%.15g] \"%s\" Vector__XXX\n",
                    s->name,
                    (unsigned)s->start_bit,
                    (unsigned)s->length,
                    s->little_endian ? 1u : 0u,
                    s->is_signed ? '-' : '+',
                    s->factor,
                    s->offset,
                    s->min,
                    s->max,
                    unit);
        }
        for (uint16_t si = 0; si < m->signal_count; si++) {
            const dbc_signal_t *s = &m->signals[si];
            if (!s->comment[0])
                continue;
            fprintf(fp, "CM_ SG_ %lu %s \"", raw_id, s->name);
            write_escaped_dbc_string(fp, s->comment);
            fprintf(fp, "\";\n");
        }
        fprintf(fp, "\n");
    }

    if (fclose(fp) != 0) {
        dbc_set_errno(err, errsz, "Cannot finish writing", path);
        remove(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        dbc_set_errno(err, errsz, "Cannot replace", path);
        remove(tmp);
        return -1;
    }

    return 0;
}

uint64_t dbc_extract_raw(const uint8_t *data, uint8_t dlc,
                         const dbc_signal_t *sig)
{
    if (!data || !sig || sig->length == 0 || sig->length > 64)
        return 0;

    uint64_t      value    = 0;
    const uint16_t len     = sig->length;
    const uint16_t total   = (uint16_t)dlc * 8u;

    if (sig->little_endian) {
        /* Intel: bit i of the value comes from bit (start_bit + i). */
        for (uint16_t i = 0; i < len; i++) {
            uint16_t bit = (uint16_t)(sig->start_bit + i);
            if (bit >= total)
                break;
            uint8_t b = data[bit >> 3];
            value |= (uint64_t)((b >> (bit & 7)) & 1u) << i;
        }
    } else {
        /* Motorola: consume MSB-first along the sawtooth bit order. */
        int bit = sig->start_bit;
        for (uint16_t i = 0; i < len; i++) {
            if (bit < 0 || bit >= total)
                break;
            uint8_t b = data[bit >> 3];
            value |= (uint64_t)((b >> (bit & 7)) & 1u) << (len - 1 - i);
            if ((bit & 7) == 0)
                bit += 15;   /* jump to the MSB of the next byte */
            else
                bit -= 1;
        }
    }
    return value;
}

double dbc_decode_physical(const dbc_signal_t *sig, uint64_t raw,
                           int64_t *signed_raw_out)
{
    int64_t sraw = (int64_t)raw;

    if (sig->is_signed && sig->length < 64) {
        uint64_t sign_bit = (uint64_t)1 << (sig->length - 1);
        if (raw & sign_bit) {
            /* Sign-extend by setting all bits above the field width. */
            uint64_t mask = ~(((uint64_t)1 << sig->length) - 1);
            sraw = (int64_t)(raw | mask);
        }
    }

    if (signed_raw_out)
        *signed_raw_out = sig->is_signed ? sraw : (int64_t)raw;

    double base = sig->is_signed ? (double)sraw : (double)raw;
    return base * sig->factor + sig->offset;
}
