# Copilot instructions

Read [`AGENTS.md`](../AGENTS.md) in the repository root first. It describes the architecture, the build, the size budget of the firmware, the storage constraints of the settings flash area, the rules for the web interface and how to test without hardware.

Short version: ESP32 firmware (PlatformIO, Arduino) plus a vanilla JS web interface in `data/`. Keep the firmware below the app partition size, bump the `?v=` cache parameters in `data/app.html` when changing `app.js` or `style.css`, and never commit secrets.
