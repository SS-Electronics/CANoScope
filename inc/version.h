/**
 * @file version.h
 * @brief Centralised version and product-identity macros for CANoScope.
 *
 * @details
 * A single source of truth for the application version string, product name,
 * vendor, and contact details.  These macros are consumed by the command-line
 * `--version` handler (@ref main), the GTK *About* dialog
 * (@ref gui_show_about_dialog), and the Debian packaging metadata.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */
#ifndef CANOSCOPE_VERSION_H
#define CANOSCOPE_VERSION_H

/** Human-readable application name. */
#define CANOSCOPE_APP_NAME    "CANoScope"

/** Semantic version of the application (MAJOR.MINOR.PATCH). */
#define CANOSCOPE_VERSION     "2.0.0"

/** Vendor / brand owning this distribution. */
#define CANOSCOPE_VENDOR      "Taksys"

/** Primary author. */
#define CANOSCOPE_AUTHOR      "Subhajit Roy"

/** Author contact e-mail. */
#define CANOSCOPE_EMAIL       "subhajitroy005@gmail.com"

/** Copyright line shown in the About dialog and `--version` output. */
#define CANOSCOPE_COPYRIGHT   "Copyright \302\251 2026 " CANOSCOPE_VENDOR

#endif /* CANOSCOPE_VERSION_H */
