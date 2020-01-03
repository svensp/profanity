/*
 * tray.c
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2012 - 2019 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#include "config.h"

#ifdef HAVE_GTK
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#include "log.h"
#include "config/preferences.h"
#include "config/files.h"
#include "ui/tray.h"
#include "ui/window_list.h"

static gboolean gtk_ready = FALSE;
static GtkStatusIcon *prof_tray = NULL;
static GString *icon_filename = NULL;
static GString *icon_msg_filename = NULL;
static gint unread_messages;
static gboolean shutting_down;
static gboolean statusicon_disabled;
static guint timer;
#ifdef HAVE_APPINDICATOR
static AppIndicator *indicator = NULL;
static GtkMenu *indicator_menu = NULL;
static gboolean appindicator_disabled;
#endif

void
_tray_appindicator_update(int unread_messages);
void
_tray_statusicon_update(int unread_messages);

/*
 * Get icons from installation share folder or (if defined) .locale user's folder
 *
 * As implementation, looking through all the entries in the .locale folder is chosen.
 * While useless as now, it might be useful in case an association name-icon is created.
 * As now, with 2 icons only, this is pretty useless, but it is not harming ;)
 *
 */
static void
_get_icons(void)
{
    GString *icons_dir =  NULL;

#ifdef ICONS_PATH

    icons_dir = g_string_new(ICONS_PATH);
    icon_filename = g_string_new(icons_dir->str);
    icon_msg_filename = g_string_new(icons_dir->str);
    g_string_append(icon_filename, "/proIcon.png");
    g_string_append(icon_msg_filename, "/proIconMsg.png");
    g_string_free(icons_dir, TRUE);

#endif /* ICONS_PATH */

    char *icons_dir_s = files_get_config_path(DIR_ICONS);
    icons_dir = g_string_new(icons_dir_s);
    free(icons_dir_s);
    GError *err = NULL;
    if (!g_file_test(icons_dir->str, G_FILE_TEST_IS_DIR)) {
        return;
    }
    GDir *dir = g_dir_open(icons_dir->str, 0, &err);
    if (dir) {
        GString *name = g_string_new(g_dir_read_name(dir));
        while (name->len) {
            if (g_strcmp0("proIcon.png", name->str) == 0) {
                if (icon_filename) {
                    g_string_free(icon_filename, TRUE);
                }
                icon_filename = g_string_new(icons_dir->str);
                g_string_append(icon_filename, "/proIcon.png");
            } else if (g_strcmp0("proIconMsg.png", name->str) == 0) {
                if (icon_msg_filename) {
                    g_string_free(icon_msg_filename, TRUE);
                }
                icon_msg_filename = g_string_new(icons_dir->str);
                g_string_append(icon_msg_filename, "/proIconMsg.png");
            }
            g_string_free(name, TRUE);
            name = g_string_new(g_dir_read_name(dir));
        }
        g_string_free(name, TRUE);
    } else {
        log_error("Unable to open dir: %s", err->message);
        g_error_free(err);
    }
    g_dir_close(dir);
    g_string_free(icons_dir, TRUE);
}

/*
 * Callback for the timer
 *
 * This is the callback that the timer is calling in order to check if messages are there.
 *
 */
gboolean
_tray_change_icon(gpointer data)
{
    if (shutting_down) {
        return FALSE;
    }

    unread_messages = wins_get_total_unread();
    _tray_statusicon_update(unread_messages);
    _tray_appindicator_update(unread_messages);
    return TRUE;
}

void
tray_init(void)
{
    statusicon_disabled = TRUE;
#ifdef HAVE_APPINDICATOR
    appindicator_disabled = TRUE;
#endif
    _get_icons();
    gtk_ready = gtk_init_check(0, NULL);
    log_debug("Env is GTK-ready: %s", gtk_ready ? "true" : "false");
    if (!gtk_ready) {
        return;
    }

    if (prefs_get_boolean(PREF_TRAY)) {
        log_debug("Building GTK icon");
        tray_enable();
    }

    gtk_main_iteration_do(FALSE);
}

void
tray_update(void)
{
    if (gtk_ready) {
        gtk_main_iteration_do(FALSE);
    }
}

void
tray_shutdown(void)
{
    if (gtk_ready && prefs_get_boolean(PREF_TRAY)) {
        tray_disable();
    }
    g_string_free(icon_filename, TRUE);
    g_string_free(icon_msg_filename, TRUE);
}

void
tray_set_timer(int interval)
{
    g_source_remove(timer);
    _tray_change_icon(NULL);
    timer = g_timeout_add(interval * 1000, _tray_change_icon, NULL);
}

/*
 * Create tray icon
 *
 * This will initialize the timer that will be called in order to change the icons
 * and will search the icons in the defaults paths
 */
void
tray_enable(void)
{
    shutting_down = FALSE;
    int interval = prefs_get_tray_timer() * 1000;
    timer = g_timeout_add(interval, _tray_change_icon, NULL);

    if (prefs_get_boolean(PREF_TRAY_STATUSICON)) {
        tray_statusicon_enable();
    }
    if (prefs_get_boolean(PREF_TRAY_APPINDICATOR)) {
        tray_appindicator_enable();
    }
}

void
tray_disable(void)
{
    shutting_down = TRUE;
    g_source_remove(timer);
    tray_statusicon_disable();
    tray_appindicator_disable();
}

/**
 * statusicon
 *
 * This uses the gtk statusicon functions to create a gtk compatible system tray
 * icon
 **/
void
tray_statusicon_enable(void)
{
    statusicon_disabled = FALSE;

    prof_tray = gtk_status_icon_new_from_file(icon_filename->str);
    _tray_change_icon(NULL);
}

void
tray_statusicon_disable(void)
{
    statusicon_disabled = TRUE;

    if (prof_tray) {
        g_clear_object(&prof_tray);
        prof_tray = NULL;
    }
}

void
_tray_statusicon_update(int unread_messages)
{
    if (statusicon_disabled) {
        return;
    }

    if (unread_messages) {
        if (!prof_tray) {
            prof_tray = gtk_status_icon_new_from_file(icon_msg_filename->str);
        } else {
            gtk_status_icon_set_from_file(prof_tray, icon_msg_filename->str);
        }
    } else {
        if (prefs_get_boolean(PREF_TRAY_READ)) {
            if (!prof_tray) {
                prof_tray = gtk_status_icon_new_from_file(icon_filename->str);
            } else {
                gtk_status_icon_set_from_file(prof_tray, icon_filename->str);
            }
        } else {
            g_clear_object(&prof_tray);
            prof_tray = NULL;
        }
    }
}

/**
 * appindicator
 *
 * This uses the ayatana-appindicator library to create a SNI standart conform
 * system tray icon
 **/
void
tray_appindicator_enable(void)
{
#ifdef HAVE_APPINDICATOR
    appindicator_disabled = FALSE;

    indicator_menu = (GtkMenu *)gtk_menu_new();
    indicator = app_indicator_new ("profanity-im",
            "proIcon",
            APP_INDICATOR_CATEGORY_COMMUNICATIONS);

    GString *icons_dir =  NULL;

    app_indicator_set_icon_theme_path(indicator, ICONS_PATH);
    g_string_free(icons_dir, TRUE);

    app_indicator_set_status (indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_attention_icon_full (indicator, "proIconMsg", "Unread messages");
    app_indicator_set_menu(indicator, indicator_menu);
#endif
}

void
tray_appindicator_disable(void)
{
#ifdef HAVE_APPINDICATOR
    appindicator_disabled = TRUE;

    if (indicator) {
        g_clear_object(&indicator);
        indicator = NULL;
    }
    if (indicator_menu) {
        g_clear_object(&indicator_menu);
        indicator_menu = NULL;
    }
#endif
}

void
_tray_appindicator_update(int unread_messages)
{
#ifdef HAVE_APPINDICATOR
    if (appindicator_disabled) {
        return;
    }

    if (unread_messages) {
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
    } else {
        if (prefs_get_boolean(PREF_TRAY_READ)) {
            app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
        } else {
            app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);
        }
    }
#endif
}

#endif
