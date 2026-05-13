# magvideo

Magvideo is a small Tya + GTK4 desktop app for recording short vertical selfie videos.

## Requirements

- Tya with native package support
- GTK4 development files
- GStreamer development files:
  - `gstreamer-1.0`
  - `gstreamer-video-1.0`
  - `gstreamer-audio-1.0`
  - `gstreamer-app-1.0`

## Development

```sh
pkg-config --exists gtk4
pkg-config --exists gstreamer-1.0
pkg-config --exists gstreamer-video-1.0
pkg-config --exists gstreamer-audio-1.0
tya install
tya doctor native
tya test
tya run src/main.tya
```

The first release keeps media handling behind `native/magvideo_media.c`. Headless tests verify device enumeration, recording state transitions, MP4 default output, clipboard fallback, and upload URL generation. Manual camera and microphone smoke testing is still required on a desktop session.

## Settings

Settings are stored in `~/.config/magvideo/settings.toml`. Defaults use MP4/H.264/AAC compatibility mode, 720x1280 geometry, 30 fps, and local file clipboard mode. WebM is available as an explicit open-format option and may not play in Discord on iOS/iPadOS.
