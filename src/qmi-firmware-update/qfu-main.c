/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * qmi-firmware-update -- Command line tool to update firmware in QMI devices
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2016 Zodiac Inflight Innovation
 * Copyright (C) 2016 Aleksander Morgado <aleksander@aleksander.es>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include <libqmi-glib.h>

#define PROGRAM_NAME    "qmi-firmware-update"
#define PROGRAM_VERSION PACKAGE_VERSION

/*****************************************************************************/
/* Main options */

static gchar    *device_str;
static gboolean  device_open_proxy_flag;
static gboolean  device_open_mbim_flag;
static gboolean  verbose_flag;
static gboolean  silent_flag;
static gboolean  version_flag;

static GOptionEntry main_entries[] = {
    { "device", 'd', 0, G_OPTION_ARG_FILENAME, &device_str,
      "Specify device path.",
      "[PATH]"
    },
    { "device-open-proxy", 'p', 0, G_OPTION_ARG_NONE, &device_open_proxy_flag,
      "Request to use the 'qmi-proxy' proxy.",
      NULL
    },
    { "device-open-mbim", 0, 0, G_OPTION_ARG_NONE, &device_open_mbim_flag,
      "Open an MBIM device with EXT_QMUX support.",
      NULL
    },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose_flag,
      "Run action with verbose logs, including the debug ones.",
      NULL
    },
    { "silent", 0, 0, G_OPTION_ARG_NONE, &silent_flag,
      "Run action with no logs; not even the error/warning ones.",
      NULL
    },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version_flag,
      "Print version.",
      NULL
    },
    { NULL }
};

/*****************************************************************************/
/* Runtime globals */

GMainLoop    *loop;
GCancellable *cancellable;
gint          exit_status;

/*****************************************************************************/
/* Signal handlers */

static gboolean
signals_handler (gpointer psignum)
{
    /* Flag failed exit */
    exit_status = EXIT_FAILURE;

    /* Ignore consecutive requests of cancellation */
    if (!g_cancellable_is_cancelled (cancellable)) {
        g_printerr ("cancelling the operation...\n");
        g_cancellable_cancel (cancellable);
        /* Re-set the signal handler to allow main loop cancellation on
         * second signal */
        g_unix_signal_add (GPOINTER_TO_INT (psignum),  (GSourceFunc) signals_handler, psignum);
        return FALSE;
    }

    if (g_main_loop_is_running (loop)) {
        g_printerr ("cancelling the main loop...\n");
        g_main_loop_quit (loop);
    }

    return FALSE;
}

/*****************************************************************************/
/* Logging output */

static void
log_handler (const gchar    *log_domain,
             GLogLevelFlags  log_level,
             const gchar    *message,
             gpointer        user_data)
{
    const gchar *log_level_str;
    time_t       now;
    gchar        time_str[64];
    struct tm   *local_time;
    gboolean     err;

    /* Nothing to do if we're silent */
    if (silent_flag)
        return;

    now = time ((time_t *) NULL);
    local_time = localtime (&now);
    strftime (time_str, 64, "%d %b %Y, %H:%M:%S", local_time);
    err = FALSE;

    switch (log_level) {
    case G_LOG_LEVEL_WARNING:
        log_level_str = "-Warning **";
        err = TRUE;
        break;

    case G_LOG_LEVEL_CRITICAL:
    case G_LOG_FLAG_FATAL:
    case G_LOG_LEVEL_ERROR:
        log_level_str = "-Error **";
        err = TRUE;
        break;

    case G_LOG_LEVEL_DEBUG:
        log_level_str = "[Debug]";
        break;

    default:
        log_level_str = "";
        break;
    }

    if (!verbose_flag && !err)
        return;

    g_fprintf (err ? stderr : stdout,
               "[%s] %s %s\n",
               time_str,
               log_level_str,
               message);
}

/*****************************************************************************/
/* Version */

static void
print_version_and_exit (void)
{
    g_print ("\n"
             PROGRAM_NAME " " PROGRAM_VERSION "\n"
             "Copyright (C) 2016 Bjørn Mork\n"
             "Copyright (C) 2016 Zodiac Inflight Innovations\n"
             "Copyright (C) 2016 Aleksander Morgado\n"
             "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl-2.0.html>\n"
             "This is free software: you are free to change and redistribute it.\n"
             "There is NO WARRANTY, to the extent permitted by law.\n"
             "\n");
    exit (EXIT_SUCCESS);
}

/*****************************************************************************/

int main (int argc, char **argv)
{
    GError         *error = NULL;
    GOptionContext *context;

    setlocale (LC_ALL, "");

    g_type_init ();

    /* Setup option context, process it and destroy it */
    context = g_option_context_new ("- Update firmware in QMI devices");
    g_option_context_add_main_entries (context, main_entries, NULL);
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("error: %s\n",
                    error->message);
        exit (EXIT_FAILURE);
    }
    g_option_context_free (context);

    if (version_flag)
        print_version_and_exit ();

    g_log_set_handler (NULL, G_LOG_LEVEL_MASK, log_handler, NULL);
    g_log_set_handler ("Qmi", G_LOG_LEVEL_MASK, log_handler, NULL);
    if (verbose_flag)
        qmi_utils_set_traces_enabled (TRUE);

    /* No device path given? */
    if (!device_str) {
        g_printerr ("error: no device path specified\n");
        exit (EXIT_FAILURE);
    }

    /* Create runtime context */
    loop        = g_main_loop_new (NULL, FALSE);
    cancellable = g_cancellable_new ();
    exit_status = EXIT_SUCCESS;

    /* Setup signals */
    g_unix_signal_add (SIGINT,  (GSourceFunc)signals_handler, GUINT_TO_POINTER (SIGINT));
    g_unix_signal_add (SIGHUP,  (GSourceFunc)signals_handler, GUINT_TO_POINTER (SIGHUP));
    g_unix_signal_add (SIGTERM, (GSourceFunc)signals_handler, GUINT_TO_POINTER (SIGTERM));

    /* Run! */
    g_main_loop_run (loop);

    g_object_unref    (cancellable);
    g_main_loop_unref (loop);

    return (exit_status);
}