# mugvideo

Mugvideo is a small C + GTK4 desktop app for recording short selfie videos.

## Requirements

- GTK4 development files
- GStreamer development files:
  - `gstreamer-1.0`
  - `gstreamer-app-1.0`

## Development

```sh
pkg-config --exists gtk4
pkg-config --exists gstreamer-1.0
pkg-config --exists gstreamer-app-1.0
make
./mugvideo
```

The current C version provides a GTK4 UI, GStreamer camera preview, device selection, recording state UI, settings persistence, MP4 recording, and clipboard copy of the saved file path.

## Settings

Settings are stored in `~/.config/mugvideo/settings.ini`. The default output directory is `~/Videos/mugvideo`.
