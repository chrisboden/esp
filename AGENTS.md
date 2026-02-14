# ESP32 Tool-Calling Agent

See `mvp_agent.md` for full design doc, architecture, and build plan.

## Board

**TTGO T-Display** (Tenstar clone) — ESP32-D0WDQ6 rev v1.1, 16MB flash, CH340 USB.

- **Display:** ST7789V 1.14" IPS TFT, 135x240 pixels (SPI)
  - MOSI=19, SCLK=18, CS=5, DC=16, RST=23, BL=4
- **Buttons:** GPIO0 (left, active LOW) and GPIO35 (right, active LOW, input-only)
- **Battery:** JST connector for LiPo
- **Serial port:** `/dev/cu.usbserial-5A5B0109761`
- **Library:** TFT_eSPI (use TTGO T-Display setup)

## Project Structure

```
esp32-agent/           Arduino sketch (the firmware)
  esp32-agent.ino      Main sketch file
  credentials.h        WiFi + API key (gitignored)
wifi-debug/            Minimal WiFi debug sketch
i2c-scan/              Hardware detection utility sketch
mvp_agent.md           Design doc, architecture, build plan
```

## Build & Flash

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:FlashSize=16M,FlashMode=dio,UploadSpeed=460800 esp32-agent
arduino-cli upload --fqbn esp32:esp32:esp32:FlashSize=16M,FlashMode=dio,UploadSpeed=460800 --port /dev/cu.usbserial-5A5B0109761 esp32-agent
```

Upload speed MUST be 460800 — 921600 causes "chip stopped responding".

## Gotchas

**WiFi:** Router is on channel 13 with band steering. Must set country to AU (ch 1-13), enable PMF (`pmf_cfg.capable = true`), and auto-retry on disconnect reason 208.

**Serial monitor reset:**
```python
ser.dtr = False    # keep GPIO0 HIGH (normal boot)
ser.rts = True     # assert reset
time.sleep(0.3)
ser.rts = False    # release reset
```
Do NOT toggle DTR — it boots into download mode.

**ArduinoJson:** Stream filter doesn't work with OpenRouter responses. Use `getString()` + `deserializeJson()` instead.

**lwIP pbuf crash (OPEN BUG):** When `http_request` tool runs and then `postChat` opens a new TLS connection, ESP32 crashes with `pbuf_free: p->ref > 0`. The tool's HTTP client buffers aren't fully released. Fix: heap-allocate the tool's WiFiClient, call `client->stop()` + `delete` + `delay(10)` after `http.end()`. Not yet applied/tested.

**minimax model:** Ignores `parallel_tool_calls: false` — may return multiple tool calls in one response. Code handles this (iterates the array).
