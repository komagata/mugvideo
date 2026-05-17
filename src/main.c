#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LEVEL_SEGMENTS 12

typedef struct {
  GtkWidget *window;
  GtkWidget *preview;
  GtkWidget *elapsed;
  GtkWidget *level_meter;
  GtkWidget *level_segments[LEVEL_SEGMENTS];
  GtkWidget *camera;
  GtkWidget *microphone;
  GtkWidget *record;
  GtkWidget *settings_button;
  GtkWidget *root;
  GtkWidget *settings_root;
  GtkStringList *camera_model;
  GtkStringList *microphone_model;
  GPtrArray *microphone_ids;
  GstElement *preview_pipeline;
  GstElement *record_pipeline;
  GstElement *level_pipeline;
  GstElement *appsink;
  GstBus *record_bus;
  GstBus *level_bus;
  guint preview_timer;
  guint tick_timer;
  guint record_bus_watch;
  guint level_bus_watch;
  gboolean recording;
  double audio_level;
  time_t started_at;
  char config_dir[1024];
  char settings_path[1200];
  char output_dir[1024];
  char output_format[16];
  char output_path[1200];
  char selected_camera[256];
  char selected_microphone[256];
} AppState;

static void mkdir_p(const char *path) {
  char tmp[1024];

  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

static void home_path(char *out, size_t size, const char *suffix) {
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') home = ".";
  snprintf(out, size, "%s/%s", home, suffix);
}

static void load_defaults(AppState *state) {
  home_path(state->config_dir, sizeof(state->config_dir), ".config/mugvideo");
  mkdir_p(state->config_dir);
  snprintf(state->settings_path, sizeof(state->settings_path), "%s/settings.ini", state->config_dir);
  home_path(state->output_dir, sizeof(state->output_dir), "Videos/mugvideo");
  snprintf(state->output_format, sizeof(state->output_format), "mp4");
  state->selected_camera[0] = '\0';
  state->selected_microphone[0] = '\0';
  mkdir_p(state->output_dir);
}

static void load_settings(AppState *state) {
  GKeyFile *key_file = g_key_file_new();
  GError *error = NULL;

  load_defaults(state);
  if (g_key_file_load_from_file(key_file, state->settings_path, G_KEY_FILE_NONE, &error)) {
    char *output_dir = g_key_file_get_string(key_file, "settings", "output_dir", NULL);
    if (output_dir != NULL && output_dir[0] != '\0') {
      snprintf(state->output_dir, sizeof(state->output_dir), "%s", output_dir);
      mkdir_p(state->output_dir);
    }
    g_free(output_dir);

    char *camera = g_key_file_get_string(key_file, "settings", "camera", NULL);
    if (camera != NULL) snprintf(state->selected_camera, sizeof(state->selected_camera), "%s", camera);
    g_free(camera);

    char *microphone = g_key_file_get_string(key_file, "settings", "microphone", NULL);
    if (microphone != NULL) snprintf(state->selected_microphone, sizeof(state->selected_microphone), "%s", microphone);
    g_free(microphone);
  }
  if (error != NULL) g_error_free(error);
  g_key_file_unref(key_file);
}

static void save_settings_file(AppState *state) {
  GKeyFile *key_file = g_key_file_new();
  g_key_file_set_string(key_file, "settings", "output_dir", state->output_dir);
  g_key_file_set_string(key_file, "settings", "camera", state->selected_camera);
  g_key_file_set_string(key_file, "settings", "microphone", state->selected_microphone);

  gsize length = 0;
  char *data = g_key_file_to_data(key_file, &length, NULL);
  if (data != NULL) {
    GError *error = NULL;
    g_file_set_contents(state->settings_path, data, (gssize)length, &error);
    if (error != NULL) {
      g_warning("failed to save settings: %s", error->message);
      g_error_free(error);
    }
  }

  g_free(data);
  g_key_file_unref(key_file);
}

static void format_seconds(int total, char *out, size_t size) {
  snprintf(out, size, "%02d:%02d", total / 60, total % 60);
}

static char *selected_string(GtkDropDown *dropdown, GtkStringList *model) {
  guint selected = gtk_drop_down_get_selected(dropdown);
  const char *value = gtk_string_list_get_string(model, selected);
  return g_strdup(value == NULL ? "" : value);
}

static void select_string(GtkDropDown *dropdown, GtkStringList *model, const char *value) {
  if (value == NULL || value[0] == '\0') return;

  guint count = g_list_model_get_n_items(G_LIST_MODEL(model));
  for (guint i = 0; i < count; i++) {
    const char *item = gtk_string_list_get_string(model, i);
    if (item != NULL && strcmp(item, value) == 0) {
      gtk_drop_down_set_selected(dropdown, i);
      return;
    }
  }
}

static GtkStringList *device_names(const char *klass, const char *fallback) {
  GtkStringList *names = gtk_string_list_new(NULL);
  GstDeviceMonitor *monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(monitor, klass, NULL);
  gst_device_monitor_start(monitor);

  GList *devices = gst_device_monitor_get_devices(monitor);
  for (GList *node = devices; node != NULL; node = node->next) {
    GstDevice *device = GST_DEVICE(node->data);
    const char *name = gst_device_get_display_name(device);
    if (name != NULL && name[0] != '\0') gtk_string_list_append(names, name);
  }
  g_list_free_full(devices, (GDestroyNotify)gst_object_unref);
  gst_device_monitor_stop(monitor);
  gst_object_unref(monitor);

  if (g_list_model_get_n_items(G_LIST_MODEL(names)) == 0) gtk_string_list_append(names, fallback);
  return names;
}

static GtkStringList *microphone_devices(GPtrArray **ids) {
  GtkStringList *names = gtk_string_list_new(NULL);
  *ids = g_ptr_array_new_with_free_func(g_free);

  GstDeviceMonitor *monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(monitor, "Audio/Source", NULL);
  gst_device_monitor_start(monitor);

  GList *devices = gst_device_monitor_get_devices(monitor);
  for (GList *node = devices; node != NULL; node = node->next) {
    GstDevice *device = GST_DEVICE(node->data);
    const char *display_name = gst_device_get_display_name(device);
    char *id = NULL;

    GstStructure *props = gst_device_get_properties(device);
    if (props != NULL) {
      const char *node_name = gst_structure_get_string(props, "node.name");
      if (node_name != NULL && node_name[0] != '\0') id = g_strdup(node_name);
      gst_structure_free(props);
    }

    if (display_name != NULL && display_name[0] != '\0' && id != NULL) {
      guint number = g_list_model_get_n_items(G_LIST_MODEL(names)) + 1;
      char *label = g_strdup_printf("%s (%u)", display_name, number);
      gtk_string_list_append(names, label);
      g_ptr_array_add(*ids, id);
      g_free(label);
    } else {
      g_free(id);
    }
  }
  g_list_free_full(devices, (GDestroyNotify)gst_object_unref);
  gst_device_monitor_stop(monitor);
  gst_object_unref(monitor);

  if (g_list_model_get_n_items(G_LIST_MODEL(names)) == 0) {
    gtk_string_list_append(names, "Default microphone");
    g_ptr_array_add(*ids, g_strdup(""));
  }
  return names;
}

static char *selected_microphone_id(AppState *state) {
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->microphone));
  if (state->microphone_ids == NULL || selected >= state->microphone_ids->len) return g_strdup("");
  return g_strdup(g_ptr_array_index(state->microphone_ids, selected));
}

static void select_microphone_id(AppState *state, const char *id) {
  if (id == NULL || id[0] == '\0' || state->microphone_ids == NULL) return;
  for (guint i = 0; i < state->microphone_ids->len; i++) {
    const char *item = g_ptr_array_index(state->microphone_ids, i);
    if (item != NULL && strcmp(item, id) == 0) {
      gtk_drop_down_set_selected(GTK_DROP_DOWN(state->microphone), i);
      return;
    }
  }
}

static char *camera_device_path(const char *selected) {
  char *path = NULL;
  GstDeviceMonitor *monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(monitor, "Video/Source", NULL);
  gst_device_monitor_start(monitor);

  GList *devices = gst_device_monitor_get_devices(monitor);
  for (GList *node = devices; node != NULL; node = node->next) {
    GstDevice *device = GST_DEVICE(node->data);
    const char *name = gst_device_get_display_name(device);
    if (selected[0] != '\0' && name != NULL && strcmp(name, selected) != 0) continue;

    GstStructure *props = gst_device_get_properties(device);
    if (props != NULL) {
      const char *v4l2_path = gst_structure_get_string(props, "api.v4l2.path");
      if (v4l2_path != NULL && v4l2_path[0] != '\0') path = g_strdup(v4l2_path);
      gst_structure_free(props);
    }
    if (path != NULL) break;
  }
  g_list_free_full(devices, (GDestroyNotify)gst_object_unref);
  gst_device_monitor_stop(monitor);
  gst_object_unref(monitor);

  return path == NULL ? g_strdup("/dev/video0") : path;
}

static gboolean update_preview(gpointer data) {
  AppState *state = data;
  GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(state->appsink), 0);
  if (sample == NULL) return G_SOURCE_CONTINUE;

  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstStructure *structure = caps == NULL ? NULL : gst_caps_get_structure(caps, 0);
  int width = 0;
  int height = 0;
  if (structure == NULL ||
      !gst_structure_get_int(structure, "width", &width) ||
      !gst_structure_get_int(structure, "height", &height) ||
      buffer == NULL) {
    gst_sample_unref(sample);
    return G_SOURCE_CONTINUE;
  }

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return G_SOURCE_CONTINUE;
  }

  gsize size = (gsize)width * (gsize)height * 3;
  guint8 *pixels = g_malloc(size);
  memcpy(pixels, map.data, MIN(size, map.size));
  GBytes *bytes = g_bytes_new_take(pixels, size);
  GdkTexture *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8, bytes, (gsize)width * 3);
  gtk_picture_set_paintable(GTK_PICTURE(state->preview), GDK_PAINTABLE(texture));

  g_object_unref(texture);
  g_bytes_unref(bytes);
  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);
  return G_SOURCE_CONTINUE;
}

static void stop_preview(AppState *state) {
  if (state->preview_timer != 0) {
    g_source_remove(state->preview_timer);
    state->preview_timer = 0;
  }
  if (state->preview_pipeline != NULL) {
    gst_element_set_state(state->preview_pipeline, GST_STATE_NULL);
    gst_object_unref(state->preview_pipeline);
    state->preview_pipeline = NULL;
  }
  if (state->appsink != NULL) {
    gst_object_unref(state->appsink);
    state->appsink = NULL;
  }
}

static void stop_recording_pipeline(AppState *state) {
  if (state->record_pipeline == NULL) return;

  if (state->record_bus_watch != 0) {
    g_source_remove(state->record_bus_watch);
    state->record_bus_watch = 0;
  }
  gst_element_send_event(state->record_pipeline, gst_event_new_eos());
  GstMessage *message = gst_bus_timed_pop_filtered(
    state->record_bus,
    2 * GST_SECOND,
    GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (message != NULL) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *error = NULL;
      char *debug = NULL;
      gst_message_parse_error(message, &error, &debug);
      g_warning("recording pipeline failed: %s", error == NULL ? "unknown error" : error->message);
      if (error != NULL) g_error_free(error);
      g_free(debug);
    }
    gst_message_unref(message);
  }
  if (state->record_bus != NULL) {
    gst_object_unref(state->record_bus);
    state->record_bus = NULL;
  }

  gst_element_set_state(state->record_pipeline, GST_STATE_NULL);
  gst_object_unref(state->record_pipeline);
  state->record_pipeline = NULL;
  if (state->appsink != NULL) {
    gst_object_unref(state->appsink);
    state->appsink = NULL;
  }
}

static gboolean value_as_double(const GValue *value, double *out) {
  if (G_VALUE_HOLDS_DOUBLE(value)) {
    *out = g_value_get_double(value);
    return TRUE;
  }
  if (G_VALUE_HOLDS_FLOAT(value)) {
    *out = g_value_get_float(value);
    return TRUE;
  }
  return FALSE;
}

static gboolean audio_level_db(const GValue *values, double *out) {
  guint count = 0;
  gboolean is_array = values != NULL && GST_VALUE_HOLDS_ARRAY(values);
  gboolean is_list = values != NULL && GST_VALUE_HOLDS_LIST(values);
  gboolean is_value_array = values != NULL && G_VALUE_HOLDS(values, G_TYPE_VALUE_ARRAY);

  if (is_array) count = gst_value_array_get_size(values);
  if (is_list) count = gst_value_list_get_size(values);
  GValueArray *value_array = is_value_array ? g_value_get_boxed(values) : NULL;
  if (value_array != NULL) count = value_array->n_values;
  if (count == 0) return FALSE;

  double best = -G_MAXDOUBLE;
  for (guint i = 0; i < count; i++) {
    const GValue *item = NULL;
    if (is_array) item = gst_value_array_get_value(values, i);
    if (is_list) item = gst_value_list_get_value(values, i);
    if (value_array != NULL) item = g_value_array_get_nth(value_array, i);
    double db = 0.0;
    if (item != NULL && value_as_double(item, &db)) best = MAX(best, db);
  }

  if (best == -G_MAXDOUBLE) return FALSE;
  *out = best;
  return TRUE;
}

static gboolean on_record_bus_message(GstBus *bus, GstMessage *message, gpointer data) {
  (void)bus;
  AppState *state = data;

  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
    const GstStructure *structure = gst_message_get_structure(message);
    if (structure != NULL && gst_structure_has_name(structure, "level")) {
      const GValue *peak = gst_structure_get_value(structure, "peak");
      double db = 0.0;
      if (audio_level_db(peak, &db)) {
        double normalized = (db + 80.0) / 50.0;
        state->audio_level = CLAMP(normalized, 0.0, 1.0);
      }
    }
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = NULL;
    char *debug = NULL;
    gst_message_parse_error(message, &error, &debug);
    g_warning("recording pipeline failed: %s", error == NULL ? "unknown error" : error->message);
    if (error != NULL) g_error_free(error);
    g_free(debug);
  }

  return G_SOURCE_CONTINUE;
}

static void stop_level_monitor(AppState *state) {
  if (state->level_bus_watch != 0) {
    g_source_remove(state->level_bus_watch);
    state->level_bus_watch = 0;
  }
  if (state->level_pipeline != NULL) {
    gst_element_set_state(state->level_pipeline, GST_STATE_NULL);
    gst_object_unref(state->level_pipeline);
    state->level_pipeline = NULL;
  }
  if (state->level_bus != NULL) {
    gst_object_unref(state->level_bus);
    state->level_bus = NULL;
  }
}

static void start_level_monitor(AppState *state) {
  if (state->recording) return;
  stop_level_monitor(state);

  char *microphone = selected_microphone_id(state);
  char *audio_device = g_shell_quote(microphone);
  char *source = microphone[0] == '\0'
    ? g_strdup("pulsesrc do-timestamp=true")
    : g_strdup_printf("pulsesrc device=%s do-timestamp=true", audio_device);
  char *pipeline = g_strdup_printf(
    "%s ! audioconvert ! audioresample ! "
    "level interval=50000000 post-messages=true ! fakesink sync=false",
    source);
  GError *error = NULL;

  state->level_pipeline = gst_parse_launch(pipeline, &error);
  if (state->level_pipeline == NULL) {
    g_warning("audio level monitor failed: %s", error == NULL ? "unknown error" : error->message);
    if (error != NULL) g_error_free(error);
  } else {
    state->level_bus = gst_element_get_bus(state->level_pipeline);
    state->level_bus_watch = gst_bus_add_watch(state->level_bus, on_record_bus_message, state);
    if (gst_element_set_state(state->level_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      g_warning("failed to start audio level monitor");
      stop_level_monitor(state);
    }
  }

  g_free(pipeline);
  g_free(source);
  g_free(audio_device);
  g_free(microphone);
}

static void start_preview(AppState *state) {
  stop_preview(state);

  char *camera = selected_string(GTK_DROP_DOWN(state->camera), state->camera_model);
  char *device = camera_device_path(camera);
  char *pipeline = g_strdup_printf(
    "v4l2src device=%s ! "
    "image/jpeg,width=640,height=480,framerate=30/1 ! jpegdec ! "
    "videoconvert ! video/x-raw,format=RGB ! "
    "appsink name=sink max-buffers=1 drop=true sync=false",
    device);
  GError *error = NULL;

  state->preview_pipeline = gst_parse_launch(pipeline, &error);
  if (state->preview_pipeline == NULL) {
    g_warning("preview pipeline failed: %s", error == NULL ? "unknown error" : error->message);
    if (error != NULL) g_error_free(error);
  } else {
    state->appsink = gst_bin_get_by_name(GST_BIN(state->preview_pipeline), "sink");
    gst_element_set_state(state->preview_pipeline, GST_STATE_PLAYING);
    state->preview_timer = g_timeout_add(33, update_preview, state);
  }

  g_free(pipeline);
  g_free(device);
  g_free(camera);
}

static gboolean copy_file_to_clipboard(const char *path) {
  GdkDisplay *display = gdk_display_get_default();
  if (display == NULL) return FALSE;

  GFile *file = g_file_new_for_path(path);
  GFile *files[] = {file};
  GdkFileList *file_list = gdk_file_list_new_from_array(files, 1);

  GValue value = G_VALUE_INIT;
  g_value_init(&value, GDK_TYPE_FILE_LIST);
  g_value_take_boxed(&value, file_list);

  GdkContentProvider *provider = gdk_content_provider_new_for_value(&value);
  gdk_clipboard_set_content(gdk_display_get_clipboard(display), provider);

  g_object_unref(provider);
  g_value_unset(&value);
  g_object_unref(file);
  return TRUE;
}

static void make_output_path(AppState *state) {
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);

  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  snprintf(state->output_path, sizeof(state->output_path), "%s/mugvideo-%s.%s", state->output_dir, stamp, state->output_format);
}

static gboolean start_recording_pipeline(AppState *state) {
  char *camera = selected_string(GTK_DROP_DOWN(state->camera), state->camera_model);
  char *device = camera_device_path(camera);
  char *audio_device = selected_microphone_id(state);
  char *audio_source = audio_device[0] == '\0'
    ? g_strdup("pulsesrc do-timestamp=true")
    : g_strdup_printf("pulsesrc device=\"%s\" do-timestamp=true", audio_device);
  char *pipeline = g_strdup_printf(
    "mp4mux name=mux faststart=true ! filesink location=\"%s\" "
    "v4l2src device=%s do-timestamp=true ! "
    "image/jpeg,width=640,height=480,framerate=30/1 ! jpegdec ! "
    "videoconvert ! video/x-raw,format=I420 ! tee name=video "
    "video. ! queue leaky=downstream max-size-buffers=1 ! videoconvert ! video/x-raw,format=RGB ! "
    "appsink name=sink max-buffers=1 drop=true sync=false "
    "video. ! queue ! openh264enc bitrate=2000000 ! h264parse ! queue ! mux. "
    "%s ! audioconvert ! audioresample ! level interval=50000000 post-messages=true ! "
    "avenc_aac ! aacparse ! queue ! mux.",
    state->output_path,
    device,
    audio_source);
  GError *error = NULL;

  state->record_pipeline = gst_parse_launch(pipeline, &error);
  g_free(pipeline);
  g_free(audio_source);
  g_free(audio_device);
  g_free(device);
  g_free(camera);

  if (state->record_pipeline == NULL) {
    g_warning("recording pipeline failed: %s", error == NULL ? "unknown error" : error->message);
    if (error != NULL) g_error_free(error);
    return FALSE;
  }

  state->appsink = gst_bin_get_by_name(GST_BIN(state->record_pipeline), "sink");
  if (state->appsink == NULL) {
    g_warning("recording preview appsink not found");
    gst_object_unref(state->record_pipeline);
    state->record_pipeline = NULL;
    return FALSE;
  }
  state->record_bus = gst_element_get_bus(state->record_pipeline);
  state->record_bus_watch = gst_bus_add_watch(state->record_bus, on_record_bus_message, state);

  GstStateChangeReturn result = gst_element_set_state(state->record_pipeline, GST_STATE_PLAYING);
  if (result == GST_STATE_CHANGE_FAILURE) {
    g_warning("failed to start recording pipeline");
    gst_element_set_state(state->record_pipeline, GST_STATE_NULL);
    gst_object_unref(state->record_pipeline);
    state->record_pipeline = NULL;
    gst_object_unref(state->appsink);
    state->appsink = NULL;
    if (state->record_bus_watch != 0) {
      g_source_remove(state->record_bus_watch);
      state->record_bus_watch = 0;
    }
    if (state->record_bus != NULL) {
      gst_object_unref(state->record_bus);
      state->record_bus = NULL;
    }
    return FALSE;
  }
  state->preview_timer = g_timeout_add(33, update_preview, state);
  return TRUE;
}

static gboolean tick(gpointer data) {
  AppState *state = data;
  guint active = (guint)(CLAMP(state->audio_level, 0.0, 1.0) * LEVEL_SEGMENTS + 0.999);
  for (guint i = 0; i < LEVEL_SEGMENTS; i++) {
    if (i < active) {
      gtk_widget_add_css_class(state->level_segments[i], "active");
    } else {
      gtk_widget_remove_css_class(state->level_segments[i], "active");
    }
  }

  if (state->recording) {
    char text[16];
    int elapsed = (int)(time(NULL) - state->started_at);
    format_seconds(elapsed, text, sizeof(text));
    gtk_label_set_text(GTK_LABEL(state->elapsed), text);
  }
  return G_SOURCE_CONTINUE;
}

static void on_record_clicked(GtkButton *button, gpointer data) {
  AppState *state = data;

  if (state->recording) {
    state->recording = FALSE;
    if (state->preview_timer != 0) {
      g_source_remove(state->preview_timer);
      state->preview_timer = 0;
    }
    stop_recording_pipeline(state);
    start_preview(state);
    start_level_monitor(state);
    copy_file_to_clipboard(state->output_path);
    gtk_button_set_icon_name(button, "media-record-symbolic");
    gtk_widget_set_sensitive(state->camera, TRUE);
    gtk_widget_set_sensitive(state->microphone, TRUE);
    gtk_widget_set_sensitive(state->settings_button, TRUE);
    return;
  }

  mkdir_p(state->output_dir);
  make_output_path(state);
  stop_level_monitor(state);
  stop_preview(state);
  if (!start_recording_pipeline(state)) {
    start_preview(state);
    start_level_monitor(state);
    return;
  }
  state->recording = TRUE;
  state->audio_level = 0.0;
  state->started_at = time(NULL);
  gtk_button_set_icon_name(button, "media-playback-stop-symbolic");
  gtk_widget_set_sensitive(state->camera, FALSE);
  gtk_widget_set_sensitive(state->microphone, FALSE);
  gtk_widget_set_sensitive(state->settings_button, FALSE);
  gtk_label_set_text(GTK_LABEL(state->elapsed), "00:00");
}

static void show_main(GtkButton *button, gpointer data) {
  (void)button;
  AppState *state = data;
  gtk_window_set_child(GTK_WINDOW(state->window), state->root);
}

static void show_settings(GtkButton *button, gpointer data) {
  (void)button;
  AppState *state = data;
  gtk_window_set_child(GTK_WINDOW(state->window), state->settings_root);
}

static void save_settings(GtkButton *button, gpointer data) {
  AppState *state = data;
  GtkWidget *entry = g_object_get_data(G_OBJECT(button), "output-dir");
  snprintf(state->output_dir, sizeof(state->output_dir), "%s", gtk_editable_get_text(GTK_EDITABLE(entry)));
  mkdir_p(state->output_dir);
  save_settings_file(state);
  gtk_button_set_label(button, "Saved");
}

static GtkWidget *build_settings(AppState *state) {
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *back = gtk_button_new_with_label("Back");
  GtkWidget *output_dir = gtk_entry_new();
  GtkWidget *save = gtk_button_new_with_label("Save");

  gtk_widget_add_css_class(root, "app-shell");
  gtk_widget_add_css_class(panel, "settings-panel");
  gtk_widget_set_margin_top(root, 14);
  gtk_widget_set_margin_bottom(root, 14);
  gtk_widget_set_margin_start(root, 14);
  gtk_widget_set_margin_end(root, 14);
  gtk_editable_set_text(GTK_EDITABLE(output_dir), state->output_dir);
  g_object_set_data(G_OBJECT(save), "output-dir", output_dir);

  g_signal_connect(back, "clicked", G_CALLBACK(show_main), state);
  g_signal_connect(save, "clicked", G_CALLBACK(save_settings), state);

  gtk_box_append(GTK_BOX(header), back);
  gtk_box_append(GTK_BOX(header), gtk_label_new("Settings"));
  gtk_box_append(GTK_BOX(panel), gtk_label_new("Output directory"));
  gtk_box_append(GTK_BOX(panel), output_dir);
  gtk_box_append(GTK_BOX(panel), save);
  gtk_box_append(GTK_BOX(root), header);
  gtk_box_append(GTK_BOX(root), panel);
  return root;
}

static void on_camera_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object;
  (void)pspec;
  AppState *state = data;
  if (state->recording) return;
  char *camera = selected_string(GTK_DROP_DOWN(state->camera), state->camera_model);
  snprintf(state->selected_camera, sizeof(state->selected_camera), "%s", camera);
  g_free(camera);
  save_settings_file(state);
  start_preview(state);
}

static void on_microphone_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object;
  (void)pspec;
  AppState *state = data;
  if (state->recording) return;
  char *microphone = selected_microphone_id(state);
  snprintf(state->selected_microphone, sizeof(state->selected_microphone), "%s", microphone);
  g_free(microphone);
  save_settings_file(state);
  start_level_monitor(state);
}

static GtkWidget *build_level_meter(AppState *state) {
  GtkWidget *meter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_add_css_class(meter, "level-meter");

  for (guint i = 0; i < LEVEL_SEGMENTS; i++) {
    GtkWidget *segment = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(segment, "level-segment");
    gtk_widget_set_size_request(segment, 8, 9);
    gtk_box_append(GTK_BOX(meter), segment);
    state->level_segments[i] = segment;
  }

  return meter;
}

static gboolean enable_window_resize(gpointer data) {
  gtk_window_set_resizable(GTK_WINDOW(data), TRUE);
  return G_SOURCE_REMOVE;
}

static void load_css(void) {
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider,
    ".preview-frame { background: #000000; }"
    ".app-shell { padding: 12px; }"
    ".controls { margin-top: 8px; }"
    ".level-meter { margin-left: 4px; margin-right: 4px; }"
    ".level-segment { background: #4a4a4a; border-radius: 2px; min-width: 8px; min-height: 9px; }"
    ".level-segment.active { background: #57e389; }");
  gtk_style_context_add_provider_for_display(
    gdk_display_get_default(),
    GTK_STYLE_PROVIDER(provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void activate(GtkApplication *app, gpointer data) {
  AppState *state = data;
  load_settings(state);
  load_css();

  state->window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(state->window), "mugvideo");
  gtk_window_set_default_size(GTK_WINDOW(state->window), 640, 470);
  gtk_window_set_resizable(GTK_WINDOW(state->window), FALSE);

  state->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *preview_frame = gtk_frame_new(NULL);
  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *left_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *right_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *camera_icon = gtk_image_new_from_icon_name("camera-photo-symbolic");
  GtkWidget *microphone_icon = gtk_image_new_from_icon_name("audio-input-microphone-symbolic");
  state->settings_button = gtk_button_new_from_icon_name("emblem-system-symbolic");

  state->preview = gtk_picture_new();
  state->elapsed = gtk_label_new("00:00");
  state->level_meter = build_level_meter(state);
  state->record = gtk_button_new_from_icon_name("media-record-symbolic");
  state->camera_model = device_names("Video/Source", "Default camera");
  state->microphone_model = microphone_devices(&state->microphone_ids);
  state->camera = gtk_drop_down_new(G_LIST_MODEL(state->camera_model), NULL);
  state->microphone = gtk_drop_down_new(G_LIST_MODEL(state->microphone_model), NULL);
  select_string(GTK_DROP_DOWN(state->camera), state->camera_model, state->selected_camera);
  select_microphone_id(state, state->selected_microphone);

  gtk_widget_add_css_class(state->root, "app-shell");
  gtk_widget_add_css_class(preview_frame, "preview-frame");
  gtk_widget_add_css_class(controls, "controls");
  gtk_widget_set_hexpand(state->root, FALSE);
  gtk_widget_set_vexpand(state->root, FALSE);
  gtk_widget_set_hexpand(preview_frame, FALSE);
  gtk_widget_set_vexpand(preview_frame, FALSE);
  gtk_widget_set_halign(preview_frame, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(state->preview, FALSE);
  gtk_widget_set_vexpand(state->preview, FALSE);
  gtk_widget_set_size_request(state->preview, 560, 315);
  gtk_picture_set_content_fit(GTK_PICTURE(state->preview), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_margin_top(state->root, 12);
  gtk_widget_set_margin_bottom(state->root, 12);
  gtk_widget_set_margin_start(state->root, 12);
  gtk_widget_set_margin_end(state->root, 12);
  gtk_widget_set_hexpand(status_row, TRUE);
  gtk_widget_set_hexpand(device_row, TRUE);
  gtk_widget_set_hexpand(left_spacer, TRUE);
  gtk_widget_set_hexpand(right_spacer, TRUE);
  gtk_widget_set_hexpand(state->camera, TRUE);
  gtk_widget_set_hexpand(state->microphone, TRUE);
  gtk_widget_set_hexpand(state->level_meter, FALSE);
  gtk_widget_set_halign(state->level_meter, GTK_ALIGN_CENTER);

  g_signal_connect(state->camera, "notify::selected", G_CALLBACK(on_camera_changed), state);
  g_signal_connect(state->microphone, "notify::selected", G_CALLBACK(on_microphone_changed), state);
  g_signal_connect(state->record, "clicked", G_CALLBACK(on_record_clicked), state);
  g_signal_connect(state->settings_button, "clicked", G_CALLBACK(show_settings), state);

  gtk_frame_set_child(GTK_FRAME(preview_frame), state->preview);
  gtk_box_append(GTK_BOX(status_row), left_spacer);
  gtk_box_append(GTK_BOX(status_row), state->elapsed);
  gtk_box_append(GTK_BOX(status_row), right_spacer);
  gtk_box_append(GTK_BOX(status_row), state->record);
  gtk_box_append(GTK_BOX(status_row), state->settings_button);
  gtk_box_append(GTK_BOX(device_row), camera_icon);
  gtk_box_append(GTK_BOX(device_row), state->camera);
  gtk_box_append(GTK_BOX(device_row), microphone_icon);
  gtk_box_append(GTK_BOX(device_row), state->microphone);
  gtk_box_append(GTK_BOX(device_row), state->level_meter);
  gtk_box_append(GTK_BOX(controls), status_row);
  gtk_box_append(GTK_BOX(controls), device_row);
  gtk_box_append(GTK_BOX(state->root), preview_frame);
  gtk_box_append(GTK_BOX(state->root), controls);

  state->settings_root = build_settings(state);
  state->tick_timer = g_timeout_add(500, tick, state);
  start_preview(state);
  start_level_monitor(state);
  gtk_window_set_child(GTK_WINDOW(state->window), state->root);
  gtk_window_present(GTK_WINDOW(state->window));
  g_timeout_add(500, enable_window_resize, state->window);
}

static void shutdown_app(GApplication *app, gpointer data) {
  (void)app;
  AppState *state = data;
  if (state->recording) {
    state->recording = FALSE;
    stop_recording_pipeline(state);
  }
  stop_level_monitor(state);
  stop_preview(state);
  if (state->tick_timer != 0) g_source_remove(state->tick_timer);
  if (state->microphone_ids != NULL) {
    g_ptr_array_unref(state->microphone_ids);
    state->microphone_ids = NULL;
  }
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);

  AppState state;
  memset(&state, 0, sizeof(state));

  GtkApplication *app = gtk_application_new("org.gtk.mugvideo", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
  g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_app), &state);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
