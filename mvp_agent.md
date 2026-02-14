# ESP32 Tool-Calling Agent via OpenRouter

An ESP32 that can reason, act, and learn — powered by any tool-calling LLM via OpenRouter.

**Core insight:** The LLM is the runtime. The firmware is just plumbing. Give the agent a small set of primitives (file I/O, HTTP, GPIO) and the LLM composes them into arbitrary behaviors. No scripting engine, no dynamic tool loading, no MicroPython. Dumb tools, smart model.

Storage tiers: **RAM-only** (no persistence), **SPIFFS/LittleFS** (onboard flash), **SD card** (external). The firmware abstracts filesystem calls so the same code works with SPIFFS or SD — just swap the mount.

## Target Devices

| Device | Flash | RAM | Storage | Notes |
|---|---|---|---|---|
| **ESP32-D0WDQ6 (current)** | 16MB | 320KB | 1.44MB SPIFFS partition | Dual-core 240MHz, CH340 USB, no SD slot |
| **ESP32 minimal** | small | limited | RAM-only | Lite tier only |
| **ESP32-CAM** | varies | varies | SD card slot | Full tier with SD storage |

Current work is on the first device using SPIFFS for persistence.

---

## Build Plan

### Phase 1: Lite — Get the agent loop talking ✅
- [x] Create Arduino sketch from Lite tier code
- [x] Configure WiFi credentials and OpenRouter API key
- [x] Install ArduinoJson library via arduino-cli
- [x] Compile for `esp32:esp32:esp32` (16MB flash, DIO)
- [x] Flash to board via `/dev/cu.usbserial-5A5B0109761`
- [x] Test: plain text exchange over serial (no tool calls)
- [x] Test: tool calls work — `get_status`, `adc_read`, `pin_read`

**Notes:** WiFi required AU country code (ch13), PMF, and band steering retries (~7 disconnects before connecting). ArduinoJson stream filter doesn't work with OpenRouter responses — using `getString()` + `deserializeJson()` instead.

### Phase 2: Harden the core — IN PROGRESS
- [x] Add HTTP timeout to `postChat()` (30s timeout already in place)
- [x] Add pin mode guard to `pin_read`/`pin_write` (configuredInputs/configuredOutputs bitmask)
- [x] Add hardcoded system prompt to Lite tier (in firmware)
- [x] Add `http_request` tool to Lite — supports both HTTP and HTTPS
- [ ] **BUG: lwIP pbuf crash on second API call after `http_request` tool** (see below)
- [ ] Handle `finish_reason: "length"` (model ran out of max_tokens) — treat as stop
- [ ] Test: multi-step tool-call chain (LLM calls tool, reasons, calls another)

**Outstanding bug — lwIP pbuf_free crash:**
When the agent calls `http_request` (tool makes an HTTP connection) and then `postChat` opens a new TLS connection to OpenRouter for the follow-up, the ESP32 crashes with `pbuf_free: p->ref > 0` assertion in lwIP. The network buffers from the tool's HTTP client aren't fully released before the next TLS handshake begins. Fix: ensure the tool's HTTP client and underlying WiFiClient are fully stopped/deleted and lwIP has time to clean up before `postChat` runs. Heap-allocating the client (not stack) and calling `client->stop()` + `delete` + short `delay()` after `http.end()` is the likely fix — not yet tested.

**Other notes:**
- `max_tokens` bumped from 150 to 300 (150 too small for multi-step answers)
- minimax model ignores `parallel_tool_calls: false` — returns multiple tool calls in one response. Code handles this fine (iterates the array), but be aware.
- Plain text and single-tool-call prompts work perfectly. The crash only happens when `http_request` tool is followed by another API call.

### Phase 2.5: Display & Buttons — T-Display TFT + GPIO triggers
- [ ] Install TFT_eSPI library, configure User_Setup for T-Display (ST7789V 135x240, MOSI=19, SCLK=18, CS=5, DC=16, RST=23, BL=4)
- [ ] TFT init in `setup()`, landscape orientation (240x135), backlight on
- [ ] `showScreen(bgColor, text)` helper — fills background color, draws white word-wrapped text
- [ ] Background color = agent state: black=idle, dark blue=thinking, dark green=tool executing, dark red=error
- [ ] Button A (GPIO0, left) triggers "status" prompt → agent checks status and displays on screen
- [ ] Button B (GPIO35, right) triggers "sense" prompt → agent reads ADC and displays on screen
- [ ] New `display` tool — agent calls `display(text)` to write its own chosen text to screen
- [ ] If agent finishes without calling `display`, final text answer auto-displays
- [ ] System prompt updated to mention display tool and buttons
- [ ] Button debounce: ignore presses while agent is running
- [ ] Test: press button → screen goes blue → green → shows result on black

**Display pin mapping (TTGO T-Display / Tenstar clone):**
```
MOSI=19  SCLK=18  CS=5  DC=16  RST=23  BL=4
Buttons: GPIO0 (left, active LOW), GPIO35 (right, active LOW, input-only)
```

### Phase 3: Persistence — SPIFFS on current device
- [ ] Mount SPIFFS (1.44MB partition already exists)
- [ ] Add `read_file`, `write_file`, `list_dir` tools backed by SPIFFS
- [ ] Implement session management (create, append, JSONL)
- [ ] Implement sliding context window (load last N messages)
- [ ] Test: multi-turn conversation persists across reboots

### Phase 4: Triggers & scheduling
- [ ] Implement scheduled tasks (millis-based cron)
- [ ] Implement GPIO interrupt triggers
- [ ] Implement threshold monitors (polled ADC with configurable thresholds)
- [ ] Implement serial commands (`/sessions`, etc.)
- [ ] Test: scheduled prompt fires and completes agent loop autonomously

### Phase 5: Robustness
- [x] WiFi reconnect in main loop (already implemented)
- [ ] Graceful handling of API errors (rate limit, timeout, malformed response)
- [ ] Test: run unattended for 24h with a scheduled task

### Future: SD card support (ESP32-CAM)
- [ ] Abstract filesystem layer so SD and SPIFFS share the same code
- [ ] Wire SD card module on ESP32-CAM
- [ ] Test: same firmware works with SD storage

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        FIRMWARE                             │
│                   (flash once, stable)                      │
│                                                             │
│  ┌──────────┐   ┌────────────┐   ┌───────────────────────┐ │
│  │ Triggers │──→│ Agent Loop │──→│ Tool Dispatch         │ │
│  │          │   │            │   │                       │ │
│  │ - Cron   │   │ 1. POST    │   │ Primitives:           │ │
│  │ - GPIO   │   │ 2. Parse   │   │  read_file write_file │ │
│  │ - MQTT   │   │ 3. Dispatch│   │  list_dir  http_req   │ │
│  │ - HTTP   │   │ 4. Loop    │   │  pin_read  pin_write  │ │
│  │ - Serial │   │            │   │  adc_read  get_status │ │
│  └──────────┘   └─────┬──────┘   └───────────────────────┘ │
│                       │                                     │
│               ┌───────▼────────┐                            │
│               │ Session Mgmt   │                            │
│               │ (JSONL on SD)  │                            │
│               └────────────────┘                            │
└─────────────────────────────────────────────────────────────┘
                        │
                        ▼
              ┌───────────────────┐
              │    OpenRouter     │
              │  (any LLM with   │
              │   tool calling)  │
              └───────────────────┘
```

### What the firmware does (dumb, stable, flash-once):

- Connects WiFi
- Runs the agent loop: POST → parse → dispatch tool → POST → ... → done
- Persists every message to JSONL on SD
- Dispatches tool calls to compiled primitive handlers
- Manages triggers (cron, GPIO interrupts, MQTT, HTTP, serial)

### What the LLM does (smart, all the reasoning):

- Decides which tools to call and in what order
- Composes primitives into complex behaviors
- Reads/writes files on SD for its own memory and notes
- Makes HTTP calls to external APIs
- Interprets sensor data and decides on actions

---

## The Agent Loop

Both tiers share the same 3-state machine:

```
┌─────────────────────────────────────┐
│  1. POST messages[] + tools[] ──────┼──→ OpenRouter
│                                     │
│  2. Parse response:                 │
│     ├─ finish_reason == "tool_calls"│
│     │  → execute locally            │
│     │  → append result to messages[]│
│     │  → GOTO 1                     │
│     │                               │
│     └─ finish_reason == "stop"      │
│        → output content, DONE       │
└─────────────────────────────────────┘
```

---

## Primitives (The Tool Set)

Instead of many specialized tools, use a small fixed set of primitives. The LLM composes them into any behavior. All fit in every request — no discovery stage, no dynamic loading, no tool plugin system.

```
Filesystem:   read_file, write_file, list_dir
Network:      http_request
Hardware:     pin_read, pin_write, adc_read
System:       get_status
```

**8 tools total.** ~1.5KB in the API payload. All compiled in firmware. Never change.

### Why primitives, not specialized tools

| Approach | Tools | Payload | Reflash to extend? |
|---|---|---|---|
| Specialized (read_dht, set_servo, ...) | 15-30 | 3-6KB | Yes, for every new sensor/device |
| Primitives | 8 | ~1.5KB | No — LLM composes what it needs |

The LLM already knows sensor protocols, API formats, data structures. Give it `http_request` and it can call any API. Give it `read_file` / `write_file` and it can maintain its own knowledge base. Give it `pin_read` / `pin_write` / `adc_read` and it can interact with hardware.

For complex sensors (DHT22, BME280, GPS) that need specific timing or protocols beyond raw GPIO, keep a small set of higher-level hardware handlers compiled in firmware. But the *composition* — the "if temperature > 30, turn on fan AND post to Slack" logic — that's all the LLM.

### Primitive Definitions (tools.json)

```json
[
  {
    "type": "function",
    "function": {
      "name": "read_file",
      "description": "Read a text file from SD card. Returns file contents (max 1KB).",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "File path, e.g. /data/log.csv"}
        },
        "required": ["path"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "write_file",
      "description": "Write text to a file on SD card. Creates or overwrites.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "File path"},
          "content": {"type": "string", "description": "Text content to write"},
          "append": {"type": "boolean", "description": "Append instead of overwrite (default false)"}
        },
        "required": ["path", "content"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "list_dir",
      "description": "List files and directories at a path on SD card.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": {"type": "string", "description": "Directory path (default /)"}
        }
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "http_request",
      "description": "Make an HTTP request. Returns status code and response body (max 1KB).",
      "parameters": {
        "type": "object",
        "properties": {
          "url": {"type": "string", "description": "Full URL"},
          "method": {"type": "string", "enum": ["GET", "POST", "PUT", "DELETE"], "description": "HTTP method (default GET)"},
          "body": {"type": "string", "description": "Request body (for POST/PUT)"},
          "headers": {"type": "object", "description": "Additional headers as key-value pairs"}
        },
        "required": ["url"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "pin_read",
      "description": "Read digital state of a GPIO pin. Returns 0 or 1.",
      "parameters": {
        "type": "object",
        "properties": {
          "pin": {"type": "integer", "description": "GPIO pin number"}
        },
        "required": ["pin"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "pin_write",
      "description": "Set a GPIO pin high (1) or low (0).",
      "parameters": {
        "type": "object",
        "properties": {
          "pin": {"type": "integer", "description": "GPIO pin number"},
          "value": {"type": "integer", "enum": [0, 1], "description": "0=LOW, 1=HIGH"}
        },
        "required": ["pin", "value"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "adc_read",
      "description": "Read analog voltage from an ADC pin. Returns voltage as float (0-3.3V).",
      "parameters": {
        "type": "object",
        "properties": {
          "pin": {"type": "integer", "description": "ADC-capable GPIO pin number"}
        },
        "required": ["pin"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "get_status",
      "description": "Get system status: free heap (bytes), uptime (seconds), WiFi IP, battery voltage.",
      "parameters": {
        "type": "object",
        "properties": {}
      }
    }
  }
]
```

### Primitive Handlers (compiled C++)

```cpp
typedef String (*ToolHandler)(JsonObject args);

String handleReadFile(JsonObject args) {
  File f = SD.open(args["path"].as<const char*>(), FILE_READ);
  if (!f) return "file not found";
  String content = f.readString();
  f.close();
  return content.substring(0, 1024);
}

String handleWriteFile(JsonObject args) {
  const char* path = args["path"];
  const char* content = args["content"];
  bool append = args["append"] | false;
  File f = SD.open(path, append ? FILE_APPEND : FILE_WRITE);
  if (!f) return "write failed";
  f.print(content);
  f.close();
  return "ok";
}

String handleListDir(JsonObject args) {
  const char* path = args["path"] | "/";
  File dir = SD.open(path);
  if (!dir) return "dir not found";
  String result = "[";
  File entry; bool first = true;
  while ((entry = dir.openNextFile())) {
    if (!first) result += ",";
    result += "{\"name\":\"" + String(entry.name()) + "\"";
    result += entry.isDirectory() ? ",\"type\":\"dir\"" : ",\"size\":" + String(entry.size());
    result += "}";
    first = false;
    entry.close();
  }
  dir.close();
  return result + "]";
}

String handleHttpRequest(JsonObject args) {
  const char* url = args["url"];
  const char* method = args["method"] | "GET";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  // Add custom headers
  if (args["headers"].is<JsonObject>()) {
    for (JsonPair h : args["headers"].as<JsonObject>()) {
      http.addHeader(h.key().c_str(), h.value().as<const char*>());
    }
  }

  int code;
  if (strcmp(method, "POST") == 0)
    code = http.POST(args["body"] | "");
  else if (strcmp(method, "PUT") == 0)
    code = http.PUT(args["body"] | "");
  else if (strcmp(method, "DELETE") == 0)
    code = http.sendRequest("DELETE");
  else
    code = http.GET();

  String body = (code > 0) ? http.getString().substring(0, 1024) : "error";
  http.end();
  return "{\"status\":" + String(code) + ",\"body\":" +
         escapeJsonString(body) + "}";
}

// Track which pins have been configured to avoid clobbering interrupts
uint64_t configuredInputs = 0, configuredOutputs = 0;

String handlePinRead(JsonObject args) {
  int pin = args["pin"];
  if (!(configuredInputs & (1ULL << pin))) {
    pinMode(pin, INPUT);
    configuredInputs |= (1ULL << pin);
  }
  return String(digitalRead(pin));
}

String handlePinWrite(JsonObject args) {
  int pin = args["pin"]; int val = args["value"];
  if (!(configuredOutputs & (1ULL << pin))) {
    pinMode(pin, OUTPUT);
    configuredOutputs |= (1ULL << pin);
  }
  digitalWrite(pin, val);
  return val ? "HIGH" : "LOW";
}

String handleAdcRead(JsonObject args) {
  int pin = args["pin"];
  return String(analogRead(pin) * 3.3 / 4095, 3);
}

String handleGetStatus(JsonObject args) {
  return "{\"heap\":" + String(ESP.getFreeHeap()) +
         ",\"uptime\":" + String(millis() / 1000) +
         ",\"wifi\":\"" + WiFi.localIP().toString() + "\"}";
}

// ---- Registry ----
struct ToolEntry { const char* name; ToolHandler handler; };

ToolEntry TOOL_REGISTRY[] = {
  {"read_file",     handleReadFile},
  {"write_file",    handleWriteFile},
  {"list_dir",      handleListDir},
  {"http_request",  handleHttpRequest},
  {"pin_read",      handlePinRead},
  {"pin_write",     handlePinWrite},
  {"adc_read",      handleAdcRead},
  {"get_status",    handleGetStatus},
  {nullptr,         nullptr}
};

String execTool(const char* name, JsonObject args) {
  for (int i = 0; TOOL_REGISTRY[i].name; i++) {
    if (strcmp(name, TOOL_REGISTRY[i].name) == 0)
      return TOOL_REGISTRY[i].handler(args);
  }
  return "unknown tool: " + String(name);
}
```

### What the LLM does with primitives

"Check the weather and if rain is forecast, close the greenhouse vent":
1. `http_request` → GET weather API
2. LLM reasons about the JSON response
3. `pin_write` → set relay pin HIGH to close vent
4. `write_file` → log the action to `/data/actions.log`

"Read all sensors and email me a daily report":
1. `adc_read` → read each sensor pin
2. `write_file` → append to `/data/daily.csv`
3. `read_file` → read the accumulated CSV
4. `http_request` → POST to email API / webhook

The LLM can also build up its own knowledge on the SD card:
- `write_file` → save a "procedures" file with recipes it's learned
- `read_file` → load those procedures in future sessions via system prompt
- Self-improving agent without MicroPython or firmware changes

---

## Tier Comparison

| Capability | Lite (no SD) | Full (with SD) |
|---|---|---|
| Agent loop | Yes | Yes |
| Primitives | GPIO + ADC only | All 8 (incl. filesystem, HTTP) |
| Max context | ~6-10 messages (RAM) | Unlimited (sliding window) |
| Persistence | None — lost on reboot | JSONL sessions on SD |
| Resume chats | No | Yes |
| System prompt | Hardcoded | Loaded from SD (`/config/system.txt`) |
| Tool definitions | Hardcoded | Loaded from SD (`/config/tools.json`) |
| Credentials | Hardcoded | Loaded from SD (`/config/config.json`) |
| Triggers | Serial only | Cron, GPIO, MQTT, HTTP, Serial |
| Agent memory | None | LLM reads/writes files on SD |
| `max_tokens` | 150 | 512+ |

---

## OpenRouter API Reference

**Endpoint:** `POST https://openrouter.ai/api/v1/chat/completions`

**Headers:**
```
Content-Type: application/json
Authorization: Bearer sk-or-v1-xxx
```

### Request (with tools)

```json
{
  "model": "openai/gpt-5.2-codex",
  "max_tokens": 512,
  "parallel_tool_calls": false,
  "messages": [
    {"role": "system", "content": "You are an agent on an ESP32..."},
    {"role": "user", "content": "What's the voltage on pin 34?"}
  ],
  "tools": [ ...tool definitions... ]
}
```

### Response: Tool Call

```json
{
  "choices": [{
    "finish_reason": "tool_calls",
    "message": {
      "role": "assistant",
      "content": null,
      "tool_calls": [{
        "id": "call_abc123",
        "type": "function",
        "function": {
          "name": "adc_read",
          "arguments": "{\"pin\": 34}"
        }
      }]
    }
  }]
}
```

### Follow-Up (tool result)

The `tools` array must be in EVERY request. The assistant message (with `tool_calls`) must appear in `messages[]` before the tool result.

```json
{
  "model": "openai/gpt-5.2-codex",
  "messages": [
    {"role": "user", "content": "What's the voltage on pin 34?"},
    {
      "role": "assistant", "content": null,
      "tool_calls": [{"id": "call_abc123", "type": "function",
        "function": {"name": "adc_read", "arguments": "{\"pin\": 34}"}}]
    },
    {"role": "tool", "tool_call_id": "call_abc123", "content": "2.31"}
  ],
  "tools": [ ...same tools... ]
}
```

### Response: Final Answer

```json
{
  "choices": [{
    "finish_reason": "stop",
    "message": {
      "role": "assistant",
      "content": "Pin 34 reads 2.31V."
    }
  }]
}
```

### Model Selection

| Model | Cost (in/out per 1M tok) | Notes |
|---|---|---|
| `openai/gpt-5.2-codex` | Fast, reliable tool calling |
| `openai/gpt-5.1-codex-mini` | Cheapest viable option |


---

# Lite Tier — RAM Only

For ESP32 boards with no SD card. Hardcoded config, ephemeral conversations, GPIO/ADC primitives only.

## Memory Budget

| Component | RAM |
|---|---|
| WiFi + TLS | ~80KB |
| JSON request doc | ~4KB |
| JSON response (filtered) | ~2KB |
| HTTP buffers | ~2KB |
| **Total** | **~88KB** of ~320KB usable |

## Implementation

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "xxx";
const char* WIFI_PASS = "xxx";
const char* API_KEY   = "sk-or-v1-xxx";
const char* MODEL     = "openai/gpt-5.2-codex";
const char* API_URL   = "https://openrouter.ai/api/v1/chat/completions";
const int   MAX_TOKENS    = 150;
const int   MAX_ITERATIONS = 5;

// ---- Primitives (Lite: GPIO + ADC only) ----
uint64_t configuredInputs = 0, configuredOutputs = 0;

String execTool(const char* name, JsonObject args) {
  if (strcmp(name, "pin_read") == 0) {
    int pin = args["pin"];
    if (!(configuredInputs & (1ULL << pin))) {
      pinMode(pin, INPUT);
      configuredInputs |= (1ULL << pin);
    }
    return String(digitalRead(pin));
  }
  if (strcmp(name, "pin_write") == 0) {
    int pin = args["pin"]; int val = args["value"];
    if (!(configuredOutputs & (1ULL << pin))) {
      pinMode(pin, OUTPUT);
      configuredOutputs |= (1ULL << pin);
    }
    digitalWrite(pin, val);
    return val ? "HIGH" : "LOW";
  }
  if (strcmp(name, "adc_read") == 0) {
    int pin = args["pin"];
    return String(analogRead(pin) * 3.3 / 4095, 3);
  }
  if (strcmp(name, "get_status") == 0) {
    return "{\"heap\":" + String(ESP.getFreeHeap()) +
           ",\"uptime\":" + String(millis() / 1000) + "}";
  }
  return "unknown tool";
}

void buildTools(JsonArray tools) {
  auto addTool = [&](const char* name, const char* desc,
                     const char* params) {
    JsonObject t = tools.add<JsonObject>();
    t["type"] = "function";
    t["function"]["name"] = name;
    t["function"]["description"] = desc;
    JsonDocument p; deserializeJson(p, params);
    t["function"]["parameters"] = p.as<JsonObject>();
  };

  addTool("pin_read", "Read digital GPIO pin (returns 0 or 1)",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}");
  addTool("pin_write", "Set GPIO pin HIGH (1) or LOW (0)",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\",\"enum\":[0,1]}},\"required\":[\"pin\",\"value\"]}");
  addTool("adc_read", "Read analog voltage (0-3.3V) from ADC pin",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}");
  addTool("get_status", "Get system status (heap, uptime)",
    "{\"type\":\"object\",\"properties\":{}}");
}

// ---- HTTP POST ----
int postChat(const String& body, JsonDocument& resp) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, API_URL);
  http.setTimeout(30000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + API_KEY);
  int code = http.POST(body);
  if (code == 200) {
    JsonDocument filter;
    filter["choices"][0]["finish_reason"] = true;
    filter["choices"][0]["message"]["content"] = true;
    filter["choices"][0]["message"]["role"] = true;
    filter["choices"][0]["message"]["tool_calls"] = true;
    deserializeJson(resp, http.getStream(),
                    DeserializationOption::Filter(filter));
  }
  http.end();
  return code;
}

// ---- Agent loop ----
String agentLoop(const char* userPrompt) {
  JsonDocument doc;
  JsonArray msgs = doc["messages"].to<JsonArray>();
  JsonObject userMsg = msgs.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = userPrompt;

  doc["model"] = MODEL;
  doc["max_tokens"] = MAX_TOKENS;
  doc["parallel_tool_calls"] = false;
  buildTools(doc["tools"].to<JsonArray>());

  for (int i = 0; i < MAX_ITERATIONS; i++) {
    String body; serializeJson(doc, body);
    JsonDocument resp;
    int code = postChat(body, resp);
    if (code != 200) return "HTTP " + String(code);

    const char* finish = resp["choices"][0]["finish_reason"];
    JsonObject msg = resp["choices"][0]["message"];

    if (strcmp(finish, "stop") == 0)
      return msg["content"].as<String>();

    if (strcmp(finish, "tool_calls") == 0) {
      JsonObject asstMsg = msgs.add<JsonObject>();
      asstMsg["role"] = "assistant";
      asstMsg["content"] = (const char*)nullptr;
      JsonArray tc = asstMsg["tool_calls"].to<JsonArray>();

      for (JsonObject call : msg["tool_calls"].as<JsonArray>()) {
        JsonObject tcEntry = tc.add<JsonObject>();
        tcEntry["id"] = call["id"];
        tcEntry["type"] = "function";
        tcEntry["function"]["name"] = call["function"]["name"];
        tcEntry["function"]["arguments"] = call["function"]["arguments"];

        JsonDocument argsDoc;
        deserializeJson(argsDoc,
                        call["function"]["arguments"].as<const char*>());
        String result = execTool(call["function"]["name"],
                                 argsDoc.as<JsonObject>());

        JsonObject toolMsg = msgs.add<JsonObject>();
        toolMsg["role"] = "tool";
        toolMsg["tool_call_id"] = call["id"];
        toolMsg["content"] = result;
      }
      continue;
    }
    return "unexpected: " + String(finish);
  }
  return "max iterations";
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("Ready.");
}

void loop() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      String answer = agentLoop(input.c_str());
      Serial.println(answer);
    }
  }
}
```

---

# Full Tier — With SD Card

SD card unlocks all 8 primitives, persistent sessions, configurable prompts, triggers/scheduling, and agent self-memory.

## SD Card Layout

```
/sd/
├── config/
│   ├── config.json              # WiFi, API key, model, limits
│   ├── system.txt               # System prompt (plain text)
│   ├── tools.json               # Tool definitions array
│   ├── schedule.json            # Cron-like scheduled tasks
│   └── triggers.json            # GPIO/MQTT event triggers
│
├── sessions/
│   ├── index.json               # Session list: [{id, title, created}]
│   ├── sess_0001.jsonl          # One JSON object per message
│   └── ...
│
└── agent/                       # Agent's own workspace
    ├── notes.txt                # Agent can write/read its own notes
    ├── procedures/              # Recipes the agent has learned
    └── data/                    # Data files the agent creates
```

### config.json

```json
{
  "wifi_ssid": "MyNetwork",
  "wifi_pass": "password123",
  "api_key": "sk-or-v1-xxx",
  "model": "openai/gpt-5.2-codex",
  "max_tokens": 512,
  "max_iterations": 10,
  "context_window": 20
}
```

### system.txt

```
You are an agent running on an ESP32 microcontroller with an SD card.

You have 8 tools: read_file, write_file, list_dir, http_request,
pin_read, pin_write, adc_read, get_status.

You can compose these primitives to accomplish any task:
- Read sensors via adc_read or pin_read
- Control devices via pin_write
- Call any API or webhook via http_request
- Store notes, data, and procedures in /agent/ on the SD card
- Read your own notes from previous sessions for continuity

Be concise. Prefer short tool results. Log important actions to /agent/log.txt.
```

### schedule.json

```json
{
  "tasks": [
    {
      "every_sec": 300,
      "prompt": "Read the ADC on pin 34 (soil moisture). If voltage is below 1.0V (dry), set pin 13 HIGH for 60 seconds to irrigate, then set it LOW. Log reading and action to /agent/data/moisture.csv."
    },
    {
      "every_sec": 3600,
      "prompt": "Check all sensor readings. Append a summary line to /agent/data/hourly.csv."
    },
    {
      "every_sec": 86400,
      "prompt": "Read /agent/data/hourly.csv. Summarize the day's data and POST a report to https://hooks.example.com/daily-report."
    }
  ]
}
```

The scheduled prompts are natural language. The cron job AND the business logic in one sentence. The LLM figures out which primitives to call.

### triggers.json

```json
{
  "triggers": [
    {
      "type": "gpio",
      "pin": 12,
      "edge": "falling",
      "prompt": "Button on pin 12 was pressed. Read /agent/procedures/button_action.txt for what to do."
    },
    {
      "type": "gpio",
      "pin": 27,
      "edge": "rising",
      "prompt": "Motion detected on PIR sensor (pin 27). Check the time. If after 10pm, POST an alert to https://hooks.example.com/motion."
    },
    {
      "type": "threshold",
      "pin": 35,
      "check_sec": 30,
      "above": 2.5,
      "prompt": "Temperature sensor on pin 35 reads ${value}V — above 2.5V threshold. Decide whether to activate cooling."
    }
  ]
}
```

## Triggers & Scheduling

Every trigger is just a prompt fed to `agentLoop()`. The firmware's job is to detect events and construct the prompt string. The LLM does all the reasoning.

### Two categories of triggers

**True interrupts** (hardware events, no polling):
- GPIO pin change (button, PIR, reed switch) → ISR sets a flag
- MQTT message arrives → callback queues the message
- HTTP request to ESP32's web server → handler queues the body
- Timer fires → millis() check in loop()
- Serial input → readline in loop()

**Polled sensors** (firmware must actively check):
- ADC readings (temperature, moisture, light, voltage)
- I2C/SPI sensors
- Any analog value

For polled sensors, the firmware handles the cheap **detection** (read value, compare to threshold). The LLM only handles the expensive **decision** (what to do about it). This saves API calls — no point invoking the LLM when the sensor reads normal.

```
┌──────────────────────────────────────────────────────────────┐
│  Firmware loop():                                            │
│                                                              │
│  1. Check scheduled tasks (millis-based)                     │
│     └─ if due → agentLoop(task.prompt)                       │
│                                                              │
│  2. Check GPIO interrupt flags                               │
│     └─ if set → agentLoop(trigger.prompt)                    │
│                                                              │
│  3. Check threshold monitors (poll ADC at intervals)         │
│     └─ if crossed → agentLoop(trigger.prompt with ${value})  │
│                                                              │
│  4. Check MQTT incoming queue                                │
│     └─ if message → agentLoop(prefix + payload)              │
│                                                              │
│  5. Check HTTP server request queue                          │
│     └─ if request → agentLoop(request.body)                  │
│                                                              │
│  6. Check Serial input                                       │
│     └─ if line → agentLoop(input)                            │
│                                                              │
│  Process one at a time (mutex / queue). No concurrent agents.│
└──────────────────────────────────────────────────────────────┘
```

## Sliding Context Window

Full history lives on SD. Only the last N messages load into RAM for each API call. System prompt is always pinned.

```
Session file on SD (unlimited):
  [system] [user1] [asst1] [tool1] [asst2] [user2] [asst3] ...

API request messages[] (last N from file):
  [system] ... [asst2] [user2] [asst3]
            ↑
         window_start = total_lines - context_window
         (system prompt always included as line 0)
```

### Session JSONL Format

Each line is one message, appended as the conversation progresses:

```jsonl
{"role":"system","content":"You are an agent running on an ESP32..."}
{"role":"user","content":"What's the voltage on pin 34?"}
{"role":"assistant","content":null,"tool_calls":[{"id":"call_abc","type":"function","function":{"name":"adc_read","arguments":"{\"pin\":34}"}}]}
{"role":"tool","tool_call_id":"call_abc","content":"2.31"}
{"role":"assistant","content":"Pin 34 reads 2.31V."}
```

## Memory Budget

| Component | RAM |
|---|---|
| WiFi + TLS | ~80KB |
| JSON request doc | ~8KB (20-message window + 8 tools) |
| JSON response (filtered) | ~2KB |
| HTTP buffers | ~2KB |
| SD file I/O | ~1KB |
| Trigger/schedule state | ~1KB |
| **Total** | **~94KB** of ~320KB usable |

Leaves ~226KB. SD absorbs all storage pressure.

## Implementation

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

// =============================================
// Config (loaded from SD)
// =============================================

struct Config {
  char wifi_ssid[64];
  char wifi_pass[64];
  char api_key[128];
  char model[64];
  int  max_tokens;
  int  max_iterations;
  int  context_window;
} cfg;

String systemPrompt;
JsonDocument toolsDef;

bool loadConfig() {
  File f = SD.open("/config/config.json", FILE_READ);
  if (!f) return false;
  JsonDocument doc; deserializeJson(doc, f); f.close();
  strlcpy(cfg.wifi_ssid, doc["wifi_ssid"] | "", sizeof(cfg.wifi_ssid));
  strlcpy(cfg.wifi_pass, doc["wifi_pass"] | "", sizeof(cfg.wifi_pass));
  strlcpy(cfg.api_key,   doc["api_key"]   | "", sizeof(cfg.api_key));
  strlcpy(cfg.model,     doc["model"] | "openai/gpt-5.2-codex",
          sizeof(cfg.model));
  cfg.max_tokens     = doc["max_tokens"]     | 512;
  cfg.max_iterations = doc["max_iterations"] | 10;
  cfg.context_window = doc["context_window"] | 20;
  return true;
}

bool loadSystemPrompt() {
  File f = SD.open("/config/system.txt", FILE_READ);
  if (!f) { systemPrompt = ""; return false; }
  systemPrompt = f.readString(); f.close();
  return true;
}

bool loadTools() {
  File f = SD.open("/config/tools.json", FILE_READ);
  if (!f) return false;
  deserializeJson(toolsDef, f); f.close();
  return true;
}

// =============================================
// Session Management
// =============================================

String currentSessionPath;

String escapeJsonString(const String& s) {
  String out = "\"";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out + "\"";
}

void appendMessage(const String& jsonLine) {
  File f = SD.open(currentSessionPath.c_str(), FILE_APPEND);
  if (f) { f.println(jsonLine); f.close(); }
}

String createSession(const char* firstMessage) {
  JsonDocument index;
  File idxFile = SD.open("/sessions/index.json", FILE_READ);
  if (idxFile) { deserializeJson(index, idxFile); idxFile.close(); }
  JsonArray sessions = index["sessions"].is<JsonArray>()
    ? index["sessions"].as<JsonArray>()
    : index["sessions"].to<JsonArray>();

  int nextId = (index["next_id"] | 1);
  index["next_id"] = nextId + 1;

  char filepath[64];
  snprintf(filepath, sizeof(filepath), "/sessions/sess_%04d.jsonl", nextId);

  JsonObject entry = sessions.add<JsonObject>();
  entry["id"] = nextId;
  entry["title"] = String(firstMessage).substring(0, 60);
  entry["created"] = millis();

  SD.mkdir("/sessions");
  idxFile = SD.open("/sessions/index.json", FILE_WRITE);
  serializeJson(index, idxFile); idxFile.close();

  currentSessionPath = filepath;

  if (systemPrompt.length() > 0) {
    appendMessage("{\"role\":\"system\",\"content\":" +
                  escapeJsonString(systemPrompt) + "}");
  }
  return filepath;
}

int countLines(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  int count = 0;
  while (f.available()) { if (f.read() == '\n') count++; }
  f.close();
  return count;
}

void loadWindowedMessages(JsonArray msgs) {
  int total = countLines(currentSessionPath.c_str());
  bool hasSystem = (systemPrompt.length() > 0);
  int skip = 0;
  if (hasSystem && total > cfg.context_window)
    skip = total - cfg.context_window + 1;

  File f = SD.open(currentSessionPath.c_str(), FILE_READ);
  if (!f) return;
  int lineNum = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() == 0) { lineNum++; continue; }
    if ((lineNum == 0 && hasSystem) || lineNum >= skip) {
      JsonDocument lineDoc; deserializeJson(lineDoc, line);
      msgs.add(lineDoc.as<JsonObject>());
    }
    lineNum++;
  }
  f.close();
}

void listSessions() {
  File f = SD.open("/sessions/index.json", FILE_READ);
  if (!f) { Serial.println("No sessions."); return; }
  JsonDocument doc; deserializeJson(doc, f); f.close();
  for (JsonObject s : doc["sessions"].as<JsonArray>()) {
    Serial.printf("#%d  %s\n", s["id"].as<int>(),
                  s["title"].as<const char*>());
  }
}

// =============================================
// Tool Dispatch (Primitives)
// =============================================

// (handlers defined in Primitives section above)

// =============================================
// HTTP
// =============================================

int postChat(const String& body, JsonDocument& resp) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://openrouter.ai/api/v1/chat/completions");
  http.setTimeout(30000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + cfg.api_key);
  int code = http.POST(body);
  if (code == 200) {
    JsonDocument filter;
    filter["choices"][0]["finish_reason"] = true;
    filter["choices"][0]["message"]["content"] = true;
    filter["choices"][0]["message"]["role"] = true;
    filter["choices"][0]["message"]["tool_calls"] = true;
    deserializeJson(resp, http.getStream(),
                    DeserializationOption::Filter(filter));
  }
  http.end();
  return code;
}

// =============================================
// Agent Loop
// =============================================

String agentLoop(const char* userPrompt,
                 const char* sessionPath = nullptr) {
  if (sessionPath) {
    currentSessionPath = sessionPath;
  } else {
    createSession(userPrompt);
  }

  appendMessage("{\"role\":\"user\",\"content\":" +
                escapeJsonString(String(userPrompt)) + "}");

  for (int i = 0; i < cfg.max_iterations; i++) {
    JsonDocument doc;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    loadWindowedMessages(msgs);

    doc["model"] = cfg.model;
    doc["max_tokens"] = cfg.max_tokens;
    doc["parallel_tool_calls"] = false;
    doc["tools"] = toolsDef.as<JsonArray>();

    String body; serializeJson(doc, body);
    JsonDocument resp;
    int code = postChat(body, resp);
    if (code != 200) return "HTTP " + String(code);

    const char* finish = resp["choices"][0]["finish_reason"];
    JsonObject msg = resp["choices"][0]["message"];

    // ---- STOP ----
    if (strcmp(finish, "stop") == 0) {
      String content = msg["content"].as<String>();
      appendMessage("{\"role\":\"assistant\",\"content\":" +
                    escapeJsonString(content) + "}");
      return content;
    }

    // ---- TOOL CALLS ----
    if (strcmp(finish, "tool_calls") == 0) {
      String asstLine; serializeJson(msg, asstLine);
      appendMessage(asstLine);

      for (JsonObject call : msg["tool_calls"].as<JsonArray>()) {
        const char* callId = call["id"];
        const char* fnName = call["function"]["name"];

        JsonDocument argsDoc;
        deserializeJson(argsDoc,
                        call["function"]["arguments"].as<const char*>());
        String result = execTool(fnName, argsDoc.as<JsonObject>());

        appendMessage("{\"role\":\"tool\",\"tool_call_id\":\"" +
                      String(callId) + "\",\"content\":" +
                      escapeJsonString(result) + "}");
      }
      continue;
    }
    return "unexpected: " + String(finish);
  }
  return "max iterations";
}

// =============================================
// Triggers & Scheduling
// =============================================

struct ScheduledTask {
  unsigned long every_ms;
  unsigned long last_run;
  String prompt;
};

struct ThresholdMonitor {
  int pin;
  unsigned long check_ms;
  unsigned long last_check;
  float threshold;
  bool above;          // true = trigger when above, false = when below
  String prompt;       // ${value} gets replaced with actual reading
};

static const int MAX_TASKS = 8;
static const int MAX_MONITORS = 8;
ScheduledTask tasks[MAX_TASKS];
ThresholdMonitor monitors[MAX_MONITORS];
int taskCount = 0, monitorCount = 0;

// GPIO interrupt flags
volatile bool gpioFlags[40] = {false};
String gpioPrompts[40];

void IRAM_ATTR gpioISR(void* arg) {
  int pin = (int)(intptr_t)arg;
  gpioFlags[pin] = true;
}

void loadSchedule() {
  File f = SD.open("/config/schedule.json", FILE_READ);
  if (!f) return;
  JsonDocument doc; deserializeJson(doc, f); f.close();
  for (JsonObject t : doc["tasks"].as<JsonArray>()) {
    if (taskCount >= MAX_TASKS) break;
    tasks[taskCount].every_ms = (t["every_sec"] | 3600) * 1000UL;
    tasks[taskCount].last_run = 0;
    tasks[taskCount].prompt = t["prompt"].as<String>();
    taskCount++;
  }
}

void loadTriggers() {
  File f = SD.open("/config/triggers.json", FILE_READ);
  if (!f) return;
  JsonDocument doc; deserializeJson(doc, f); f.close();
  for (JsonObject t : doc["triggers"].as<JsonArray>()) {
    const char* type = t["type"];

    if (strcmp(type, "gpio") == 0) {
      int pin = t["pin"];
      const char* edge = t["edge"] | "falling";
      gpioPrompts[pin] = t["prompt"].as<String>();
      pinMode(pin, INPUT_PULLUP);
      attachInterruptArg(digitalPinToInterrupt(pin), gpioISR,
                         (void*)(intptr_t)pin,
                         strcmp(edge, "rising") == 0 ? RISING : FALLING);
    }

    if (strcmp(type, "threshold") == 0) {
      if (monitorCount >= MAX_MONITORS) continue;
      monitors[monitorCount].pin = t["pin"];
      monitors[monitorCount].check_ms = (t["check_sec"] | 30) * 1000UL;
      monitors[monitorCount].last_check = 0;
      monitors[monitorCount].threshold = t["above"] | t["below"] | 2.0f;
      monitors[monitorCount].above = t.containsKey("above");
      monitors[monitorCount].prompt = t["prompt"].as<String>();
      monitorCount++;
    }
  }
}

// =============================================
// Main Loop (Event Dispatcher)
// =============================================

void setup() {
  Serial.begin(115200);

  if (!SD.begin()) {
    Serial.println("SD init failed");
    return;
  }

  loadConfig();
  loadSystemPrompt();
  loadTools();
  loadSchedule();
  loadTriggers();

  // Ensure agent workspace exists
  SD.mkdir("/agent");
  SD.mkdir("/agent/procedures");
  SD.mkdir("/agent/data");

  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println("Ready. IP: " + WiFi.localIP().toString());
}

void loop() {
  unsigned long now = millis();

  // 1. Scheduled tasks
  for (int i = 0; i < taskCount; i++) {
    if (now - tasks[i].last_run >= tasks[i].every_ms) {
      tasks[i].last_run = now;
      Serial.println("[cron] " + tasks[i].prompt.substring(0, 60));
      String answer = agentLoop(tasks[i].prompt.c_str());
      Serial.println(answer);
    }
  }

  // 2. GPIO interrupt flags
  for (int pin = 0; pin < 40; pin++) {
    if (gpioFlags[pin]) {
      gpioFlags[pin] = false;
      if (gpioPrompts[pin].length() > 0) {
        Serial.printf("[gpio] pin %d triggered\n", pin);
        String answer = agentLoop(gpioPrompts[pin].c_str());
        Serial.println(answer);
      }
    }
  }

  // 3. Threshold monitors
  for (int i = 0; i < monitorCount; i++) {
    if (now - monitors[i].last_check >= monitors[i].check_ms) {
      monitors[i].last_check = now;
      float voltage = analogRead(monitors[i].pin) * 3.3 / 4095;
      bool triggered = monitors[i].above
        ? (voltage > monitors[i].threshold)
        : (voltage < monitors[i].threshold);
      if (triggered) {
        String prompt = monitors[i].prompt;
        prompt.replace("${value}", String(voltage, 2));
        Serial.println("[threshold] " + prompt.substring(0, 60));
        String answer = agentLoop(prompt.c_str());
        Serial.println(answer);
      }
    }
  }

  // 4. Serial input
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n'); input.trim();
    if (input.length() > 0) {
      if (input == "/sessions") {
        listSessions();
      } else {
        String answer = agentLoop(input.c_str());
        Serial.println(answer);
      }
    }
  }

  delay(10); // yield
}
```

---

## Design Decisions

| Decision | Why |
|---|---|
| **Primitives, not specialized tools** | 8 tools fit every request. LLM composes behaviors. No discovery stage needed. No reflash to "add" a tool |
| **Firmware = plumbing** | Agent loop, session persistence, trigger dispatch. Flash once, never changes |
| **LLM = runtime** | All reasoning, sequencing, decision-making. System prompt on SD defines the agent's personality and capabilities |
| **Triggers → prompts** | Every event (cron, GPIO, threshold, MQTT, serial) becomes a natural-language prompt. One code path for everything |
| **Detection in firmware, decisions in LLM** | Firmware polls sensors cheaply. Only invokes the LLM when a threshold is crossed. Saves API calls |
| **JSONL sessions** | Append-only writes. Survives power loss. One JSON per line |
| **Sliding context window** | Unlimited history on disk, bounded RAM. System prompt always pinned |
| **Agent workspace (`/agent/`)** | LLM can read/write its own notes, procedures, data. Self-improving memory without MicroPython |
| **`parallel_tool_calls: false`** | Handle one tool at a time. Simpler, less RAM |
| **ArduinoJson filter** | Only parse `choices[0].message` + `finish_reason`. Ignore everything else |
| **No streaming** | One JSON blob is simpler than SSE. Agent loop doesn't need real-time output |
| **No MicroPython** | Can't afford ~60-80KB interpreter overhead on base ESP32. Primitives + LLM reasoning achieves the same flexibility |

## Payload Sizes

| Scenario | ~Bytes |
|---|---|
| Request: 8 tools, 1 message | ~2KB |
| Request: 8 tools, 10 messages | ~4KB |
| Request: 8 tools, 20 messages | ~7KB |
| Response: tool call | 300-500 |
| Response: text answer | 200-800 |
