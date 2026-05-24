# Contributing to ESP32 METAR OLED Display

Thanks for your interest in contributing! This is a small open source project and all contributions are welcome — whether it's a bug fix, a new feature, improved documentation, or just a question.

---

## Ways to Contribute

- **Bug reports** — open a GitHub Issue with your ESP32 board, OLED type, Arduino IDE version, and what you see in the Serial Monitor
- **Feature requests** — open an Issue describing what you'd like and why it's useful
- **Code contributions** — open a Pull Request (see below)
- **Documentation** — fix typos, improve wiring diagrams, add translations
- **Hardware testing** — test on a different ESP32 variant or OLED module and report results

---

## Before You Start

1. Check the [Issues list](../../issues) — your bug or idea may already be tracked
2. For significant changes, open an Issue first to discuss the approach before writing code
3. Keep PRs focused — one feature or fix per PR makes review much easier

---

## Development Setup

### Firmware (`firmware/metar_esp32.ino`)

- Arduino IDE 2.x with ESP32 core ≥ 2.0.x
- Libraries: U8g2 (olikraus), ArduinoJson (Benoit Blanchon v6 or v7)
- Test on real hardware if possible; Serial Monitor output is the main debug tool
- Keep the **USER CONFIGURATION** section at the top clean — all user-editable values should live there, not scattered through the code

### Simulator (`simulator/metar_oled.html`)

- Pure HTML + vanilla JavaScript — no build step, no npm, no dependencies
- Open directly in a browser to test
- The pixel-drawing primitives (`px`, `hline`, `vline`, `rect`, `circle`, `line`) mirror U8g2's API intentionally — keep them in sync if adding new display elements

---

## Code Style

**Arduino (.ino):**
- 2-space indentation
- `UPPER_CASE` for `#define` constants
- `camelCase` for functions and variables
- Comment any non-obvious logic, especially around the SNTP/NTP setup and HTTPS certificate handling
- Keep `renderOLED()` and `drawClockOverlay()` as the only functions that call U8g2 draw functions directly

**HTML/JavaScript:**
- 2-space indentation
- Pixel drawing helpers (`px`, `drawText`, `drawSeg7`) should remain pure functions — no side effects other than drawing to the canvas
- `renderOLED(r, cat)` is the single entry point for a full OLED redraw — keep it that way

---

## Pull Request Process

1. Fork the repository
2. Create a branch: `git checkout -b feature/your-feature-name`
3. Make your changes
4. Test on hardware (firmware) or in a browser (simulator)
5. Commit with a clear message: `git commit -m "Add TAF second screen support"`
6. Push to your fork: `git push origin feature/your-feature-name`
7. Open a Pull Request against `main`

PR description should include:
- What changed and why
- How you tested it
- Any hardware/software versions used

---

## Suggested Future Features

If you're looking for something to work on:

| Feature | Difficulty | Notes |
|---|---|---|
| TAF forecast second page | Medium | NOAA TAF endpoint: `/api/data/taf` — same format |
| Web config portal | Medium | Use `WiFiManager` library — avoids hardcoded credentials |
| Button to cycle stations | Easy | Single pushbutton on any GPIO, debounce in loop() |
| SIGMET / AIRMET alert | Medium | NOAA has a SIGMET endpoint |
| Deep sleep + battery mode | Hard | Wake on timer, fetch, display, sleep |
| PCB design (KiCad) | Hard | Would love a proper PCB for this |
| Unit tests for METAR parser | Easy | Pure JS — can test the `parse()` function in Node |
| METAR history chart | Medium | Store last N readings, render trend line on OLED |

---

## Reporting Security Issues

This project fetches data from public APIs over HTTPS. If you find a security issue (e.g. certificate validation bypass, credential exposure), please open a private GitHub Security Advisory rather than a public Issue.

---

## Licence

By contributing, you agree that your contributions will be licensed under the same MIT licence as the rest of the project.
