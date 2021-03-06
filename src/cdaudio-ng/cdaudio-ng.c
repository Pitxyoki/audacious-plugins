/*
 * Audacious CD Digital Audio plugin
 *
 * Copyright (c) 2007 Calin Crisan <ccrisan@gmail.com>
 * Copyright (c) 2009-2012 John Lindgren <john.lindgren@aol.com>
 * Copyright (c) 2009 Tomasz Moń <desowin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 */

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <cdio/cdio.h>
#include <cdio/cdtext.h>
#include <cdio/track.h>
#include <cdio/audio.h>
#include <cdio/sector.h>
#include <cdio/cd_types.h>

#if LIBCDIO_VERSION_NUM >= 90
#include <cdio/paranoia/cdda.h>
#else
#include <cdio/cdda.h>
#endif

/* libcdio's header files #define these */
#undef PACKAGE
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#undef VERSION

#include <cddb/cddb.h>

#include <glib.h>

#include <audacious/debug.h>
#include <audacious/i18n.h>
#include <audacious/misc.h>
#include <audacious/playlist.h>
#include <audacious/plugin.h>
#include <audacious/preferences.h>
#include <libaudcore/hook.h>
#include <libaudgui/libaudgui.h>
#include <libaudgui/libaudgui-gtk.h>

#include "config.h"

#define DEF_STRING_LEN 256

#define MIN_DISC_SPEED 2
#define MAX_DISC_SPEED 24

#define MAX_RETRIES 10
#define MAX_SKIPS 10

#define warn(...) fprintf(stderr, "cdaudio-ng: " __VA_ARGS__)

typedef struct
{
    char performer[DEF_STRING_LEN];
    char name[DEF_STRING_LEN];
    char genre[DEF_STRING_LEN];
    int startlsn;
    int endlsn;
}
trackinfo_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int seek_time;
static bool_t playing;

/* lock mutex to read / set these variables */
static int firsttrackno = -1;
static int lasttrackno = -1;
static int n_audio_tracks;
static cdrom_drive_t *pcdrom_drive = NULL;
static trackinfo_t *trackinfo = NULL;
static int monitor_source = 0;

static bool_t cdaudio_init (void);
static int cdaudio_is_our_file (const char * filename, VFSFile * file);
static bool_t cdaudio_play (InputPlayback * p, const char * name, VFSFile *
 file, int start, int stop, bool_t pause);
static void cdaudio_stop (InputPlayback * pinputplayback);
static void cdaudio_pause (InputPlayback * p, bool_t paused);
static void cdaudio_mseek (InputPlayback * p, int time);
static void cdaudio_cleanup (void);
static Tuple * make_tuple (const char * filename, VFSFile * file);
static void scan_cd (void);
static void refresh_trackinfo (bool_t warning);
static int calculate_track_length (int startlsn, int endlsn);
static int find_trackno_from_filename (const char * filename);

static const char cdaudio_about[] =
 N_("Copyright (C) 2007-2012 Calin Crisan <ccrisan@gmail.com> and others.\n\n"
    "Many thanks to libcdio developers <http://www.gnu.org/software/libcdio/>\n"
    "and to libcddb developers <http://libcddb.sourceforge.net/>.\n\n"
    "Also thank you to Tony Vroon for mentoring and guiding me.\n\n"
    "This was a Google Summer of Code 2007 project.");

static const char * const schemes[] = {"cdda", NULL};

static const char * const cdaudio_defaults[] = {
 "disc_speed", "2",
 "use_cdtext", "TRUE",
 "use_cddb", "TRUE",
 "cddbhttp", "FALSE",
 "cddbserver", "freedb.org",
 "cddbport", "8880",
 NULL};

static const PreferencesWidget cdaudio_widgets[] = {
 {WIDGET_LABEL, N_("<b>Device</b>")},
 {WIDGET_SPIN_BTN, N_("Read speed:"),
  .cfg_type = VALUE_INT, .csect = "CDDA", .cname = "disc_speed",
  .data = {.spin_btn = {MIN_DISC_SPEED, MAX_DISC_SPEED, 1}}},
 {WIDGET_ENTRY, N_("Override device:"),
  .cfg_type = VALUE_STRING, .csect = "CDDA", .cname = "device"},
 {WIDGET_LABEL, N_("<b>Metadata</b>")},
 {WIDGET_CHK_BTN, N_("Use CD-Text"),
  .cfg_type = VALUE_BOOLEAN, .csect = "CDDA", .cname = "use_cdtext"},
 {WIDGET_CHK_BTN, N_("Use CDDB"),
  .cfg_type = VALUE_BOOLEAN, .csect = "CDDA", .cname = "use_cddb"},
 {WIDGET_CHK_BTN, N_("Use HTTP instead of CDDBP"), .child = TRUE,
  .cfg_type = VALUE_BOOLEAN, .csect = "CDDA", .cname = "cddbhttp"},
 {WIDGET_ENTRY, N_("Server:"), .child = TRUE,
  .cfg_type = VALUE_STRING, .csect = "CDDA", .cname = "cddbserver"},
 {WIDGET_ENTRY, N_("Path:"), .child = TRUE,
  .cfg_type = VALUE_STRING, .csect = "CDDA", .cname = "cddbpath"},
 {WIDGET_SPIN_BTN, N_("Port:"), .child = TRUE,
  .cfg_type = VALUE_INT, .csect = "CDDA", .cname = "cddbport",
  .data = {.spin_btn = {0, 65535, 1}}}};

static const PluginPreferences cdaudio_prefs = {
 .widgets = cdaudio_widgets,
 .n_widgets = G_N_ELEMENTS (cdaudio_widgets)};

AUD_INPUT_PLUGIN
(
    .name = N_("Audio CD Plugin"),
    .domain = PACKAGE,
    .about_text = cdaudio_about,
    .prefs = & cdaudio_prefs,
    .init = cdaudio_init,
    .cleanup = cdaudio_cleanup,
    .is_our_file_from_vfs = cdaudio_is_our_file,
    .play = cdaudio_play,
    .stop = cdaudio_stop,
    .pause = cdaudio_pause,
    .mseek = cdaudio_mseek,
    .probe_for_tuple = make_tuple,
    .schemes = schemes,
    .have_subtune = TRUE,
)

static void cdaudio_error (const char * message_format, ...)
{
    va_list args;
    char *msg = NULL;

    va_start (args, message_format);
    msg = g_markup_vprintf_escaped (message_format, args);
    va_end (args);

    aud_interface_show_error (msg);
    g_free (msg);
}

/* main thread only */
static void purge_playlist (int playlist)
{
    int length = aud_playlist_entry_count (playlist);

    for (int count = 0; count < length; count ++)
    {
        char * filename = aud_playlist_entry_get_filename (playlist, count);

        if (cdaudio_is_our_file (filename, NULL))
        {
            aud_playlist_entry_delete (playlist, count, 1);
            count--;
            length--;
        }

        str_unref (filename);
    }
}

/* main thread only */
static void purge_all_playlists (void)
{
    int playlists = aud_playlist_count ();
    int count;

    for (count = 0; count < playlists; count++)
        purge_playlist (count);
}

/* main thread only */
static bool_t monitor (gpointer unused)
{
    pthread_mutex_lock (& mutex);

    /* make sure not to close drive handle while playing */
    if (playing)
    {
        pthread_mutex_unlock (& mutex);
        return true;
    }

    if (trackinfo != NULL)
        refresh_trackinfo (FALSE);

    if (trackinfo != NULL)
    {
        pthread_mutex_unlock (& mutex);
        return TRUE;
    }

    monitor_source = 0;
    pthread_mutex_unlock (& mutex);

    purge_all_playlists ();
    return FALSE;
}

/* mutex must be locked */
static void trigger_monitor (void)
{
    if (! monitor_source)
        monitor_source = g_timeout_add_seconds (1, monitor, NULL);
}

/* main thread only */
static bool_t cdaudio_init (void)
{
    aud_config_set_defaults ("CDDA", cdaudio_defaults);

    if (!cdio_init ())
    {
        cdaudio_error (_("Failed to initialize cdio subsystem."));
        return FALSE;
    }

    libcddb_init ();

    return TRUE;
}

/* thread safe (mutex may be locked) */
static int cdaudio_is_our_file (const char * filename, VFSFile * file)
{
    return !strncmp (filename, "cdda://", 7);
}

/* thread safe (mutex may be locked) */
static void cdaudio_set_strinfo (trackinfo_t * t,
                                 const char * performer, const char * name,
                                 const char * genre)
{
    g_strlcpy (t->performer, performer ? performer : "", DEF_STRING_LEN);
    g_strlcpy (t->name, name ? name : "", DEF_STRING_LEN);
    g_strlcpy (t->genre, genre ? genre : "", DEF_STRING_LEN);
}

/* thread safe (mutex may be locked) */
static void cdaudio_set_fullinfo (trackinfo_t * t,
                                  const lsn_t startlsn, const lsn_t endlsn,
                                  const char * performer, const char * name,
                                  const char * genre)
{
    t->startlsn = startlsn;
    t->endlsn = endlsn;
    cdaudio_set_strinfo (t, performer, name, genre);
}

/* play thread only */
static bool_t cdaudio_play (InputPlayback * p, const char * name, VFSFile *
 file, int start, int stop, bool_t pause)
{
    pthread_mutex_lock (& mutex);

    if (trackinfo == NULL)
    {
        refresh_trackinfo (TRUE);

        if (trackinfo == NULL)
        {
            pthread_mutex_unlock (& mutex);
            return FALSE;
        }
    }

    bool_t okay = FALSE;
    int trackno = find_trackno_from_filename (name);

    if (trackno < 0)
        cdaudio_error (_("Invalid URI %s."), name);
    else if (trackno < firsttrackno || trackno > lasttrackno)
        cdaudio_error (_("Track %d not found."), trackno);
    else if (! cdda_track_audiop (pcdrom_drive, trackno))
        cdaudio_error (_("Track %d is a data track."), trackno);
    else if (! p->output->open_audio (FMT_S16_LE, 44100, 2))
        cdaudio_error (_("Failed to open audio output."));
    else
        okay = TRUE;

    if (! okay)
    {
        pthread_mutex_unlock (& mutex);
        return FALSE;
    }

    int startlsn = trackinfo[trackno].startlsn;
    int endlsn = trackinfo[trackno].endlsn;

    seek_time = (start > 0) ? start : -1;
    playing = TRUE;

    if (stop >= 0)
        endlsn = MIN (endlsn, startlsn + stop * 75 / 1000);

    if (pause)
        p->output->pause (TRUE);

    p->set_params (p, 1411200, 44100, 2);
    p->set_pb_ready (p);

    int buffer_size = aud_get_int (NULL, "output_buffer_size");
    int speed = aud_get_int ("CDDA", "disc_speed");
    speed = CLAMP (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    int sectors = CLAMP (buffer_size / 2, 50, 250) * speed * 75 / 1000;
    unsigned char buffer[2352 * sectors];
    int currlsn = startlsn;
    int retry_count = 0, skip_count = 0;

    while (playing)
    {
        if (seek_time >= 0)
        {
            p->output->flush (seek_time);
            currlsn = startlsn + (seek_time * 75 / 1000);
            seek_time = -1;
        }

        sectors = MIN (sectors, endlsn + 1 - currlsn);
        if (sectors < 1)
            break;

        /* unlock mutex here to avoid blocking
         * other threads must be careful not to close drive handle */
        pthread_mutex_unlock (& mutex);

        int ret = cdio_read_audio_sectors (pcdrom_drive->p_cdio, buffer,
         currlsn, sectors);

        if (ret == DRIVER_OP_SUCCESS)
            p->output->write_audio (buffer, 2352 * sectors);

        pthread_mutex_lock (& mutex);

        if (ret == DRIVER_OP_SUCCESS)
        {
            currlsn += sectors;
            retry_count = 0;
            skip_count = 0;
        }
        else if (sectors > 16)
        {
            /* maybe a smaller read size will help */
            sectors /= 2;
        }
        else if (retry_count < MAX_RETRIES)
        {
            /* still failed; retry a few times */
            retry_count ++;
        }
        else if (skip_count < MAX_SKIPS)
        {
            /* maybe the disk is scratched; try skipping ahead */
            currlsn = MIN (currlsn + 75, endlsn + 1);
            skip_count ++;
        }
        else
        {
            /* still failed; give it up */
            cdaudio_error (_("Error reading audio CD."));
            break;
        }
    }

    playing = FALSE;

    pthread_mutex_unlock (& mutex);
    return TRUE;
}

/* main thread only */
static void cdaudio_stop (InputPlayback * p)
{
    pthread_mutex_lock (& mutex);
    playing = FALSE;
    p->output->abort_write();
    pthread_mutex_unlock (& mutex);
}

/* main thread only */
static void cdaudio_pause (InputPlayback * p, bool_t pause)
{
    pthread_mutex_lock (& mutex);
    p->output->pause (pause);
    pthread_mutex_unlock (& mutex);
}

/* main thread only */
static void cdaudio_mseek (InputPlayback * p, int time)
{
    pthread_mutex_lock (& mutex);
    seek_time = time;
    p->output->abort_write();
    pthread_mutex_unlock (& mutex);
}

/* main thread only */
static void cdaudio_cleanup (void)
{
    pthread_mutex_lock (& mutex);

    if (monitor_source)
    {
        g_source_remove (monitor_source);
        monitor_source = 0;
    }

    if (pcdrom_drive != NULL)
    {
        cdda_close (pcdrom_drive);
        pcdrom_drive = NULL;
    }

    if (trackinfo != NULL)
    {
        g_free (trackinfo);
        trackinfo = NULL;
    }

    libcddb_shutdown ();

    pthread_mutex_unlock (& mutex);
}

/* thread safe */
static Tuple * make_tuple (const char * filename, VFSFile * file)
{
    Tuple *tuple = NULL;
    int trackno;

    pthread_mutex_lock (& mutex);

    if (trackinfo == NULL)
        refresh_trackinfo (TRUE);
    if (trackinfo == NULL)
        goto DONE;

    if (!strcmp (filename, "cdda://"))
    {
        tuple = tuple_new_from_filename (filename);

        int subtunes[n_audio_tracks];
        int i = 0;

        /* only add the audio tracks to the playlist */
        for (trackno = firsttrackno; trackno <= lasttrackno; trackno++)
            if (cdda_track_audiop (pcdrom_drive, trackno))
                subtunes[i ++] = trackno;

        tuple_set_subtunes (tuple, n_audio_tracks, subtunes);

        goto DONE;
    }

    trackno = find_trackno_from_filename (filename);

    if (trackno < firsttrackno || trackno > lasttrackno)
    {
        warn ("Track %d not found.\n", trackno);
        goto DONE;
    }

    if (!cdda_track_audiop (pcdrom_drive, trackno))
    {
        warn ("Track %d is a data track.\n", trackno);
        goto DONE;
    }

    tuple = tuple_new_from_filename (filename);
    tuple_set_format (tuple, _("Audio CD"), 2, 44100, 1411);
    tuple_set_int (tuple, FIELD_TRACK_NUMBER, NULL, trackno);
    tuple_set_int (tuple, FIELD_LENGTH, NULL, calculate_track_length
     (trackinfo[trackno].startlsn, trackinfo[trackno].endlsn));

    if (trackinfo[trackno].performer[0])
        tuple_set_str (tuple, FIELD_ARTIST, NULL, trackinfo[trackno].performer);
    if (trackinfo[0].name[0])
        tuple_set_str (tuple, FIELD_ALBUM, NULL, trackinfo[0].name);
    if (trackinfo[trackno].name[0])
        tuple_set_str (tuple, FIELD_TITLE, NULL, trackinfo[trackno].name);
    if (trackinfo[trackno].genre[0])
        tuple_set_str (tuple, FIELD_GENRE, NULL, trackinfo[trackno].genre);

  DONE:
    pthread_mutex_unlock (& mutex);
    return tuple;
}

/* mutex must be locked */
static void open_cd (void)
{
    AUDDBG ("Opening CD drive.\n");
    g_return_if_fail (pcdrom_drive == NULL);

    char * device = aud_get_string ("CDDA", "device");

    if (device[0])
    {
        if (! (pcdrom_drive = cdda_identify (device, 1, NULL)))
            cdaudio_error (_("Failed to open CD device %s."), device);
    }
    else
    {
        char * * ppcd_drives = cdio_get_devices_with_cap (NULL, CDIO_FS_AUDIO, FALSE);

        if (ppcd_drives && ppcd_drives[0])
        {
            if (! (pcdrom_drive = cdda_identify (ppcd_drives[0], 1, NULL)))
                cdaudio_error (_("Failed to open CD device %s."), ppcd_drives[0]);
        }
        else
            cdaudio_error (_("No audio capable CD drive found."));

        if (ppcd_drives)
            cdio_free_device_list (ppcd_drives);
    }

    free (device);
}

/* mutex must be locked */
static void scan_cd (void)
{
    AUDDBG ("Scanning CD drive.\n");
    g_return_if_fail (pcdrom_drive != NULL);
    g_return_if_fail (trackinfo == NULL);

    int trackno;

    /* general track initialization */

    /* skip endianness detection (because it only affects cdda_read, and we use
     * cdio_read_audio_sectors instead) */
    pcdrom_drive->bigendianp = 0;

    /* finish initialization of drive/disc (performs disc TOC sanitization) */
    if (cdda_open (pcdrom_drive) != 0)
    {
        cdaudio_error (_("Failed to finish initializing opened CD drive."));
        goto ERR;
    }

    int speed = aud_get_int ("CDDA", "disc_speed");
    speed = CLAMP (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    if (cdda_speed_set (pcdrom_drive, speed) != DRIVER_OP_SUCCESS)
        warn ("Cannot set drive speed.\n");

    firsttrackno = cdio_get_first_track_num (pcdrom_drive->p_cdio);
    lasttrackno = cdio_get_last_track_num (pcdrom_drive->p_cdio);
    if (firsttrackno == CDIO_INVALID_TRACK || lasttrackno == CDIO_INVALID_TRACK)
    {
        cdaudio_error (_("Failed to retrieve first/last track number."));
        goto ERR;
    }
    AUDDBG ("first track is %d and last track is %d\n", firsttrackno,
           lasttrackno);

    trackinfo = (trackinfo_t *) g_new (trackinfo_t, (lasttrackno + 1));

    cdaudio_set_fullinfo (&trackinfo[0],
                          cdda_track_firstsector (pcdrom_drive, 0),
                          cdda_track_lastsector (pcdrom_drive, lasttrackno),
                          "", "", "");

    n_audio_tracks = 0;

    for (trackno = firsttrackno; trackno <= lasttrackno; trackno++)
    {
        cdaudio_set_fullinfo (&trackinfo[trackno],
                              cdda_track_firstsector (pcdrom_drive, trackno),
                              cdda_track_lastsector (pcdrom_drive, trackno),
                              "", "", "");

        if (trackinfo[trackno].startlsn == CDIO_INVALID_LSN
            || trackinfo[trackno].endlsn == CDIO_INVALID_LSN)
        {
            cdaudio_error (_("Cannot read start/end LSN for track %d."), trackno);
            goto ERR;
        }

        /* count how many tracks are audio tracks */
        if (cdda_track_audiop (pcdrom_drive, trackno))
            n_audio_tracks++;
    }

    /* get trackinfo[0] cdtext information (the disc) */
    cdtext_t *pcdtext = NULL;
    if (aud_get_bool ("CDDA", "use_cdtext"))
    {
        AUDDBG ("getting cd-text information for disc\n");
#if LIBCDIO_VERSION_NUM >= 90
        pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio);
        if (pcdtext == NULL)
#else
        pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio, 0);
        if (pcdtext == NULL || pcdtext->field[CDTEXT_TITLE] == NULL)
#endif
        {
            AUDDBG ("no cd-text available for disc\n");
        }
        else
        {
            cdaudio_set_strinfo (&trackinfo[0],
#if LIBCDIO_VERSION_NUM >= 90
                                 cdtext_get(pcdtext, CDTEXT_FIELD_PERFORMER, 0),
                                 cdtext_get(pcdtext, CDTEXT_FIELD_TITLE, 0),
                                 cdtext_get(pcdtext, CDTEXT_FIELD_GENRE, 0));
#else
                                 pcdtext->field[CDTEXT_PERFORMER],
                                 pcdtext->field[CDTEXT_TITLE],
                                 pcdtext->field[CDTEXT_GENRE]);
#endif
        }
    }

    /* get track information from cdtext */
    bool_t cdtext_was_available = FALSE;
    for (trackno = firsttrackno; trackno <= lasttrackno; trackno++)
    {
#if LIBCDIO_VERSION_NUM < 90
        if (aud_get_bool ("CDDA", "use_cdtext"))
        {
            AUDDBG ("getting cd-text information for track %d\n", trackno);
            pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio, trackno);
            if (pcdtext == NULL || pcdtext->field[CDTEXT_PERFORMER] == NULL)
            {
                AUDDBG ("no cd-text available for track %d\n", trackno);
                pcdtext = NULL;
            }
        }
#endif

        if (pcdtext != NULL)
        {
            cdaudio_set_strinfo (&trackinfo[trackno],
#if LIBCDIO_VERSION_NUM >= 90
                                 cdtext_get(pcdtext, CDTEXT_FIELD_PERFORMER, trackno),
                                 cdtext_get(pcdtext, CDTEXT_FIELD_TITLE, trackno),
                                 cdtext_get(pcdtext, CDTEXT_FIELD_GENRE, trackno));
#else
                                 pcdtext->field[CDTEXT_PERFORMER],
                                 pcdtext->field[CDTEXT_TITLE],
                                 pcdtext->field[CDTEXT_GENRE]);
#endif
            cdtext_was_available = TRUE;
        }
        else
        {
            cdaudio_set_strinfo (&trackinfo[trackno], "", "", "");
            snprintf (trackinfo[trackno].name, DEF_STRING_LEN,
                      "Track %d", trackno);
        }
    }

    if (!cdtext_was_available)
    {
        /* initialize de cddb subsystem */
        cddb_conn_t *pcddb_conn = NULL;
        cddb_disc_t *pcddb_disc = NULL;
        cddb_track_t *pcddb_track = NULL;
        lba_t lba;              /* Logical Block Address */

        if (aud_get_bool ("CDDA", "use_cddb"))
        {
            pcddb_conn = cddb_new ();
            if (pcddb_conn == NULL)
                cdaudio_error (_("Failed to create the cddb connection."));
            else
            {
                AUDDBG ("getting CDDB info\n");

                cddb_cache_enable (pcddb_conn);
                // cddb_cache_set_dir(pcddb_conn, "~/.cddbslave");

                char * server = aud_get_string ("CDDA", "cddbserver");
                char * path = aud_get_string ("CDDA", "cddbpath");
                int port = aud_get_int ("CDDA", "cddbport");

                if (aud_get_bool (NULL, "use_proxy"))
                {
                    char * prhost = aud_get_string (NULL, "proxy_host");
                    int prport = aud_get_int (NULL, "proxy_port");
                    char * pruser = aud_get_string (NULL, "proxy_user");
                    char * prpass = aud_get_string (NULL, "proxy_pass");

                    cddb_http_proxy_enable (pcddb_conn);
                    cddb_set_http_proxy_server_name (pcddb_conn, prhost);
                    cddb_set_http_proxy_server_port (pcddb_conn, prport);
                    cddb_set_http_proxy_username (pcddb_conn, pruser);
                    cddb_set_http_proxy_password (pcddb_conn, prpass);

                    free (prhost);
                    free (pruser);
                    free (prpass);

                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                }
                else if (aud_get_bool ("CDDA", "cddbhttp"))
                {
                    cddb_http_enable (pcddb_conn);
                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                    cddb_set_http_path_query (pcddb_conn, path);
                }
                else
                {
                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                }

                free (server);
                free (path);

                pcddb_disc = cddb_disc_new ();

                lba = cdio_get_track_lba (pcdrom_drive->p_cdio,
                                          CDIO_CDROM_LEADOUT_TRACK);
                cddb_disc_set_length (pcddb_disc, FRAMES_TO_SECONDS (lba));

                for (trackno = firsttrackno; trackno <= lasttrackno; trackno++)
                {
                    pcddb_track = cddb_track_new ();
                    cddb_track_set_frame_offset (pcddb_track,
                                                 cdio_get_track_lba (
                                                     pcdrom_drive->p_cdio,
                                                     trackno));
                    cddb_disc_add_track (pcddb_disc, pcddb_track);
                }

                cddb_disc_calc_discid (pcddb_disc);

#if DEBUG
                guint discid = cddb_disc_get_discid (pcddb_disc);
                AUDDBG ("CDDB disc id = %x\n", discid);
#endif

                int matches;
                if ((matches = cddb_query (pcddb_conn, pcddb_disc)) == -1)
                {
                    if (cddb_errno (pcddb_conn) == CDDB_ERR_OK)
                        cdaudio_error (_("Failed to query the CDDB server"));
                    else
                        cdaudio_error (_("Failed to query the CDDB server: %s"),
                                       cddb_error_str (cddb_errno
                                                       (pcddb_conn)));

                    cddb_disc_destroy (pcddb_disc);
                    pcddb_disc = NULL;
                }
                else
                {
                    if (matches == 0)
                    {
                        AUDDBG ("no cddb info available for this disc\n");

                        cddb_disc_destroy (pcddb_disc);
                        pcddb_disc = NULL;
                    }
                    else
                    {
                        AUDDBG ("CDDB disc category = \"%s\"\n",
                               cddb_disc_get_category_str (pcddb_disc));

                        cddb_read (pcddb_conn, pcddb_disc);
                        if (cddb_errno (pcddb_conn) != CDDB_ERR_OK)
                        {
                            cdaudio_error (_("Failed to read the cddb info: %s"),
                                           cddb_error_str (cddb_errno
                                                           (pcddb_conn)));
                            cddb_disc_destroy (pcddb_disc);
                            pcddb_disc = NULL;
                        }
                        else
                        {
                            cdaudio_set_strinfo (&trackinfo[0],
                                                 cddb_disc_get_artist
                                                 (pcddb_disc),
                                                 cddb_disc_get_title
                                                 (pcddb_disc),
                                                 cddb_disc_get_genre
                                                 (pcddb_disc));

                            int trackno;
                            for (trackno = firsttrackno; trackno <= lasttrackno;
                                 trackno++)
                            {
                                cddb_track_t *pcddb_track =
                                    cddb_disc_get_track (pcddb_disc,
                                                         trackno - 1);
                                cdaudio_set_strinfo (&trackinfo[trackno],
                                                     cddb_track_get_artist
                                                     (pcddb_track),
                                                     cddb_track_get_title
                                                     (pcddb_track),
                                                     cddb_disc_get_genre
                                                     (pcddb_disc));
                            }
                        }
                    }
                }
            }
        }

        if (pcddb_disc != NULL)
            cddb_disc_destroy (pcddb_disc);

        if (pcddb_conn != NULL)
            cddb_destroy (pcddb_conn);
    }

    return;

  ERR:
    g_free (trackinfo);
    trackinfo = NULL;
}

/* mutex must be locked */
static void refresh_trackinfo (bool_t warning)
{
    trigger_monitor ();

    if (pcdrom_drive == NULL)
    {
        open_cd ();
        if (pcdrom_drive == NULL)
            return;
    }

    int mode = cdio_get_discmode (pcdrom_drive->p_cdio);
#ifdef _WIN32 /* cdio_get_discmode reports the wrong disk type sometimes */
    if (mode == CDIO_DISC_MODE_NO_INFO || mode == CDIO_DISC_MODE_ERROR)
#else
    if (mode != CDIO_DISC_MODE_CD_DA && mode != CDIO_DISC_MODE_CD_MIXED)
#endif
    {
        if (warning)
        {
            if (mode == CDIO_DISC_MODE_NO_INFO)
                cdaudio_error (_("Drive is empty."));
            else
                cdaudio_error (_("Unsupported disk type."));
        }

        /* reset libcdio, else it will not read a new disk correctly */
        if (pcdrom_drive)
        {
            cdda_close (pcdrom_drive);
            pcdrom_drive = NULL;
        }

        g_free (trackinfo);
        trackinfo = NULL;
        return;
    }

    if (trackinfo == NULL || cdio_get_media_changed (pcdrom_drive->p_cdio))
    {
        g_free (trackinfo);
        trackinfo = NULL;
        scan_cd ();
    }
}

/* thread safe (mutex may be locked) */
static int calculate_track_length (int startlsn, int endlsn)
{
    return ((endlsn - startlsn + 1) * 1000) / 75;
}

/* thread safe (mutex may be locked) */
static int find_trackno_from_filename (const char * filename)
{
    int track;

    if (strncmp (filename, "cdda://?", 8) || sscanf (filename + 8, "%d",
                                                     &track) != 1)
        return -1;

    return track;
}
