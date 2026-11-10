#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <glib/gstdio.h>

#ifndef GNUCHAN_SOUND_RECORD_VERSION
#define GNUCHAN_SOUND_RECORD_VERSION "dev"
#endif

typedef struct {
    GtkWidget *window;
    GtkWidget *source_combo;
    GtkWidget *format_combo;
    GtkWidget *rate_combo;
    GtkWidget *depth_combo;
    GtkWidget *quality_combo;
    GtkWidget *gain_scale;
    GtkWidget *meter;
    GtkWidget *duration_label;
    GtkWidget *status_label;
    GtkWidget *record_button;
    GtkWidget *pause_button;
    GtkWidget *stop_button;
    GtkWidget *play_button;
    GtkWidget *rename_button;
    GtkWidget *delete_button;
    GtkWidget *directory_entry;
    GtkWidget *browse_button;
    GtkWidget *recordings_list;
    GtkWidget *waveform;
    GtkWidget *depth_label;
    GtkWidget *quality_label;
    GtkListStore *sources;
    GSubprocess *capture;
    GInputStream *capture_stream;
    GSubprocess *player;
    FILE *capture_file;
    GArray *wave_points;
    char *working_path;
    char *final_path;
    char *selected_path;
    char *playback_temp;
    char *config_path;
    char *encoder_output;
    guint timer_id;
    guint64 frames_written;
    guint frames_to_wave;
    guint64 data_bytes;
    guint sample_rate;
    guint bit_depth;
    guint8 pending[4];
    guint pending_length;
    gboolean recording;
    gboolean paused;
    gboolean stop_requested;
    gboolean stream_eof;
    gboolean child_exited;
    gdouble gain;
    gdouble meter_level;
    gboolean have_lame;
    gboolean have_vorbis;
    gboolean have_flac;
    gboolean have_mp3_muxer;
    gboolean have_ogg_muxer;
    gboolean have_flac_muxer;
    gboolean have_ffmpeg;
    gboolean have_ffplay;
    gboolean have_paplay;
    gboolean have_pactl;
    gboolean have_parec;
    gboolean depth_pcm;
    gboolean quality_supported;
    gboolean ui_ready;
} App;

static App app;
static gboolean refresh_sources(void);

static gboolean program_exists(const char *name) {
    char *path = g_find_program_in_path(name);
    gboolean found = path != NULL;
    g_free(path);
    return found;
}

static void set_status(const char *text) {
    gtk_label_set_text(GTK_LABEL(app.status_label), text ? text : "");
}

static void on_refresh_sources_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    refresh_sources();
}

static guint active_uint(GtkWidget *combo, guint fallback) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    if (!id || !*id) return fallback;
    char *end = NULL;
    guint64 value = g_ascii_strtoull(id, &end, 10);
    return end && *end == '\0' && value > 0 && value <= G_MAXUINT
        ? (guint)value : fallback;
}

static gboolean contains_encoder(const char *output, const char *encoder) {
    if (!output) return FALSE;
    char **lines = g_strsplit(output, "\n", -1);
    gboolean found = FALSE;
    for (guint i = 0; lines[i] && !found; ++i) {
        char **tokens = g_strsplit_set(lines[i], " \t\r", -1);
        for (guint j = 0; tokens[j]; ++j) {
            if (g_str_equal(tokens[j], encoder)) {
                found = TRUE;
                break;
            }
        }
        g_strfreev(tokens);
    }
    g_strfreev(lines);
    return found;
}

static gboolean run_capture(char **argv, char **stdout_text, char **stderr_text,
                            gint *status, GError **error) {
    return g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                        stdout_text, stderr_text, status, error);
}

static void detect_dependencies(void) {
    app.have_pactl = program_exists("pactl");
    app.have_parec = program_exists("parec");
    app.have_ffmpeg = program_exists("ffmpeg");
    app.have_ffplay = program_exists("ffplay");
    app.have_paplay = program_exists("paplay");
    if (!app.have_ffmpeg) return;
    char *argv[] = {"ffmpeg", "-hide_banner", "-encoders", NULL};
    char *out = NULL, *err = NULL;
    gint status = 0;
    GError *error = NULL;
    if (run_capture(argv, &out, &err, &status, &error) && status == 0) {
        char *combined = g_strconcat(out ? out : "", "\n", err ? err : "", NULL);
        app.encoder_output = combined;
        app.have_lame = contains_encoder(combined, "libmp3lame");
        app.have_vorbis = contains_encoder(combined, "libvorbis");
        app.have_flac = contains_encoder(combined, "flac");
    } else {
        app.have_ffmpeg = FALSE;
    }
    g_free(out);
    g_free(err);
    g_clear_error(&error);
    if (!app.have_ffmpeg) return;
    char *mux_argv[] = {"ffmpeg", "-hide_banner", "-muxers", NULL};
    out = NULL;
    err = NULL;
    status = 0;
    if (run_capture(mux_argv, &out, &err, &status, &error) && status == 0) {
        char *combined = g_strconcat(out ? out : "", "\n", err ? err : "", NULL);
        app.have_mp3_muxer = contains_encoder(combined, "mp3");
        app.have_ogg_muxer = contains_encoder(combined, "ogg");
        app.have_flac_muxer = contains_encoder(combined, "flac");
        g_free(combined);
    }
    g_free(out);
    g_free(err);
    g_clear_error(&error);
}

static void add_format(const char *id, const char *label) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.format_combo), id, label);
}

static void populate_formats(void) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.format_combo));
    add_format("wav", "WAV · PCM");
    if (app.have_lame && app.have_mp3_muxer) add_format("mp3", "MP3 · libmp3lame");
    if (app.have_vorbis && app.have_ogg_muxer) add_format("ogg", "OGG · libvorbis");
    if (app.have_flac && app.have_flac_muxer) add_format("flac", "FLAC");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.format_combo), "wav");
}

static void update_depth_controls(void) {
    const char *format = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.format_combo));
    const char *current_depth = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.depth_combo));
    if (current_depth) app.bit_depth = (guint)g_ascii_strtoull(current_depth, NULL, 10);
    gboolean pcm = g_strcmp0(format, "wav") == 0 || g_strcmp0(format, "flac") == 0;
    app.depth_pcm = pcm;
    gtk_widget_set_sensitive(app.depth_combo, pcm);
    if (g_strcmp0(format, "flac") == 0) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.depth_combo));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), "16", "16-bit");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), "24", "24-bit");
        if (app.bit_depth == 32) app.bit_depth = 24;
    } else if (g_strcmp0(format, "wav") == 0) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.depth_combo));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), "16", "16-bit");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), "24", "24-bit");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), "32", "32-bit");
    }
    if (pcm) {
        char id[8];
        g_snprintf(id, sizeof id, "%u", app.bit_depth);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.depth_combo), id);
    }
    gtk_label_set_text(GTK_LABEL(app.depth_label),
        pcm ? "PCM bit depth" : "PCM bit depth (not applicable to lossy codecs)");
    gboolean has_quality = format && g_strcmp0(format, "wav") != 0;
    app.quality_supported = has_quality;
    gtk_widget_set_sensitive(app.quality_combo, has_quality);
    gtk_label_set_text(GTK_LABEL(app.quality_label),
        has_quality ? "Encoding quality" : "Encoding quality (not used by WAV)");
}

static gboolean is_monitor_source(const char *name) {
    return name && g_str_has_suffix(name, ".monitor");
}

static gboolean refresh_sources(void) {
    gtk_list_store_clear(app.sources);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app.source_combo));
    if (!app.have_pactl) {
        set_status("Microphone selection unavailable: install pactl (pulseaudio-utils).");
        return FALSE;
    }
    char *argv[] = {"pactl", "list", "short", "sources", NULL};
    char *out = NULL, *err = NULL;
    gint status = 0;
    GError *error = NULL;
    if (!run_capture(argv, &out, &err, &status, &error) || status != 0) {
        char *msg = g_strdup_printf("Could not query audio sources: %s",
            error ? error->message : (err && *err ? err : "audio server unavailable"));
        set_status(msg);
        g_free(msg);
        g_clear_error(&error);
        g_free(out);
        g_free(err);
        return FALSE;
    }
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    guint count = 0;
    for (guint i = 0; lines[i]; ++i) {
        char **parts = g_strsplit_set(lines[i], " \t", -1);
        GPtrArray *columns = g_ptr_array_new();
        for (guint j = 0; parts[j]; ++j)
            if (*parts[j]) g_ptr_array_add(columns, parts[j]);
        if (columns->len >= 2) {
            const char *name = g_ptr_array_index(columns, 1);
            if (!is_monitor_source(name)) {
                GtkTreeIter iter;
                gtk_list_store_append(app.sources, &iter);
                gtk_list_store_set(app.sources, &iter, 0, name, 1, name, -1);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.source_combo),
                                          name, name);
                ++count;
            }
        }
        g_ptr_array_free(columns, TRUE);
        g_strfreev(parts);
    }
    g_strfreev(lines);
    if (count) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.source_combo), 0);
        set_status("Ready. Choose a microphone and recording settings.");
    } else {
        set_status("No microphone sources found. Connect a microphone and refresh.");
    }
    g_free(out);
    g_free(err);
    g_clear_error(&error);
    return count > 0;
}

static void write_u16_le(FILE *file, guint16 value) {
    fputc(value & 0xff, file);
    fputc((value >> 8) & 0xff, file);
}

static void write_u32_le(FILE *file, guint32 value) {
    write_u16_le(file, value & 0xffff);
    write_u16_le(file, value >> 16);
}

static void write_wave_header(FILE *file, guint sample_rate, guint bit_depth,
                              guint32 data_size) {
    const guint block_align = bit_depth / 8;
    fseek(file, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, file);
    write_u32_le(file, 36 + data_size);
    fwrite("WAVEfmt ", 1, 8, file);
    write_u32_le(file, 16);
    write_u16_le(file, 1);
    write_u16_le(file, 1);
    write_u32_le(file, sample_rate);
    write_u32_le(file, sample_rate * block_align);
    write_u16_le(file, block_align);
    write_u16_le(file, bit_depth);
    fwrite("data", 1, 4, file);
    write_u32_le(file, data_size);
    fseek(file, 0, SEEK_END);
}

static gboolean write_sample(FILE *file, gint64 sample, guint depth) {
    if (depth == 16) {
        write_u16_le(file, (guint16)(gint16)sample);
        return !ferror(file);
    }
    if (depth == 24) {
        guint32 value = ((guint32)sample) & 0x00ffffff;
        fputc(value & 0xff, file);
        fputc((value >> 8) & 0xff, file);
        fputc((value >> 16) & 0xff, file);
        return !ferror(file);
    }
    write_u32_le(file, (guint32)(gint32)sample);
    return !ferror(file);
}

static gboolean convert_recording(const char *input, const char *output,
                                  const char *format, guint rate,
                                  guint quality, char **message) {
    if (g_str_equal(format, "wav")) {
        if (g_rename(input, output) == 0) return TRUE;
        *message = g_strdup_printf("Could not finalize WAV: %s", g_strerror(errno));
        return FALSE;
    }
    const char *codec = NULL;
    if (g_str_equal(format, "mp3")) codec = "libmp3lame";
    else if (g_str_equal(format, "ogg")) codec = "libvorbis";
    else if (g_str_equal(format, "flac")) codec = "flac";
    gboolean encoder_available =
        (g_str_equal(format, "mp3") && app.have_lame && app.have_mp3_muxer) ||
        (g_str_equal(format, "ogg") && app.have_vorbis && app.have_ogg_muxer) ||
        (g_str_equal(format, "flac") && app.have_flac && app.have_flac_muxer);
    if (!app.have_ffmpeg || !codec || !encoder_available ||
        !contains_encoder(app.encoder_output, codec)) {
        *message = g_strdup_printf("Requested %s encoder is unavailable; no format fallback was used.",
                                   format);
        return FALSE;
    }
    char rate_s[16], quality_s[16];
    g_snprintf(rate_s, sizeof rate_s, "%u", rate);
    const char *extension_format = g_str_equal(format, "ogg") ? "ogg" : format;
    char *temporary_output = g_strconcat(output, ".part", NULL);
    char *argv[32];
    guint i = 0;
    argv[i++] = "ffmpeg"; argv[i++] = "-nostdin"; argv[i++] = "-hide_banner";
    argv[i++] = "-loglevel"; argv[i++] = "error"; argv[i++] = "-y";
    argv[i++] = "-f"; argv[i++] = "wav"; argv[i++] = "-i"; argv[i++] = (char *)input;
    argv[i++] = "-ar"; argv[i++] = rate_s; argv[i++] = "-c:a"; argv[i++] = (char *)codec;
    if (g_str_equal(format, "mp3")) {
        const char *bitrates[] = {"128k", "192k", "320k"};
        argv[i++] = "-b:a"; argv[i++] = (char *)bitrates[MIN(quality, 2)];
    } else if (g_str_equal(format, "ogg")) {
        const char *levels[] = {"3", "5", "8"};
        argv[i++] = "-q:a"; argv[i++] = (char *)levels[MIN(quality, 2)];
    } else {
        const char *levels[] = {"0", "5", "8"};
        argv[i++] = "-compression_level"; argv[i++] = (char *)levels[MIN(quality, 2)];
    }
    g_snprintf(quality_s, sizeof quality_s, "%s", extension_format);
    argv[i++] = "-f"; argv[i++] = quality_s; argv[i++] = temporary_output; argv[i] = NULL;
    char *out = NULL, *err = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean spawned = run_capture(argv, &out, &err, &status, &error);
    gboolean success = spawned && status == 0 &&
                       g_rename(temporary_output, output) == 0;
    if (!success) {
        g_unlink(temporary_output);
        *message = g_strdup_printf("Encoding failed: %s",
            error ? error->message : (err && *err ? err : "ffmpeg returned an error"));
    }
    g_free(temporary_output);
    g_free(out);
    g_free(err);
    g_clear_error(&error);
    return success;
}

static char *selected_format(void) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.format_combo));
    return g_strdup(id ? id : "wav");
}

static char *make_recording_path(const char *directory, const char *format) {
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "Recording-%Y%m%d-%H%M%S");
    g_date_time_unref(now);
    char *base = g_strdup_printf("%s.%s", stamp, format);
    char *path = g_build_filename(directory, base, NULL);
    guint suffix = 1;
    while (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(base);
        g_free(path);
        base = g_strdup_printf("%s-%u.%s", stamp, suffix++, format);
        path = g_build_filename(directory, base, NULL);
    }
    g_free(stamp);
    g_free(base);
    return path;
}

static void refresh_recordings(void);
static void update_buttons(void);
static void finish_recording(void);

static void child_waited(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    GError *error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    app.child_exited = TRUE;
    g_clear_error(&error);
    if (app.stop_requested && app.stream_eof) finish_recording();
}

static void start_wait_for_capture(void) {
    if (!app.capture || app.child_exited) return;
    g_subprocess_wait_async(app.capture, NULL, child_waited, NULL);
}

static void finalize_capture_file(void) {
    if (!app.capture_file) return;
    write_wave_header(app.capture_file, app.sample_rate, app.bit_depth,
                      (guint32)MIN(app.data_bytes, G_MAXUINT32 - 36));
    fclose(app.capture_file);
    app.capture_file = NULL;
}

static void finish_recording(void) {
    if (!app.stop_requested || !app.stream_eof || !app.child_exited) return;
    app.recording = FALSE;
    app.paused = FALSE;
    if (app.timer_id) {
        g_source_remove(app.timer_id);
        app.timer_id = 0;
    }
    g_clear_object(&app.capture_stream);
    g_clear_object(&app.capture);
    finalize_capture_file();
    if (!app.working_path || !app.final_path) {
        set_status("Recording stopped.");
    } else if (!app.data_bytes) {
        g_unlink(app.working_path);
        set_status("No audio samples were received; empty recording discarded.");
    } else {
        char *format = selected_format();
        char *message = NULL;
        guint quality = gtk_combo_box_get_active(GTK_COMBO_BOX(app.quality_combo));
        gboolean success = convert_recording(app.working_path, app.final_path,
                    format, app.sample_rate, quality, &message);
        if (success) {
            char *text = g_strdup_printf("Saved %s", app.final_path);
            set_status(text);
            g_free(text);
            g_free(app.selected_path);
            app.selected_path = g_strdup(app.final_path);
        } else {
            char *text = g_strdup_printf("%s Temporary PCM retained at %s",
                    message ? message : "Could not save recording.",
                    app.working_path);
            set_status(text);
            g_free(text);
        }
        g_free(format);
        g_free(message);
        refresh_recordings();
    }
    g_clear_pointer(&app.working_path, g_free);
    g_clear_pointer(&app.final_path, g_free);
    app.stop_requested = FALSE;
    app.stream_eof = FALSE;
    app.child_exited = FALSE;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.meter), 0.0);
    update_buttons();
}

static gboolean append_captured_samples(const guint8 *data, gsize length) {
    const guint sample_bytes = app.bit_depth / 8;
    gsize offset = 0;
    while (offset < length || app.pending_length == sample_bytes) {
        while (app.pending_length < sample_bytes && offset < length)
            app.pending[app.pending_length++] = data[offset++];
        if (app.pending_length < sample_bytes) break;
        gint64 value;
        if (app.bit_depth == 16) {
            value = (gint16)((guint16)app.pending[0] |
                             ((guint16)app.pending[1] << 8));
        } else if (app.bit_depth == 24) {
            gint32 raw = (gint32)((guint32)app.pending[0] |
                       ((guint32)app.pending[1] << 8) |
                       ((guint32)app.pending[2] << 16));
            if (raw & 0x00800000) raw |= (gint32)0xff000000;
            value = raw;
        } else {
            value = (gint32)((guint32)app.pending[0] |
                      ((guint32)app.pending[1] << 8) |
                      ((guint32)app.pending[2] << 16) |
                      ((guint32)app.pending[3] << 24));
        }
        long double limit = ldexpl(1.0L, app.bit_depth - 1);
        long double scaled = roundl((long double)value * app.gain);
        scaled = MAX(-limit, MIN(limit - 1.0L, scaled));
        gint64 output_sample = (gint64)scaled;
        if (app.data_bytes + sample_bytes > G_MAXUINT32 - 36)
            return FALSE;
        if (!write_sample(app.capture_file, output_sample, app.bit_depth)) return FALSE;
        app.data_bytes += app.bit_depth / 8;
        ++app.frames_written;
        ++app.frames_to_wave;
        app.meter_level = MAX(app.meter_level,
                              (gdouble)fabsl((long double)output_sample / limit));
        if (app.frames_to_wave >= MAX(1, app.sample_rate / 30)) {
            gfloat point = (gfloat)app.meter_level;
            g_array_append_val(app.wave_points, point);
            app.frames_to_wave = 0;
            app.meter_level = 0.0;
        }
        app.pending_length = 0;
    }
    return !ferror(app.capture_file);
}

static gboolean poll_capture(gpointer data) {
    (void)data;
    if (!app.capture_stream) return G_SOURCE_CONTINUE;
    guint8 buffer[65536];
    gboolean got_data = FALSE;
    for (guint i = 0; i < 16; ++i) {
        GError *error = NULL;
        gssize n = g_pollable_input_stream_read_nonblocking(
            G_POLLABLE_INPUT_STREAM(app.capture_stream), buffer, sizeof buffer,
            NULL, &error);
        if (n > 0) {
            got_data = TRUE;
            if (!app.paused && app.capture_file &&
                !append_captured_samples(buffer, (gsize)n)) {
                set_status("WAV size limit reached or disk write failed; stopping recording.");
                app.stop_requested = TRUE;
                if (app.capture) g_subprocess_force_exit(app.capture);
                start_wait_for_capture();
            }
            continue;
        }
        if (n == 0) {
            app.stream_eof = TRUE;
            if (!app.stop_requested) {
                app.stop_requested = TRUE;
                if (app.capture) start_wait_for_capture();
                set_status("Audio capture ended unexpectedly.");
            }
            break;
        }
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_clear_error(&error);
            break;
        }
        if (error) {
            char *text = g_strdup_printf("Audio capture read failed: %s", error->message);
            set_status(text);
            g_free(text);
            g_clear_error(&error);
            app.stream_eof = TRUE;
            app.stop_requested = TRUE;
            if (app.capture) {
                g_subprocess_force_exit(app.capture);
                start_wait_for_capture();
            }
            break;
        }
    }
    if (app.recording) {
        guint64 seconds = app.frames_written / MAX(1, app.sample_rate);
        char *text = g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
                                      seconds / 60, seconds % 60);
        gtk_label_set_text(GTK_LABEL(app.duration_label), text);
        g_free(text);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.meter),
                                      MIN(app.meter_level, 1.0));
        gtk_widget_queue_draw(app.waveform);
    }
    if ((app.stop_requested && app.stream_eof && app.child_exited) ||
        (!app.recording && !app.capture_stream)) {
        finish_recording();
        return G_SOURCE_REMOVE;
    }
    return got_data || app.recording || app.stop_requested
        ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean start_recording(void) {
    if (app.recording || app.stop_requested) return FALSE;
    if (!app.have_pactl || !app.have_parec) {
        set_status("Recording needs pactl and parec (install pulseaudio-utils).");
        return FALSE;
    }
    const char *source = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.source_combo));
    if (!source || !*source) {
        set_status("Select an available microphone before recording.");
        return FALSE;
    }
    const char *directory = gtk_entry_get_text(GTK_ENTRY(app.directory_entry));
    if (!directory || !*directory || g_mkdir_with_parents(directory, 0755) != 0) {
        set_status("Choose a writable output folder.");
        return FALSE;
    }
    char *format = selected_format();
    app.sample_rate = active_uint(app.rate_combo, 48000);
    app.bit_depth = active_uint(app.depth_combo, 16);
    app.gain = gtk_range_get_value(GTK_RANGE(app.gain_scale)) / 100.0;
    app.final_path = make_recording_path(directory, format);
    app.working_path = g_str_equal(format, "wav")
        ? g_strconcat(app.final_path, ".part", NULL)
        : g_strconcat(app.final_path, ".capture.wav.part", NULL);
    app.capture_file = g_fopen(app.working_path, "wb+");
    if (!app.capture_file) {
        char *text = g_strdup_printf("Could not create recording: %s", g_strerror(errno));
        set_status(text);
        g_free(text);
        g_free(app.final_path); app.final_path = NULL;
        g_free(app.working_path); app.working_path = NULL;
        g_free(format);
        return FALSE;
    }
    write_wave_header(app.capture_file, app.sample_rate, app.bit_depth, 0);
    char rate[16];
    g_snprintf(rate, sizeof rate, "%u", app.sample_rate);
    char pcm_format[16];
    guint capture_depth = app.depth_pcm ? active_uint(app.depth_combo, 16) : 16;
    app.bit_depth = capture_depth;
    g_snprintf(pcm_format, sizeof pcm_format, "--format=s%ule",
               capture_depth);
    char *argv[] = {"parec", "--raw", pcm_format, "--channels=1",
                    "--rate", rate, "--device", (char *)source, NULL};
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    GError *error = NULL;
    app.capture = g_subprocess_launcher_spawnv(launcher,
                         (const gchar * const *)argv, &error);
    g_object_unref(launcher);
    g_free(format);
    if (!app.capture) {
        char *text = g_strdup_printf("Could not start parec: %s",
                                     error ? error->message : "unknown error");
        set_status(text);
        g_free(text);
        g_clear_error(&error);
        fclose(app.capture_file); app.capture_file = NULL;
        g_unlink(app.working_path);
        g_clear_pointer(&app.final_path, g_free);
        g_clear_pointer(&app.working_path, g_free);
        return FALSE;
    }
    app.capture_stream = g_object_ref(g_subprocess_get_stdout_pipe(app.capture));
    app.wave_points = g_array_new(FALSE, FALSE, sizeof(gfloat));
    app.frames_written = 0;
    app.data_bytes = 0;
    app.frames_to_wave = 0;
    app.meter_level = 0.0;
    app.pending_length = 0;
    app.recording = TRUE;
    app.paused = FALSE;
    app.stop_requested = FALSE;
    app.stream_eof = FALSE;
    app.child_exited = FALSE;
    gtk_label_set_text(GTK_LABEL(app.duration_label), "00:00");
    set_status("Recording. Gain is applied in software; your system microphone volume is unchanged.");
    app.timer_id = g_timeout_add(20, poll_capture, NULL);
    update_buttons();
    return TRUE;
}

static void stop_recording(void) {
    if (!app.recording || app.stop_requested) return;
    app.stop_requested = TRUE;
    app.recording = FALSE;
    app.paused = FALSE;
    if (app.capture) {
        g_subprocess_force_exit(app.capture);
        start_wait_for_capture();
    } else {
        app.stream_eof = TRUE;
        app.child_exited = TRUE;
    }
    set_status("Stopping recording and finalizing the audio file…");
    update_buttons();
}

static void toggle_pause(void) {
    if (!app.recording || !app.capture) return;
    const char *identifier = g_subprocess_get_identifier(app.capture);
    long pid = identifier ? strtol(identifier, NULL, 10) : -1;
    if (pid <= 0 || kill((pid_t)pid, app.paused ? SIGCONT : SIGSTOP) != 0) {
        char *text = g_strdup_printf("Could not %s capture: %s",
                            app.paused ? "resume" : "pause", g_strerror(errno));
        set_status(text);
        g_free(text);
        return;
    }
    app.paused = !app.paused;
    set_status(app.paused ? "Recording paused." : "Recording resumed.");
    update_buttons();
}

static gint compare_names(gconstpointer a, gconstpointer b) {
    return -g_strcmp0(*(char * const *)a, *(char * const *)b);
}

static gboolean audio_extension(const char *name) {
    return g_str_has_suffix(name, ".wav") || g_str_has_suffix(name, ".mp3") ||
           g_str_has_suffix(name, ".ogg") || g_str_has_suffix(name, ".flac");
}

static void refresh_recordings(void) {
    char *restore_path = app.selected_path ? g_strdup(app.selected_path) : NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(app.recordings_list));
    for (GList *it = children; it; it = it->next)
        gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);
    const char *directory = gtk_entry_get_text(GTK_ENTRY(app.directory_entry));
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (!dir) return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (audio_extension(name)) g_ptr_array_add(names, g_strdup(name));
    }
    g_dir_close(dir);
    g_ptr_array_sort(names, compare_names);
    for (guint i = 0; i < names->len; ++i) {
        const char *entry = g_ptr_array_index(names, i);
        char *path = g_build_filename(directory, entry, NULL);
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *label = gtk_label_new(entry);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_widget_set_margin_top(label, 5);
        gtk_widget_set_margin_bottom(label, 5);
        gtk_container_add(GTK_CONTAINER(row), label);
        g_object_set_data_full(G_OBJECT(row), "recording-path", path, g_free);
        gtk_list_box_insert(GTK_LIST_BOX(app.recordings_list), row, -1);
        if (restore_path && g_strcmp0(restore_path, path) == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(app.recordings_list),
                                    GTK_LIST_BOX_ROW(row));
    }
    g_ptr_array_free(names, TRUE);
    g_free(restore_path);
    gtk_widget_show_all(app.recordings_list);
}

static void add_wave_sample(gint16 value) {
    gfloat point = fabsf((gfloat)value / 32768.0f);
    g_array_append_val(app.wave_points, point);
}

static void load_waveform(const char *path) {
    if (!app.wave_points) app.wave_points = g_array_new(FALSE, FALSE, sizeof(gfloat));
    g_array_set_size(app.wave_points, 0);
    if (app.have_ffmpeg && !g_str_has_suffix(path, ".wav")) {
        char *argv[] = {"ffmpeg", "-nostdin", "-v", "error", "-i", (char *)path,
                        "-f", "s16le", "-ac", "1", "-ar", "100", "-", NULL};
        GError *error = NULL;
        GSubprocess *decoder = g_subprocess_newv((const gchar * const *)argv,
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
        GBytes *decoded = NULL;
        if (decoder && g_subprocess_communicate(decoder, NULL, NULL, &decoded, NULL,
                                                &error) &&
            g_subprocess_get_successful(decoder) && decoded) {
            gsize output_length = 0;
            const guint8 *pcm = g_bytes_get_data(decoded, &output_length);
            for (gsize i = 0; i + 1 < output_length; i += 2) {
                gint16 value = (gint16)((guint16)pcm[i] |
                                        ((guint16)pcm[i + 1] << 8));
                add_wave_sample(value);
            }
        }
        g_clear_pointer(&decoded, g_bytes_unref);
        g_clear_object(&decoder);
        g_clear_error(&error);
    } else {
        FILE *file = g_fopen(path, "rb");
        if (file) {
            guint8 header[44];
            if (fread(header, 1, sizeof header, file) == sizeof header &&
                memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0 &&
                memcmp(header + 36, "data", 4) == 0) {
                guint bits = header[34] | ((guint)header[35] << 8);
                guint block = bits / 8;
                guint32 bytes = (guint32)header[40] | ((guint32)header[41] << 8) |
                    ((guint32)header[42] << 16) | ((guint32)header[43] << 24);
                if (block && (bits == 16 || bits == 24 || bits == 32)) {
                    guint64 frames = bytes / block;
                    guint64 stride = MAX((guint64)1, frames / 10000);
                    for (guint64 frame = 0; frame < frames; frame += stride) {
                        guint8 b[4] = {0, 0, 0, 0};
                        if (fseek(file, 44 + (long)(frame * block), SEEK_SET) != 0 ||
                            fread(b, 1, block, file) != block) break;
                        gint32 sample;
                        if (bits == 16)
                            sample = (gint16)((guint16)b[0] | ((guint16)b[1] << 8));
                        else if (bits == 24) {
                            sample = (gint32)((guint32)b[0] | ((guint32)b[1] << 8) |
                                               ((guint32)b[2] << 16));
                            if (sample & 0x00800000) sample |= (gint32)0xff000000;
                            sample >>= 8;
                        } else {
                            sample = (gint32)((guint32)b[0] | ((guint32)b[1] << 8) |
                                               ((guint32)b[2] << 16) | ((guint32)b[3] << 24));
                            sample >>= 16;
                        }
                        add_wave_sample((gint16)sample);
                    }
                }
            }
            fclose(file);
        }
    }
    gtk_widget_queue_draw(app.waveform);
}

static gboolean draw_waveform(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const double width = allocation.width, height = allocation.height;
    cairo_set_source_rgb(cr, 0.08, 0.09, 0.12);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.73, 0.35);
    cairo_move_to(cr, 0, height / 2);
    cairo_line_to(cr, width, height / 2);
    cairo_stroke(cr);
    if (!app.wave_points || !app.wave_points->len || width <= 1 || height <= 1)
        return FALSE;
    cairo_set_source_rgb(cr, 0.57, 0.40, 0.94);
    cairo_set_line_width(cr, 1.5);
    double center = height / 2.0;
    guint n = app.wave_points->len;
    for (gint direction = -1; direction <= 1; direction += 2) {
        for (guint x = 0; x < (guint)width; ++x) {
            guint start = (guint)((guint64)x * n / (guint)width);
            guint end = MAX(start + 1, (guint)((guint64)(x + 1) * n / (guint)width));
            end = MIN(end, n);
            gfloat peak = 0;
            for (guint j = start; j < end; ++j)
                peak = MAX(peak, g_array_index(app.wave_points, gfloat, j));
            double y = center + direction * peak * (height * 0.45);
            if (x == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
    return FALSE;
}

static void save_config(void) {
    GKeyFile *key = g_key_file_new();
    g_key_file_set_string(key, "General", "OutputDirectory",
                          gtk_entry_get_text(GTK_ENTRY(app.directory_entry)));
    const char *format = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.format_combo));
    const char *rate = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.rate_combo));
    const char *depth = gtk_combo_box_get_active_id(GTK_COMBO_BOX(app.depth_combo));
    if (format) g_key_file_set_string(key, "General", "Format", format);
    g_key_file_set_string(key, "General", "SampleRate", rate ? rate : "48000");
    g_key_file_set_string(key, "General", "BitDepth", depth ? depth : "16");
    g_key_file_set_integer(key, "General", "Quality",
                           gtk_combo_box_get_active(GTK_COMBO_BOX(app.quality_combo)));
    g_key_file_set_integer(key, "General", "GainPercent",
                           (gint)gtk_range_get_value(GTK_RANGE(app.gain_scale)));
    char *data = g_key_file_to_data(key, NULL, NULL);
    char *parent = g_path_get_dirname(app.config_path);
    g_mkdir_with_parents(parent, 0700);
    g_file_set_contents(app.config_path, data, -1, NULL);
    g_chmod(app.config_path, 0600);
    g_free(parent);
    g_free(data);
    g_key_file_unref(key);
}

static char *default_output_directory(void) {
    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (!music || !*music) music = g_get_home_dir();
    return g_build_filename(music, "GnuChanSoundRecord", NULL);
}

static void load_config(void) {
    GKeyFile *key = g_key_file_new();
    if (g_key_file_load_from_file(key, app.config_path, G_KEY_FILE_NONE, NULL)) {
        char *path = g_key_file_get_string(key, "General", "OutputDirectory", NULL);
        if (path && *path) gtk_entry_set_text(GTK_ENTRY(app.directory_entry), path);
        char *rate = g_key_file_get_string(key, "General", "SampleRate", NULL);
        char *depth = g_key_file_get_string(key, "General", "BitDepth", NULL);
        char *format = g_key_file_get_string(key, "General", "Format", NULL);
        gint quality = g_key_file_has_key(key, "General", "Quality", NULL)
            ? g_key_file_get_integer(key, "General", "Quality", NULL) : 1;
        gint gain = g_key_file_has_key(key, "General", "GainPercent", NULL)
            ? g_key_file_get_integer(key, "General", "GainPercent", NULL) : 100;
        if (rate) gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.rate_combo), rate);
        if (depth) gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.depth_combo), depth);
        if (format && gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.format_combo), format)) {
            update_depth_controls();
        }
        if (quality >= 0 && quality <= 2)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.quality_combo), quality);
        if (gain >= 0 && gain <= 200)
            gtk_range_set_value(GTK_RANGE(app.gain_scale), gain);
        g_free(path); g_free(rate); g_free(depth); g_free(format);
    } else {
        char *path = default_output_directory();
        gtk_entry_set_text(GTK_ENTRY(app.directory_entry), path);
        g_free(path);
    }
    g_key_file_unref(key);
}

static void update_buttons(void) {
    if (!app.ui_ready) return;
    gtk_widget_set_sensitive(app.record_button, !app.recording && !app.stop_requested);
    gtk_widget_set_sensitive(app.pause_button, app.recording);
    gtk_button_set_label(GTK_BUTTON(app.pause_button), app.paused ? "Resume" : "Pause");
    gtk_widget_set_sensitive(app.stop_button, app.recording);
    gboolean selected = app.selected_path != NULL;
    gboolean can_play = selected && ((g_str_has_suffix(app.selected_path, ".wav") &&
        (app.have_paplay || app.have_ffplay)) || app.have_ffplay);
    gtk_widget_set_sensitive(app.play_button, can_play && !app.recording);
    gtk_button_set_label(GTK_BUTTON(app.play_button), app.player ? "Stop playback" : "Play");
    gtk_widget_set_sensitive(app.rename_button, selected && !app.recording);
    gtk_widget_set_sensitive(app.delete_button, selected && !app.recording);
    gtk_widget_set_sensitive(app.source_combo, !app.recording);
    gtk_widget_set_sensitive(app.format_combo, !app.recording);
    gtk_widget_set_sensitive(app.rate_combo, !app.recording);
    gtk_widget_set_sensitive(app.depth_combo, !app.recording && app.depth_pcm);
    gtk_widget_set_sensitive(app.quality_combo,
                             !app.recording && app.quality_supported);
    gtk_widget_set_sensitive(app.directory_entry, !app.recording);
    gtk_widget_set_sensitive(app.browse_button, !app.recording);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    g_free(app.selected_path);
    app.selected_path = row ? g_strdup(g_object_get_data(G_OBJECT(row), "recording-path")) : NULL;
    if (app.selected_path) load_waveform(app.selected_path);
    else if (app.wave_points) {
        g_array_set_size(app.wave_points, 0);
        gtk_widget_queue_draw(app.waveform);
    }
    update_buttons();
}

static void on_record_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    if (!start_recording()) update_buttons();
}

static void on_pause_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    toggle_pause();
}

static void on_stop_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    stop_recording();
}

static void playback_waited(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    GError *error = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
    g_clear_error(&error);
    g_clear_object(&app.player);
    if (app.playback_temp) g_unlink(app.playback_temp);
    g_clear_pointer(&app.playback_temp, g_free);
    update_buttons();
}

static void stop_playback(void) {
    if (app.player) g_subprocess_force_exit(app.player);
}

static void play_selected(void) {
    if (!app.selected_path) return;
    if (app.player) {
        stop_playback();
        return;
    }
    gboolean is_wav = g_str_has_suffix(app.selected_path, ".wav");
    const char *program = is_wav && app.have_paplay ? "paplay" :
                          (app.have_ffplay ? "ffplay" : NULL);
    if (!program) {
        set_status("Playback unavailable: install paplay for WAV or ffplay for compressed formats.");
        return;
    }
    char *argv[8];
    if (g_str_equal(program, "paplay")) {
        argv[0] = "paplay"; argv[1] = app.selected_path; argv[2] = NULL;
    } else {
        argv[0] = "ffplay"; argv[1] = "-nodisp"; argv[2] = "-autoexit";
        argv[3] = "-loglevel"; argv[4] = "error"; argv[5] = app.selected_path;
        argv[6] = NULL;
    }
    GError *error = NULL;
    app.player = g_subprocess_newv((const gchar * const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (!app.player) {
        char *text = g_strdup_printf("Could not start playback: %s",
                                     error ? error->message : "unknown error");
        set_status(text);
        g_free(text);
        g_clear_error(&error);
        return;
    }
    g_subprocess_wait_async(app.player, NULL, playback_waited, NULL);
    set_status("Playing selected recording.");
    update_buttons();
}

static void on_play_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    play_selected();
}

static void on_rename_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    if (!app.selected_path) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename recording",
        GTK_WINDOW(app.window), GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    char *base = g_path_get_basename(app.selected_path);
    gtk_entry_set_text(GTK_ENTRY(entry), base);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 10);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        char *directory = g_path_get_dirname(app.selected_path);
        char *new_path = g_build_filename(directory, new_name, NULL);
        char *new_basename = g_path_get_basename(new_name);
        char *old_basename = g_path_get_basename(app.selected_path);
        if (!*new_name || g_strcmp0(new_name, new_basename) != 0 ||
            !audio_extension(new_name) || g_strcmp0(old_basename, new_name) == 0) {
            set_status("Use a new filename ending in .wav, .mp3, .ogg, or .flac.");
        } else if (g_file_test(new_path, G_FILE_TEST_EXISTS) ||
                   g_rename(app.selected_path, new_path) != 0) {
            set_status("Could not rename: destination exists or file access was denied.");
        } else {
            g_free(app.selected_path);
            app.selected_path = g_strdup(new_path);
            refresh_recordings();
            set_status("Recording renamed.");
        }
        g_free(new_basename);
        g_free(old_basename);
        g_free(directory);
        g_free(new_path);
    }
    g_free(base);
    gtk_widget_destroy(dialog);
}

static void on_delete_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    if (!app.selected_path) return;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_CANCEL, "Delete this recording permanently?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                              app.selected_path);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Delete", GTK_RESPONSE_ACCEPT);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        if (g_unlink(app.selected_path) == 0) {
            g_clear_pointer(&app.selected_path, g_free);
            if (app.wave_points) g_array_set_size(app.wave_points, 0);
            refresh_recordings();
            gtk_widget_queue_draw(app.waveform);
            set_status("Recording deleted.");
        } else {
            set_status("Could not delete the selected recording.");
        }
    }
    gtk_widget_destroy(dialog);
    update_buttons();
}

static void on_browse_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Choose recording folder",
        GTK_WINDOW(app.window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(GTK_ENTRY(app.directory_entry), path);
        g_free(path);
        save_config();
        refresh_recordings();
        set_status("Recording folder changed.");
    }
    gtk_widget_destroy(dialog);
}

static void on_directory_changed(GtkEditable *editable, gpointer data) {
    (void)editable; (void)data;
    if (!app.ui_ready) return;
    save_config();
    refresh_recordings();
}

static void on_format_changed(GtkComboBox *combo, gpointer data) {
    (void)combo; (void)data;
    update_depth_controls();
    save_config();
    update_buttons();
}

static void on_options_changed(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    if (app.ui_ready) save_config();
}

static void on_gain_changed(GtkRange *range, gpointer data) {
    (void)data;
    app.gain = gtk_range_get_value(range) / 100.0;
    if (app.ui_ready) save_config();
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)widget; (void)data;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app.window));
    gboolean editing = focus && (GTK_IS_EDITABLE(focus) ||
        GTK_IS_TEXT_VIEW(focus) || GTK_IS_COMBO_BOX(focus));
    if (event->keyval == GDK_KEY_Escape && app.recording) {
        stop_recording();
        return TRUE;
    }
    if (!editing && (event->state & GDK_CONTROL_MASK)) {
        switch (event->keyval) {
        case GDK_KEY_r: case GDK_KEY_R:
            if (!app.recording) start_recording();
            return TRUE;
        case GDK_KEY_s: case GDK_KEY_S:
            stop_recording();
            return TRUE;
        case GDK_KEY_p: case GDK_KEY_P:
            play_selected();
            return TRUE;
        case GDK_KEY_o: case GDK_KEY_O:
            on_browse_clicked(NULL, NULL);
            return TRUE;
        default: break;
        }
    }
    if (!editing && event->keyval == GDK_KEY_space) {
        if (app.recording) toggle_pause();
        else play_selected();
        return TRUE;
    }
    if (!editing && event->keyval == GDK_KEY_Delete && app.selected_path) {
        on_delete_clicked(NULL, NULL);
        return TRUE;
    }
    if (!editing && event->keyval == GDK_KEY_F2 && app.selected_path) {
        on_rename_clicked(NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

static void add_rate_option(const char *id, const char *label) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.rate_combo), id, label);
}

static void add_depth_option(const char *id, const char *label) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.depth_combo), id, label);
}

static void build_ui(GtkApplication *application) {
    app.window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app.window), "GnuChan Sound Record");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 900, 680);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(root), 14);
    gtk_container_add(GTK_CONTAINER(app.window), root);

    GtkWidget *settings = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings), 7);
    gtk_grid_set_column_spacing(GTK_GRID(settings), 8);
    gtk_box_pack_start(GTK_BOX(root), settings, FALSE, FALSE, 0);
    GtkWidget *source_label = gtk_label_new("Microphone");
    gtk_label_set_xalign(GTK_LABEL(source_label), 0.0f);
    app.source_combo = gtk_combo_box_text_new();
    GtkWidget *refresh = gtk_button_new_with_label("Refresh");
    gtk_grid_attach(GTK_GRID(settings), source_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(settings), app.source_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(settings), refresh, 2, 0, 1, 1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_sources_clicked), NULL);

    app.format_combo = gtk_combo_box_text_new();
    app.rate_combo = gtk_combo_box_text_new();
    app.depth_combo = gtk_combo_box_text_new();
    app.quality_combo = gtk_combo_box_text_new();
    add_rate_option("44100", "44.1 kHz");
    add_rate_option("48000", "48 kHz");
    add_rate_option("96000", "96 kHz");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.rate_combo), "48000");
    add_depth_option("16", "16-bit");
    add_depth_option("24", "24-bit");
    add_depth_option("32", "32-bit");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(app.depth_combo), "16");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.quality_combo), "Standard");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.quality_combo), "High");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.quality_combo), "Best");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.quality_combo), 1);
    populate_formats();
    GtkWidget *format_label = gtk_label_new("Format");
    GtkWidget *rate_label = gtk_label_new("Sample rate");
    app.depth_label = gtk_label_new("PCM bit depth");
    app.quality_label = gtk_label_new("Encoding quality");
    GtkWidget *labels[] = {format_label, rate_label, app.depth_label, app.quality_label};
    GtkWidget *controls[] = {app.format_combo, app.rate_combo, app.depth_combo, app.quality_combo};
    for (guint row = 0; row < G_N_ELEMENTS(labels); ++row) {
        gtk_label_set_xalign(GTK_LABEL(labels[row]), 0.0f);
        gtk_grid_attach(GTK_GRID(settings), labels[row], 0, (gint)row + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(settings), controls[row], 1, (gint)row + 1, 2, 1);
    }
    g_signal_connect(app.format_combo, "changed", G_CALLBACK(on_format_changed), NULL);
    g_signal_connect(app.rate_combo, "changed", G_CALLBACK(on_options_changed), NULL);
    g_signal_connect(app.depth_combo, "changed", G_CALLBACK(on_options_changed), NULL);
    g_signal_connect(app.quality_combo, "changed", G_CALLBACK(on_options_changed), NULL);
    GtkWidget *gain_label = gtk_label_new("Microphone gain");
    gtk_label_set_xalign(GTK_LABEL(gain_label), 0.0f);
    app.gain_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 200, 5);
    gtk_scale_set_value_pos(GTK_SCALE(app.gain_scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(app.gain_scale), 0);
    gtk_range_set_value(GTK_RANGE(app.gain_scale), 100);
    gtk_grid_attach(GTK_GRID(settings), gain_label, 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(settings), app.gain_scale, 1, 5, 2, 1);
    g_signal_connect(app.gain_scale, "value-changed", G_CALLBACK(on_gain_changed), NULL);

    GtkWidget *folder_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.directory_entry = gtk_entry_new();
    app.browse_button = gtk_button_new_with_label("Browse…");
    gtk_box_pack_start(GTK_BOX(folder_row), gtk_label_new("Save to"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(folder_row), app.directory_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(folder_row), app.browse_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), folder_row, FALSE, FALSE, 0);
    g_signal_connect(app.browse_button, "clicked", G_CALLBACK(on_browse_clicked), NULL);
    g_signal_connect(app.directory_entry, "changed", G_CALLBACK(on_directory_changed), NULL);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.record_button = gtk_button_new_with_label("Record");
    app.pause_button = gtk_button_new_with_label("Pause");
    app.stop_button = gtk_button_new_with_label("Stop");
    app.duration_label = gtk_label_new("00:00");
    gtk_widget_set_size_request(app.duration_label, 76, -1);
    gtk_box_pack_start(GTK_BOX(actions), app.record_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), app.pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), app.stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), app.duration_label, FALSE, FALSE, 5);
    app.meter = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.meter), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.meter), "Input level");
    gtk_box_pack_start(GTK_BOX(actions), app.meter, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    g_signal_connect(app.record_button, "clicked", G_CALLBACK(on_record_clicked), NULL);
    g_signal_connect(app.pause_button, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    g_signal_connect(app.stop_button, "clicked", G_CALLBACK(on_stop_clicked), NULL);

    GtkWidget *wave_frame = gtk_frame_new("Waveform");
    app.waveform = gtk_drawing_area_new();
    gtk_widget_set_size_request(app.waveform, -1, 120);
    gtk_container_add(GTK_CONTAINER(wave_frame), app.waveform);
    gtk_box_pack_start(GTK_BOX(root), wave_frame, FALSE, FALSE, 0);
    g_signal_connect(app.waveform, "draw", G_CALLBACK(draw_waveform), NULL);

    GtkWidget *files_frame = gtk_frame_new("Recordings");
    GtkWidget *file_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);
    app.recordings_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.recordings_list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scroll), app.recordings_list);
    gtk_box_pack_start(GTK_BOX(file_box), scroll, TRUE, TRUE, 0);
    GtkWidget *file_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.play_button = gtk_button_new_with_label("Play");
    app.rename_button = gtk_button_new_with_label("Rename");
    app.delete_button = gtk_button_new_with_label("Delete");
    gtk_box_pack_start(GTK_BOX(file_actions), app.play_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_actions), app.rename_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_actions), app.delete_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_box), file_actions, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(files_frame), file_box);
    gtk_box_pack_start(GTK_BOX(root), files_frame, TRUE, TRUE, 0);
    g_signal_connect(app.recordings_list, "row-selected", G_CALLBACK(on_row_selected), NULL);
    g_signal_connect(app.play_button, "clicked", G_CALLBACK(on_play_clicked), NULL);
    g_signal_connect(app.rename_button, "clicked", G_CALLBACK(on_rename_clicked), NULL);
    g_signal_connect(app.delete_button, "clicked", G_CALLBACK(on_delete_clicked), NULL);

    app.status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app.status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app.status_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(root), app.status_label, FALSE, FALSE, 0);
    app.sources = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    gtk_widget_show_all(app.window);
    app.ui_ready = TRUE;
    load_config();
    update_depth_controls();
    refresh_sources();
    refresh_recordings();
    if (!app.have_parec || !app.have_pactl) {
        set_status("Recording needs pactl and parec; install pulseaudio-utils. Formats are checked separately.");
    } else if (!app.have_ffmpeg) {
        set_status("WAV PCM is available. Compressed formats are hidden until an FFmpeg encoder is installed.");
    }
    update_buttons();
}

static void application_activate(GtkApplication *application, gpointer data) {
    (void)data;
    if (!app.window) build_ui(application);
    gtk_window_present(GTK_WINDOW(app.window));
}

static int self_test(void) {
    if (!contains_encoder(" A..... libmp3lame MP3 encoder\n A..... flac FLAC encoder",
                          "libmp3lame")) return 1;
    if (contains_encoder(" A..... libmp3lame MP3 encoder", "libmp3")) return 2;
    char *pcm = NULL;
    size_t pcm_length = 0;
    FILE *pcm_file = open_memstream(&pcm, &pcm_length);
    if (!pcm_file) return 3;
    gboolean samples_written = write_sample(pcm_file, 0x1234, 16) &&
        write_sample(pcm_file, 0x123456, 24) &&
        write_sample(pcm_file, 0x12345678, 32);
    if (fclose(pcm_file) != 0 || !samples_written || pcm_length != 9) {
        free(pcm);
        return 4;
    }
    const guint8 expected_pcm[] = {0x34, 0x12, 0x56, 0x34, 0x12,
                                   0x78, 0x56, 0x34, 0x12};
    if (memcmp(pcm, expected_pcm, sizeof expected_pcm) != 0) {
        free(pcm);
        return 5;
    }
    free(pcm);
    char *bytes = NULL;
    size_t length = 0;
    FILE *file = open_memstream(&bytes, &length);
    if (!file) return 6;
    write_wave_header(file, 48000, 24, 0);
    if (fclose(file) != 0 || length != 44) {
        free(bytes);
        return 7;
    }
    const guint8 *header = (const guint8 *)bytes;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0 ||
        header[34] != 24 || header[35] != 0 ||
        header[24] != 0x80 || header[25] != 0xbb) {
        free(bytes);
        return 8;
    }
    free(bytes);
    puts("self-test: encoder detection, PCM bit-depth packing, and WAV header passed");
    return 0;
}

static void print_dependencies(void) {
    printf("Build GUI: GTK 3 (%s)\n", gtk_get_major_version() ? "available" : "unavailable");
    printf("Recording: pactl %s, parec %s (both provided by pulseaudio-utils)\n",
           app.have_pactl ? "available" : "MISSING", app.have_parec ? "available" : "MISSING");
    printf("Playback: paplay %s, ffplay %s\n",
           app.have_paplay ? "available" : "optional/missing",
           app.have_ffplay ? "available" : "optional/missing");
    printf("WAV PCM: available (built in)\n");
    printf("MP3: %s\n", app.have_lame && app.have_mp3_muxer
        ? "available (FFmpeg libmp3lame encoder and MP3 muxer)"
        : "unavailable (required FFmpeg encoder/muxer missing)");
    printf("OGG: %s\n", app.have_vorbis && app.have_ogg_muxer
        ? "available (FFmpeg libvorbis encoder and OGG muxer)"
        : "unavailable (required FFmpeg encoder/muxer missing)");
    printf("FLAC: %s\n", app.have_flac && app.have_flac_muxer
        ? "available (FFmpeg FLAC encoder and muxer)"
        : "unavailable (required FFmpeg encoder/muxer missing)");
}

int main(int argc, char **argv) {
    if (argc > 1 && g_str_equal(argv[1], "--version")) {
        puts(GNUCHAN_SOUND_RECORD_VERSION);
        return 0;
    }
    if (argc > 1 && g_str_equal(argv[1], "--self-test"))
        return self_test();
    detect_dependencies();
    if (argc > 1 && g_str_equal(argv[1], "--check-dependencies")) {
        print_dependencies();
        return 0;
    }
    char *config_dir = g_build_filename(g_get_user_config_dir(),
                                        "GnuChanSoundRecord", NULL);
    app.config_path = g_build_filename(config_dir, "config.conf", NULL);
    g_free(config_dir);
    GtkApplication *application = gtk_application_new(
        "org.gnuchanos.GnuChanSoundRecord", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(application_activate), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    if (app.recording) stop_recording();
    if (app.player) stop_playback();
    if (app.capture_file) {
        finalize_capture_file();
    }
    g_clear_object(&app.capture_stream);
    g_clear_object(&app.capture);
    g_clear_object(&app.player);
    if (app.timer_id) g_source_remove(app.timer_id);
    if (app.wave_points) g_array_unref(app.wave_points);
    if (app.sources) g_object_unref(app.sources);
    g_free(app.working_path);
    g_free(app.final_path);
    g_free(app.selected_path);
    g_free(app.playback_temp);
    g_free(app.config_path);
    g_free(app.encoder_output);
    g_object_unref(application);
    return status;
}
