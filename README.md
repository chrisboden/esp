# ESP32 Tool-Calling Agent

An ESP32 that can reason and act — powered by any tool-calling LLM via OpenRouter.

The LLM is the runtime. The firmware is just plumbing. Give the agent a small set of primitives (GPIO, HTTP, ADC) and the model composes them into arbitrary behaviors. No scripting engine, no MicroPython. Dumb tools, smart model.

## Hardware

**TTGO T-Display** — ESP32, 16MB flash, 1.14" color TFT (135x240), two buttons, LiPo connector.

## Tools

| Tool | Description |
|------|-------------|
| `pin_read` | Read digital GPIO pin |
| `pin_write` | Write digital GPIO pin |
| `adc_read` | Read analog value |
| `get_status` | Report system info (heap, uptime, WiFi) |
| `http_request` | Fetch data from HTTP/HTTPS URLs |
| `set_display_color` | Set TFT background color by name |

## How It Works

1. Press a button (or send a prompt over serial)
2. Display turns blue (thinking) — prompt is sent to the LLM via OpenRouter
3. Display turns green as the model calls tools
4. The model's final response is displayed on the TFT

Left button triggers a status check. Right button fetches a remote prompt and generates a haiku.

## Setup

1. Copy `esp32-agent/credentials.h.example` to `esp32-agent/credentials.h` and fill in your WiFi SSID/password and OpenRouter API key
2. Install dependencies:
   ```bash
   arduino-cli lib install ArduinoJson TFT_eSPI
   ```
3. Build and flash:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32:FlashSize=16M,FlashMode=dio,UploadSpeed=460800 esp32-agent
   arduino-cli upload --fqbn esp32:esp32:esp32:FlashSize=16M,FlashMode=dio,UploadSpeed=460800 --port /dev/cu.usbserial-5A5B0109761 esp32-agent
   ```

## Status

Phase 1 (agent loop) and Phase 2.5 (display + buttons) are complete. See [mvp_agent.md](mvp_agent.md) for the full build plan.
