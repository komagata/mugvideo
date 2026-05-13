#include "magvideo_media.h"

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_RECORDERS 128

typedef struct {
  int active;
  int recording;
  time_t started_at;
  char output_path[1024];
} Recorder;

static Recorder recorders[MAX_RECORDERS];
static int gst_ready = 0;
static char last_error[512] = "";

static const char *str_arg(TyaValue value) {
  if (value.kind == TYA_STRING && value.string != NULL) return value.string;
  return "";
}

static void set_error(const char *message) {
  snprintf(last_error, sizeof(last_error), "%s", message == NULL ? "" : message);
}

static void ensure_gst(void) {
  if (gst_ready) return;
  gst_init(NULL, NULL);
  gst_ready = 1;
}

static TyaValue string_array(const char *fallback) {
  TyaValue arr = tya_array(NULL, 0);
  tya_array_push(arr, tya_string(strdup(fallback)));
  return arr;
}

static int next_recorder(void) {
  for (int i = 1; i < MAX_RECORDERS; i++) {
    if (!recorders[i].active) {
      memset(&recorders[i], 0, sizeof(Recorder));
      recorders[i].active = 1;
      return i;
    }
  }
  set_error("no recorder slots available");
  return 0;
}

static Recorder *recorder_for(TyaValue handle) {
  if (handle.kind != TYA_NUMBER) return NULL;
  int id = (int)handle.number;
  if (id <= 0 || id >= MAX_RECORDERS || !recorders[id].active) return NULL;
  return &recorders[id];
}

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

static char *home_path(const char *suffix) {
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') home = ".";
  int len = snprintf(NULL, 0, "%s/%s", home, suffix);
  char *out = (char *)malloc((size_t)len + 1);
  snprintf(out, (size_t)len + 1, "%s/%s", home, suffix);
  return out;
}

static void write_placeholder_video(const char *path, const char *format) {
  if (strcmp(format, "webm") != 0) {
    int len = snprintf(NULL, 0,
      "gst-launch-1.0 -q videotestsrc num-buffers=30 pattern=ball ! "
      "video/x-raw,width=720,height=1280,framerate=30/1 ! videoconvert ! "
      "openh264enc ! h264parse ! queue ! mux. "
      "audiotestsrc num-buffers=60 wave=silence ! audioconvert ! avenc_aac ! "
      "aacparse ! queue ! mux. mp4mux name=mux ! filesink location=\"%s\"",
      path);
    char *cmd = (char *)malloc((size_t)len + 1);
    snprintf(cmd, (size_t)len + 1,
      "gst-launch-1.0 -q videotestsrc num-buffers=30 pattern=ball ! "
      "video/x-raw,width=720,height=1280,framerate=30/1 ! videoconvert ! "
      "openh264enc ! h264parse ! queue ! mux. "
      "audiotestsrc num-buffers=60 wave=silence ! audioconvert ! avenc_aac ! "
      "aacparse ! queue ! mux. mp4mux name=mux ! filesink location=\"%s\"",
      path);
    int status = system(cmd);
    free(cmd);
    if (status == 0) return;
    set_error("gstreamer mp4 pipeline failed; wrote fallback marker");
  }
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    set_error("failed to create output file");
    return;
  }
  if (strcmp(format, "webm") == 0) {
    fwrite("MAGVIDEO WEBM PLACEHOLDER\n", 1, 26, fp);
  } else {
    fwrite("MAGVIDEO MP4 PLACEHOLDER\n", 1, 25, fp);
  }
  fclose(fp);
}

TyaValue tya_magvideo_media_init(TyaValue __this, TyaValue a0, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a0; (void)a1; (void)a2; (void)a3;
  ensure_gst();
  return tya_bool(true);
}

TyaValue tya_magvideo_media_devices(TyaValue __this, TyaValue kind, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  ensure_gst();
  const char *k = str_arg(kind);
  if (strcmp(k, "microphone") == 0) return string_array("Default microphone");
  return string_array("Default camera");
}

TyaValue tya_magvideo_media_start_preview(TyaValue __this, TyaValue camera, TyaValue sink, TyaValue a2, TyaValue a3) {
  (void)__this; (void)camera; (void)sink; (void)a2; (void)a3;
  ensure_gst();
  return tya_number(next_recorder());
}

TyaValue tya_magvideo_media_stop_preview(TyaValue __this, TyaValue handle, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  Recorder *rec = recorder_for(handle);
  if (rec != NULL) rec->recording = 0;
  return tya_nil();
}

TyaValue tya_magvideo_media_start_recording(TyaValue __this, TyaValue camera, TyaValue microphone, TyaValue output, TyaValue format) {
  (void)__this; (void)camera; (void)microphone;
  ensure_gst();
  int id = next_recorder();
  Recorder *rec = &recorders[id];
  rec->recording = 1;
  rec->started_at = time(NULL);
  snprintf(rec->output_path, sizeof(rec->output_path), "%s", str_arg(output));
  char *dir = strdup(rec->output_path);
  char *slash = strrchr(dir, '/');
  if (slash != NULL) {
    *slash = '\0';
    mkdir_p(dir);
  }
  free(dir);
  write_placeholder_video(rec->output_path, str_arg(format));
  return tya_number(id);
}

TyaValue tya_magvideo_media_stop_recording(TyaValue __this, TyaValue handle, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  Recorder *rec = recorder_for(handle);
  if (rec == NULL) {
    set_error("unknown recorder handle");
    return tya_string("");
  }
  rec->recording = 0;
  return tya_string(strdup(rec->output_path));
}

TyaValue tya_magvideo_media_elapsed(TyaValue __this, TyaValue handle, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  Recorder *rec = recorder_for(handle);
  if (rec == NULL || rec->started_at == 0) return tya_number(0);
  return tya_number((double)(time(NULL) - rec->started_at));
}

TyaValue tya_magvideo_media_last_error(TyaValue __this, TyaValue a0, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a0; (void)a1; (void)a2; (void)a3;
  return tya_string(strdup(last_error));
}

TyaValue tya_magvideo_media_open_file(TyaValue __this, TyaValue path, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  const char *file = str_arg(path);
  if (file[0] == '\0') return tya_bool(false);
  if (fork() == 0) {
    execlp("xdg-open", "xdg-open", file, (char *)NULL);
    _exit(127);
  }
  return tya_bool(true);
}

TyaValue tya_magvideo_clipboard_set_file(TyaValue __this, TyaValue path, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a1; (void)a2; (void)a3;
  const char *file = str_arg(path);
  FILE *pipe = popen("wl-copy 2>/dev/null || xclip -selection clipboard 2>/dev/null", "w");
  if (pipe == NULL) return tya_bool(false);
  fputs(file, pipe);
  int status = pclose(pipe);
  return tya_bool(status == 0);
}

TyaValue tya_magvideo_config_dir(TyaValue __this, TyaValue a0, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a0; (void)a1; (void)a2; (void)a3;
  char *path = home_path(".config/magvideo");
  mkdir_p(path);
  return tya_string(path);
}

TyaValue tya_magvideo_output_dir(TyaValue __this, TyaValue a0, TyaValue a1, TyaValue a2, TyaValue a3) {
  (void)__this; (void)a0; (void)a1; (void)a2; (void)a3;
  char *path = home_path("Videos/magvideo");
  mkdir_p(path);
  return tya_string(path);
}
