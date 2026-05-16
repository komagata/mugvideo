#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  GtkWidget *window;
  GtkWidget *preview;
  GtkWidget *elapsed;
  GtkWidget *progress;
  GtkWidget *camera;
  GtkWidget *microphone;
  GtkWidget *record;
  GtkWidget *settings_button;
  GtkWidget *root;
  GtkWidget *settings_root;
  GtkStringList *camera_model;
  GtkStringList *microphone_model;
  GstElement *preview_pipeline;
  GstElement *record_pipeline;
  GstElement *appsink;
  guint preview_timer;
  guint tick_timer;
  gboolean recording;
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

static char *audio_device_name(const char *selected) {
  char *name = NULL;
  GstDeviceMonitor *monitor = gst_device_monitor_new();
  gst_device_monitor_add_filter(monitor, "Audio/Source", NULL);
  gst_device_monitor_start(monitor);

  GList *devices = gst_device_monitor_get_devices(monitor);
  for (GList *node = devices; node != NULL; node = node->next) {
    GstDevice *device = GST_DEVICE(node->data);
    const char *display_name = gst_device_get_display_name(device);
    if (selected[0] != '\0' && display_name != NULL && strcmp(display_name, selected) != 0) continue;

    GstStructure *props = gst_device_get_properties(device);
    if (props != NULL) {
      const char *node_name = gst_structure_get_string(props, "node.name");
      if (node_name != NULL && node_name[0] != '\0') name = g_strdup(node_name);
      gst_structure_free(props);
    }
    if (name != NULL) break;
  }
  g_list_free_full(devices, (GDestroyNotify)gst_object_unref);
  gst_device_monitor_stop(monitor);
  gst_object_unref(monitor);

  return name == NULL ? g_strdup("") : name;
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

  gst_element_send_event(state->record_pipeline, gst_event_new_eos());
  GstBus *bus = gst_element_get_bus(state->record_pipeline);
  GstMessage *message = gst_bus_timed_pop_filtered(
    bus,
    5 * GST_SECOND,
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
  gst_object_unref(bus);

  gst_element_set_state(state->record_pipeline, GST_STATE_NULL);
  gst_object_unref(state->record_pipeline);
  state->record_pipeline = NULL;
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

static gboolean copy_text_to_clipboard(const char *text) {
  GdkDisplay *display = gdk_display_get_default();
  if (display == NULL) return FALSE;
  GdkClipboard *clipboard = gdk_display_get_clipboard(display);
  if (clipboard == NULL) return FALSE;
  gdk_clipboard_set_text(clipboard, text);
  return TRUE;
}

static gboolean command_exists(const char *command) {
  char *path = g_find_program_in_path(command);
  gboolean exists = path != NULL;
  g_free(path);
  return exists;
}

static gboolean copy_file_with_command(const char *path) {
  char *uri = g_filename_to_uri(path, NULL, NULL);
  if (uri == NULL) return FALSE;

  gboolean copied = FALSE;
  if (command_exists("wl-copy")) {
    char *uri_list = g_strdup_printf("%s\n", uri);
    char *gnome_files = g_strdup_printf("copy\n%s\n", uri);
    char *argv_uri[] = {"wl-copy", "--type", "text/uri-list", uri_list, NULL};
    char *argv_gnome[] = {"wl-copy", "--type", "x-special/gnome-copied-files", gnome_files, NULL};
    copied = g_spawn_sync(NULL, argv_uri, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL) &&
      g_spawn_sync(NULL, argv_gnome, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(uri_list);
    g_free(gnome_files);
  } else if (command_exists("xclip")) {
    char *uri_list = g_strdup_printf("%s\n", uri);
    char *argv_uri[] = {"xclip", "-selection", "clipboard", "-t", "text/uri-list", NULL};
    copied = g_spawn_sync(NULL, argv_uri, NULL, G_SPAWN_SEARCH_PATH, NULL, uri_list, NULL, NULL, NULL, NULL);
    g_free(uri_list);
  }

  g_free(uri);
  return copied;
}

static gboolean copy_file_to_clipboard(const char *path) {
  if (copy_file_with_command(path)) return TRUE;
  return copy_text_to_clipboard(path);
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
  char *microphone = selected_string(GTK_DROP_DOWN(state->microphone), state->microphone_model);
  char *device = camera_device_path(camera);
  char *audio_device = audio_device_name(microphone);
  char *audio_source = audio_device[0] == '\0'
    ? g_strdup("pulsesrc do-timestamp=true")
    : g_strdup_printf("pulsesrc device=\"%s\" do-timestamp=true", audio_device);
  char *pipeline = g_strdup_printf(
    "mp4mux name=mux faststart=true ! filesink location=\"%s\" "
    "v4l2src device=%s do-timestamp=true ! "
    "image/jpeg,width=640,height=480,framerate=30/1 ! jpegdec ! "
    "videoconvert ! video/x-raw,format=I420 ! "
    "openh264enc bitrate=2000000 ! h264parse ! queue ! mux. "
    "%s ! audioconvert ! audioresample ! "
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
  g_free(microphone);
  g_free(camera);

  if (state->record_pipeline == NULL) {
    g_warning("recording pipeline failed: %s", error == NULL ? "unknown error" : error->message);
    if (error != NULL) g_error_free(error);
    return FALSE;
  }

  GstStateChangeReturn result = gst_element_set_state(state->record_pipeline, GST_STATE_PLAYING);
  if (result == GST_STATE_CHANGE_FAILURE) {
    g_warning("failed to start recording pipeline");
    gst_element_set_state(state->record_pipeline, GST_STATE_NULL);
    gst_object_unref(state->record_pipeline);
    state->record_pipeline = NULL;
    return FALSE;
  }
  return TRUE;
}

static gboolean tick(gpointer data) {
  AppState *state = data;
  if (state->recording) {
    char text[16];
    int elapsed = (int)(time(NULL) - state->started_at);
    format_seconds(elapsed, text, sizeof(text));
    gtk_label_set_text(GTK_LABEL(state->elapsed), text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), MIN(elapsed / 60.0, 1.0));
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), text);
  }
  return G_SOURCE_CONTINUE;
}

static void on_record_clicked(GtkButton *button, gpointer data) {
  AppState *state = data;

  if (state->recording) {
    state->recording = FALSE;
    stop_recording_pipeline(state);
    start_preview(state);
    copy_file_to_clipboard(state->output_path);
    gtk_button_set_icon_name(button, "media-record-symbolic");
    gtk_widget_set_sensitive(state->camera, TRUE);
    gtk_widget_set_sensitive(state->microphone, TRUE);
    gtk_widget_set_sensitive(state->settings_button, TRUE);
    gtk_label_set_text(GTK_LABEL(state->elapsed), "saved");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), "ready");
    return;
  }

  mkdir_p(state->output_dir);
  make_output_path(state);
  stop_preview(state);
  if (!start_recording_pipeline(state)) {
    start_preview(state);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), "recording failed");
    return;
  }
  state->recording = TRUE;
  state->started_at = time(NULL);
  gtk_button_set_icon_name(button, "media-playback-stop-symbolic");
  gtk_widget_set_sensitive(state->camera, FALSE);
  gtk_widget_set_sensitive(state->microphone, FALSE);
  gtk_widget_set_sensitive(state->settings_button, FALSE);
  gtk_label_set_text(GTK_LABEL(state->elapsed), "00:00");
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), 0.0);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), "recording");
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
  char *microphone = selected_string(GTK_DROP_DOWN(state->microphone), state->microphone_model);
  snprintf(state->selected_microphone, sizeof(state->selected_microphone), "%s", microphone);
  g_free(microphone);
  save_settings_file(state);
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
    ".controls { margin-top: 8px; }");
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
  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *camera_icon = gtk_image_new_from_icon_name("camera-photo-symbolic");
  GtkWidget *microphone_icon = gtk_image_new_from_icon_name("audio-input-microphone-symbolic");
  state->settings_button = gtk_button_new_from_icon_name("emblem-system-symbolic");

  state->preview = gtk_picture_new();
  state->elapsed = gtk_label_new("00:00");
  state->progress = gtk_progress_bar_new();
  state->record = gtk_button_new_from_icon_name("media-record-symbolic");
  state->camera_model = device_names("Video/Source", "Default camera");
  state->microphone_model = device_names("Audio/Source", "Default microphone");
  state->camera = gtk_drop_down_new(G_LIST_MODEL(state->camera_model), NULL);
  state->microphone = gtk_drop_down_new(G_LIST_MODEL(state->microphone_model), NULL);
  select_string(GTK_DROP_DOWN(state->camera), state->camera_model, state->selected_camera);
  select_string(GTK_DROP_DOWN(state->microphone), state->microphone_model, state->selected_microphone);

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
  gtk_widget_set_hexpand(device_row, TRUE);
  gtk_widget_set_hexpand(state->progress, TRUE);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), "ready");
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progress), TRUE);

  g_signal_connect(state->camera, "notify::selected", G_CALLBACK(on_camera_changed), state);
  g_signal_connect(state->microphone, "notify::selected", G_CALLBACK(on_microphone_changed), state);
  g_signal_connect(state->record, "clicked", G_CALLBACK(on_record_clicked), state);
  g_signal_connect(state->settings_button, "clicked", G_CALLBACK(show_settings), state);

  gtk_frame_set_child(GTK_FRAME(preview_frame), state->preview);
  gtk_box_append(GTK_BOX(device_row), camera_icon);
  gtk_box_append(GTK_BOX(device_row), state->camera);
  gtk_box_append(GTK_BOX(device_row), microphone_icon);
  gtk_box_append(GTK_BOX(device_row), state->microphone);
  gtk_box_append(GTK_BOX(device_row), state->settings_button);
  gtk_box_append(GTK_BOX(status_row), state->elapsed);
  gtk_box_append(GTK_BOX(status_row), state->progress);
  gtk_box_append(GTK_BOX(status_row), state->record);
  gtk_box_append(GTK_BOX(controls), status_row);
  gtk_box_append(GTK_BOX(controls), device_row);
  gtk_box_append(GTK_BOX(state->root), preview_frame);
  gtk_box_append(GTK_BOX(state->root), controls);

  state->settings_root = build_settings(state);
  state->tick_timer = g_timeout_add(500, tick, state);
  start_preview(state);
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
  stop_preview(state);
  if (state->tick_timer != 0) g_source_remove(state->tick_timer);
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
