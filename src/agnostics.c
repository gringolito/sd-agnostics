/*
Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <sys/wait.h>

#include <libintl.h>

/* Columns in tree store */

#define PIAG_FILE           0
#define PIAG_NAME           1
#define PIAG_TEXT           2
#define PIAG_RESULT         3
#define PIAG_ENABLED        4

#define LOGFILE "/home/pi/rpdiags.txt"

/* Controls */

static GtkWidget *piag_wd, *piag_tv, *btn_run, *btn_close, *btn_reset, *btn_log;
static GtkWidget *msg_wd, *msg_label, *msg_prog, *msg_btn;

/* List of tests */

GtkListStore *tests;
GtkTreeModel *stests;

/* Inter-thread globals */

gchar *test_name;
gboolean cancelled;
int testpid;

/* Function prototypes */

static void log_message (int reset, const char *format, ...);
static int find_tests (void);
static void parse_test_file (gchar *path);
static gpointer test_thread (gpointer data);
static int dialog_update (gpointer data);
static void run_test (GtkWidget *wid, gpointer data);
static void reset_test (GtkWidget *wid, gpointer data);
static void run_toggled (GtkCellRendererToggle *cell, gchar *path, gpointer data);
static void cancel_test (GtkWidget *wid, gpointer data);
static void end_program (GtkWidget *wid, gpointer data);


/* Write supplied varargs text to log file */

static void log_message (int reset, const char *format, ...)
{
    FILE *fp;
    va_list args;
    char buffer[256];

    va_start (args, format);
    vsprintf (buffer, format, args);
    va_end (args);

    fp = fopen (LOGFILE, reset ? "w" : "a");
    fprintf (fp, "%s\n", buffer);
    fclose (fp);
}

/* Find all test files in the data directory and add them to the tests list store */

static int find_tests (void)
{
    GDir *data_dir;
    const gchar *name;
    gchar *path;

    data_dir = g_dir_open (PACKAGE_DATA_DIR, 0, NULL);
    if (data_dir)
    {
        while ((name = g_dir_read_name (data_dir)) != NULL)
        {
            if (!g_strcmp0 (name + strlen (name) - 3, ".sh"))
            {
                path = g_strdup_printf ("%s/%s", PACKAGE_DATA_DIR, name);
                parse_test_file (path);
                g_free (path);
            }
        }
        g_dir_close (data_dir);
        stests = gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (tests));
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (stests), PIAG_NAME, GTK_SORT_ASCENDING);
    }
}

/* Open the supplied test file and populate an entry in the tests list store from it
 * A test file must:
 * a) Be a valid shell script that can be run by sh - a shebang is unnecessary and will be ignored
 * b) Contain two metadata lines of the format #NAME=... and #DESC=...
 * c) Return 0 if the test passes and non-zero if it fails
 * All output a test file sends to stdout or stderr will be added to the logfile
 */

static void parse_test_file (gchar *path)
{
    FILE *fp;
    char *line = NULL, *name = NULL, *desc = NULL, *mutext = NULL;
    size_t len = 0;
    GtkTreeIter entry;

    fp = fopen (path, "rb");
    if (fp)
    {
        name = NULL;
        desc = NULL;
        while (getline (&line, &len, fp) != -1)
        {
            if (!strncmp (line, "#NAME=", 6)) name = g_strdup (line + 6);
            if (!strncmp (line, "#DESC=", 6)) desc = g_strdup (line + 6);
        }
        if (name && desc)
        {
            *(name + strlen (name) - 1) = 0;
            *(desc + strlen (desc) - 1) = 0;
            mutext = g_strdup_printf (_("<b>%s</b>\n%s"), name, desc);
            gtk_list_store_append (tests, &entry);
            gtk_list_store_set (tests, &entry, PIAG_FILE, path, PIAG_NAME, name, 
                PIAG_TEXT, mutext, PIAG_RESULT, _("Not Run"), PIAG_ENABLED, TRUE, -1);
            g_free (mutext);
        }
        if (name) g_free (name);
        if (desc) g_free (desc);

        free (line);
        fclose (fp);
    }
}

/* Thread which runs all enabled tests in list store */

static gpointer test_thread (gpointer data)
{
    GtkTreeIter iter, siter;
    GtkTreePath *tp;
    gchar *file;
    gboolean valid, enabled;
    int status, fd;

    // redirect stdout and stderr to the logfile
    fd = open (LOGFILE, O_RDWR|O_APPEND|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (fd != -1)
    {
        dup2 (fd, STDOUT_FILENO);
        dup2 (fd, STDERR_FILENO);
        close (fd);
    }

    // for some reason iterators don't iterate on sorted models...
    tp = gtk_tree_path_new_from_string ("0");
    valid = gtk_tree_model_get_iter (stests, &siter, tp);
    while (valid && !cancelled)
    {
        gtk_tree_model_sort_convert_iter_to_child_iter (GTK_TREE_MODEL_SORT (stests), &iter, &siter);
        gtk_tree_model_get (GTK_TREE_MODEL (tests), &iter, PIAG_FILE, &file, PIAG_NAME, &test_name, PIAG_ENABLED, &enabled, -1);
        if (enabled)
        {
            log_message (FALSE, "Test : %s", test_name);
            testpid = fork ();

            if (testpid == 0)
            {
                execl ("/bin/sh", "sh", file, NULL);
                exit (0);
            }
            else
            {
                setpgid (testpid, testpid);
                wait (&status);
                if (cancelled)
                {
                    gtk_list_store_set (tests, &iter, PIAG_RESULT, _("Aborted"), -1);
                    log_message (FALSE, "Test aborted\n");
                }
                else if (status)
                {
                    gtk_list_store_set (tests, &iter, PIAG_RESULT, _("<span foreground=\"#FF0000\"><b>FAIL</b></span>"), -1);
                    log_message (FALSE, "Test FAIL\n");
                }
                else
                {
                    gtk_list_store_set (tests, &iter, PIAG_RESULT, _("<span foreground=\"#00FF00\"><b>PASS</b></span>"), -1);
                    log_message (FALSE, "Test PASS\n");
                }
            }
        }
        gtk_tree_path_next (tp);
        valid = gtk_tree_model_get_iter (stests, &siter, tp);
    }

    g_free (test_name);
    test_name = NULL;

    return NULL;
}

/* Handler for timer call during tests to update dialog */

static int dialog_update (gpointer data)
{
    gchar *buffer;

    if (test_name)
    {
        if (cancelled)
            buffer = g_strdup_printf (_("Cancelling..."));
        else
            buffer = g_strdup_printf (_("Running %s..."), test_name);
        gtk_label_set_text (GTK_LABEL (msg_label), buffer);
        g_free (buffer);
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_prog));
        return TRUE;
    }
    else
    {
        gtk_widget_destroy (msg_wd);
        gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), FALSE);
        gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), TRUE);
        gtk_widget_show (btn_reset);
        gtk_widget_show (btn_log);
        gtk_widget_hide (btn_run);
        return FALSE;
    }
}

/* Handler for 'run test' button */

static void run_test (GtkWidget *wid, gpointer data)
{
    GtkBuilder *builder;
    time_t now;
    struct tm *tstr;

    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_UI_DIR "/agnostics.ui", NULL);

    msg_wd = (GtkWidget *) gtk_builder_get_object (builder, "msg_wd");
    msg_label = (GtkWidget *) gtk_builder_get_object (builder, "msg_label");
    msg_prog = (GtkWidget *) gtk_builder_get_object (builder, "msg_prog");
    msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "msg_btn");

    gtk_window_set_transient_for (GTK_WINDOW (msg_wd), GTK_WINDOW (piag_wd));
    gtk_widget_set_name (msg_wd, "pixbox");

    g_signal_connect (msg_btn, "clicked", G_CALLBACK (cancel_test), NULL);
    gtk_widget_show_all (GTK_WIDGET (msg_wd));
    g_object_unref (builder);

    // add a timer to update the dialog
    gdk_threads_add_timeout (1000, dialog_update, NULL);

    log_message (TRUE, "Raspberry Pi Diagnostics");
    now = time (NULL);
    tstr = localtime (&now);
    log_message (FALSE, asctime (tstr));

    // launch a thread with the system call to run the tests
    cancelled = FALSE;
    g_thread_new (NULL, test_thread, NULL);
}

/* Handler for 'reset' button */

static void reset_test (GtkWidget *wid, gpointer data)
{
    GtkTreeIter iter;
    gboolean valid;

    gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), TRUE);
    gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), FALSE);
    gtk_widget_hide (btn_reset);
    gtk_widget_hide (btn_log);
    gtk_widget_show (btn_run);

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (tests), &iter);
    while (valid)
    {
        gtk_list_store_set (tests, &iter, PIAG_RESULT, _("Not run"), -1);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (tests), &iter);
    }
}

/* Handler for 'show log' button */

static void show_log (GtkWidget *wid, gpointer data)
{
    char buffer[128];
    sprintf (buffer, "mousepad %s &", LOGFILE);
    system (buffer);
}

/* Handler for click on tree view check box */

static void run_toggled (GtkCellRendererToggle *cell, gchar *path, gpointer data)
{
    GtkTreeIter iter, siter;
    gboolean val;

    gtk_tree_model_get_iter_from_string (stests, &siter, path);
    gtk_tree_model_sort_convert_iter_to_child_iter (GTK_TREE_MODEL_SORT (stests), &iter, &siter);
    gtk_tree_model_get (GTK_TREE_MODEL (tests), &iter, PIAG_ENABLED, &val, -1);
    gtk_list_store_set (GTK_LIST_STORE (tests), &iter, PIAG_ENABLED, 1 - val, -1);
}

/* Handler for 'cancel' button on progress dialog */

static void cancel_test (GtkWidget *wid, gpointer data)
{
    cancelled = TRUE;
    killpg (testpid, SIGTERM);
}

/* Handler for 'close' button */

static void end_program (GtkWidget *wid, gpointer data)
{
    gtk_main_quit ();
}

/* The dialog... */

int main (int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkCellRenderer *crt, *crb, *crr;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    // GTK setup
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // find test files in data directory
    tests = gtk_list_store_new (5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    find_tests ();

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_UI_DIR "/agnostics.ui", NULL);

    piag_wd = (GtkWidget *) gtk_builder_get_object (builder, "piag_wd");
    piag_tv = (GtkWidget *) gtk_builder_get_object (builder, "piag_tv");
    btn_run = (GtkWidget *) gtk_builder_get_object (builder, "btn_run");
    btn_close = (GtkWidget *) gtk_builder_get_object (builder, "btn_close");
    btn_reset = (GtkWidget *) gtk_builder_get_object (builder, "btn_reset");
    btn_log = (GtkWidget *) gtk_builder_get_object (builder, "btn_log");

    gtk_window_set_default_size (GTK_WINDOW (piag_wd), 500, 350);

    // set up tree view
    gtk_tree_view_set_model (GTK_TREE_VIEW (piag_tv), GTK_TREE_MODEL (stests));

    crt = gtk_cell_renderer_text_new ();
    g_object_set (G_OBJECT (crt), "wrap-width", 350, "wrap-mode", PANGO_WRAP_WORD_CHAR, NULL);
    crb = gtk_cell_renderer_toggle_new ();
    g_object_set (G_OBJECT (crb), "activatable", TRUE, NULL);
    crr = gtk_cell_renderer_text_new ();
    g_object_set (G_OBJECT (crr), "xalign", 0.5, NULL);

    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (piag_tv), 0, _("Test"), crt, "markup", PIAG_TEXT, NULL);
    gtk_tree_view_column_set_expand (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 0), TRUE);

    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (piag_tv), 1, _("Run Test?"), crb, "active", PIAG_ENABLED, NULL);
    gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), TRUE);
    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), 100);
    gtk_tree_view_column_set_alignment (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 1), 0.5);

    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (piag_tv), 2, _("Result"), crr, "markup", PIAG_RESULT, NULL);
    gtk_tree_view_column_set_visible (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), FALSE);
    gtk_tree_view_column_set_sizing (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), 100);
    gtk_tree_view_column_set_alignment (gtk_tree_view_get_column (GTK_TREE_VIEW (piag_tv), 2), 0.5);

    // connect handlers
    g_signal_connect (crb, "toggled", G_CALLBACK (run_toggled), NULL);
    g_signal_connect (piag_wd, "delete-event", G_CALLBACK (end_program), NULL);
    g_signal_connect (btn_close, "clicked", G_CALLBACK (end_program), NULL);
    g_signal_connect (btn_run, "clicked", G_CALLBACK (run_test), NULL);
    g_signal_connect (btn_reset, "clicked", G_CALLBACK (reset_test), NULL);
    g_signal_connect (btn_log, "clicked", G_CALLBACK (show_log), NULL);

    gtk_widget_hide (btn_reset);
    gtk_widget_hide (btn_log);

    gtk_widget_show (piag_wd);
    g_object_unref (builder);

    // main loop
    gtk_main ();

    return 0;
}
