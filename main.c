/**
 * @file main.c
 * @brief Entry point for CANoScope — a GTK3 CAN bus monitor/analyser.
 *
 * @details
 * CANoScope is a GTK3 desktop application that uses the Linux SocketCAN
 * subsystem (`PF_CAN`) to monitor, transmit, and record CAN / CAN FD traffic.
 * This translation unit wires up the GTK application object, parses the small
 * command-line interface (`--version` / `--help`), initialises the global
 * application state, runs the GTK main loop, and tears everything down on exit.
 *
 * Core feature set:
 *   - Real-time receive trace (timestamp, ID, type, DLC, data).
 *   - CAN FD support (ISO 11898-1:2015), including 64-byte payloads.
 *   - Listen-only mode and configurable nominal/data bitrates.
 *   - One-shot and cyclic message transmission.
 *   - Bus-load metering and error-frame detection.
 *   - In-memory trace capture with CSV export.
 *
 * Threading model:
 *   - Main thread  — GTK event loop.
 *   - rx_thread    — @ref thread_rx : socketcan_recv → idle callback → GUI.
 *   - tx_thread    — @ref thread_tx : GAsyncQueue → socketcan_send.
 *   - stats_thread — @ref thread_stats : periodic bus-load update.
 *
 * @author Subhajit Roy <subhajitroy005@gmail.com>
 * @date 2026
 * @copyright SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "inc/app_state.h"
#include "inc/gui.h"
#include "inc/version.h"

/**
 * @brief GTK `activate` signal handler — builds and shows the main window.
 *
 * Invoked by GApplication once the application has been registered and is
 * ready to present its user interface.
 *
 * @param app   The owning GtkApplication instance.
 * @param data  Unused user data.
 */
static void on_app_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    gui_create_main_window(app);
}

/**
 * @brief Print the version banner to standard output.
 */
static void print_version(void)
{
    printf("%s %s\n%s\nAuthor: %s <%s>\nLicense: Apache-2.0\n",
           CANOSCOPE_APP_NAME, CANOSCOPE_VERSION,
           "Copyright (C) 2026 " CANOSCOPE_VENDOR,
           CANOSCOPE_AUTHOR, CANOSCOPE_EMAIL);
}

/**
 * @brief Print command-line usage to standard output.
 *
 * @param argv0  The program name (typically `argv[0]`).
 */
static void print_help(const char *argv0)
{
    printf("Usage: %s [OPTION]\n\n"
           "A GTK3 CAN / CAN FD bus monitor using Linux SocketCAN.\n\n"
           "Options:\n"
           "  -v, --version   Show version information and exit\n"
           "  -h, --help      Show this help text and exit\n\n"
           "With no options the graphical application is launched.\n",
           argv0);
}

/**
 * @brief Program entry point.
 *
 * Handles the `--version` / `--help` flags before any GTK initialisation so
 * that the binary can be queried in headless environments (CI, packaging
 * smoke tests).  Otherwise it initialises the global @ref app_state_t, creates
 * the GtkApplication, runs the main loop, and performs cleanup on shutdown.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return Process exit status (0 on success, GApplication status otherwise).
 */
int main(int argc, char *argv[])
{
    /* Lightweight CLI handled before GTK so it works without a display. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            print_version();
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help(argv[0]);
            return 0;
        }
    }

    app_state_init();

    GtkApplication *app = gtk_application_new(
        "com.taksys.canoscope",
#if GLIB_CHECK_VERSION(2, 74, 0)
        G_APPLICATION_DEFAULT_FLAGS);
#else
        G_APPLICATION_FLAGS_NONE);   /* GLib < 2.74 (e.g. Ubuntu 20.04/22.04) */
#endif

    g_signal_connect(app, "activate",
                     G_CALLBACK(on_app_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    app_state_cleanup();

    return status;
}
