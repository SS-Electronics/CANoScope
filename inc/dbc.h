/**
 * @file dbc.h
 * @brief Minimal CAN database (DBC) model, parser, and signal decoder.
 *
 * @details
 * Provides a lightweight, GTK-independent representation of a Vector DBC file
 * sufficient to drive the Signal Analysis view: a database
 * (@ref dbc_db_t) is a list of messages (@ref dbc_message_t), each carrying a
 * set of signals (@ref dbc_signal_t).  The parser understands the `BO_`
 * (message) and `SG_` (signal) records of the DBC grammar — enough to decode
 * real-world databases for monitoring — and the decoder extracts a signal's raw
 * bits (Intel/little-endian or Motorola/big-endian) and converts them to a
 * physical value via the linear `factor`/`offset` transform.
 *
 * The module depends only on the C standard library so it can be unit-tested or
 * reused outside the GUI.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef DBC_H
#define DBC_H

#include <stddef.h>
#include <stdint.h>

#define DBC_NAME_MAX            64  /**< Max message/signal name length.        */
#define DBC_UNIT_MAX            24  /**< Max signal unit string length.         */
#define DBC_MAX_SIGNALS_PER_MSG 64  /**< Max signals stored per message.        */

/**
 * @brief A single decoded-able signal within a message.
 *
 * Bit positions follow the DBC convention: @ref start_bit is the position of the
 * signal's least-significant bit for Intel layout, or of its most-significant
 * bit for Motorola layout, in byte-wise LSB0 numbering
 * (bit 0 = LSB of byte 0, bit 7 = MSB of byte 0, bit 8 = LSB of byte 1, …).
 */
typedef struct {
    char     name[DBC_NAME_MAX]; /**< Signal name.                              */
    uint16_t start_bit;          /**< Start bit (see struct note).              */
    uint16_t length;             /**< Bit length (1..64).                       */
    uint8_t  little_endian;      /**< 1 = Intel (LE), 0 = Motorola (BE).        */
    uint8_t  is_signed;          /**< Non-zero for a signed (two's-complement). */
    double   factor;             /**< Physical = raw * factor + offset.         */
    double   offset;             /**< Linear offset.                            */
    double   min;                /**< Documented minimum physical value.        */
    double   max;                /**< Documented maximum physical value.        */
    char     unit[DBC_UNIT_MAX]; /**< Engineering unit (may be empty).          */
} dbc_signal_t;

/**
 * @brief A CAN message definition with its signals.
 */
typedef struct {
    uint32_t     id;            /**< Raw 11-/29-bit identifier (no flags).      */
    uint8_t      is_extended;   /**< Non-zero for a 29-bit extended ID.         */
    uint8_t      dlc;           /**< Declared payload length.                   */
    char         name[DBC_NAME_MAX];                 /**< Message name.         */
    dbc_signal_t signals[DBC_MAX_SIGNALS_PER_MSG];   /**< Signal table.         */
    uint16_t     signal_count;  /**< Number of valid entries in @ref signals.   */
} dbc_message_t;

/**
 * @brief A parsed CAN database (collection of messages).
 */
typedef struct {
    dbc_message_t *messages;      /**< Dynamically grown message array.         */
    size_t         message_count; /**< Number of messages.                      */
    size_t         message_cap;   /**< Allocated capacity.                      */
    size_t         signal_count;  /**< Total signals across all messages.       */
    char           path[512];     /**< Source file path.                        */
} dbc_db_t;

/**
 * @brief Parse a DBC file into a new database.
 * @param path   Path to the `.dbc` file.
 * @param err    Optional buffer for a human-readable error (may be NULL).
 * @param errsz  Size of @p err.
 * @return A new database (free with @ref dbc_free), or NULL on failure.
 */
dbc_db_t *dbc_load_file(const char *path, char *err, size_t errsz);

/**
 * @brief Release a database returned by @ref dbc_load_file. @param db Database.
 */
void dbc_free(dbc_db_t *db);

/**
 * @brief Find the message definition matching a CAN identifier.
 * @param db           Database (may be NULL).
 * @param id           Raw 11-/29-bit identifier.
 * @param is_extended  Non-zero to match an extended-ID message.
 * @return The message, or NULL if not present.
 */
const dbc_message_t *dbc_find_message(const dbc_db_t *db,
                                      uint32_t id, int is_extended);

/**
 * @brief Extract a signal's raw (unscaled) bit field from a payload.
 * @param data  Payload bytes.
 * @param dlc   Payload length in bytes.
 * @param sig   Signal description.
 * @return The raw value, right-aligned (not sign-extended).
 */
uint64_t dbc_extract_raw(const uint8_t *data, uint8_t dlc,
                         const dbc_signal_t *sig);

/**
 * @brief Convert a raw field to its physical value.
 * @param sig             Signal description.
 * @param raw             Raw value from @ref dbc_extract_raw.
 * @param signed_raw_out  Optional out: sign-extended raw as a signed integer.
 * @return The physical value (`raw * factor + offset`).
 */
double dbc_decode_physical(const dbc_signal_t *sig, uint64_t raw,
                           int64_t *signed_raw_out);

#endif /* DBC_H */
