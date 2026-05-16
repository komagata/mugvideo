# Repository Guidelines

## Project Structure & Module Organization

Mugvideo is a small C + GTK4 desktop app for recording short selfie videos. The application entry point is `src/main.c`. Build rules live in `Makefile`. Keep generated binaries, object files, and local output videos out of git.

## Build, Test, and Development Commands

- `pkg-config --exists gtk4`: verify GTK4 development files are installed.
- `pkg-config --exists gstreamer-1.0`: verify GStreamer is available.
- `pkg-config --exists gstreamer-app-1.0`: verify GStreamer appsink support is available.
- `make`: build the `mugvideo` binary.
- `make run`: launch the desktop app locally.
- `make clean`: remove the compiled binary.

## Coding Style & Naming Conventions

Keep changes surgical and consistent with the current C style: two-space indentation, small static helper functions, and `snake_case` names. Prefer GTK and GStreamer APIs directly over custom abstractions unless duplication becomes meaningful. Keep UI layout changes in `src/main.c` until the file becomes too large to navigate.

## Testing Guidelines

There is no automated test suite yet. For now, verify C changes with `make` and a short desktop smoke test with `./mugvideo` or `make run`. Camera preview, microphone/device selection, floating startup behavior, and recording button state changes require manual testing in a desktop session.

## Commit & Pull Request Guidelines

Git history uses short imperative commit subjects, for example `Add Mugvideo desktop app` and `Switch app to C GTK4`. Keep commits focused on one behavior or maintenance task. For pull requests, include a concise summary, build results such as `make`, any manual media-device checks performed, and screenshots or short recordings for visible GTK UI changes.

## Security & Configuration Tips

Do not commit local settings, generated recordings, credentials, upload destinations with secrets, or machine-specific build artifacts such as the compiled `mugvideo` binary.
