// ESP32 Tool-Calling Agent — Lite Tier
// Agent loop: POST messages+tools → parse → dispatch tool → loop → done
// Tools: pin_read, pin_write, adc_read, get_status, http_request, set_display_color
// Display: ST7789V 1.14" TFT on TTGO T-Display (firmware-controlled)
// Buttons: GPIO0 (left=status), GPIO35 (right=haiku)

#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "credentials.h"

// ---- Config ----
const char* MODEL          = "openai/gpt-5.2-codex";
const char* API_URL        = "https://openrouter.ai/api/v1/chat/completions";
const int   MAX_TOKENS     = 300;
const int   MAX_ITERATIONS = 8;

// ---- Buttons ----
#define BTN_LEFT   0   // GPIO0, active LOW
#define BTN_RIGHT  35  // GPIO35, active LOW, input-only

const char* PROMPT_LEFT =
  "Check your system status using get_status and report what you find.";

const char* PROMPT_RIGHT =
  "Fetch the text from http://www.peregianhub.com.au/esp.txt using http_request. "
  "Read the subject. Use set_display_color to choose a background color that complements the subject. "
  "Then write a haiku about that subject. Respond with ONLY the haiku, nothing else.";

// ---- Display state colors (RGB565) ----
#define COL_IDLE     TFT_BLACK
#define COL_THINKING 0x0012   // dark blue
#define COL_TOOL     0x0320   // dark green
#define COL_ERROR    0x6000   // dark red

// ---- Named color map for set_display_color ----
struct NamedColor { const char* name; uint16_t color; };
const NamedColor COLOR_MAP[] = {
  {"black",   0x0000}, {"white",   0xFFFF}, {"red",     0xF800},
  {"green",   0x07E0}, {"blue",    0x001F}, {"yellow",  0xFFE0},
  {"cyan",    0x07FF}, {"magenta", 0xF81F}, {"orange",  0xFD20},
  {"purple",  0x780F}, {"pink",    0xFC18}, {"teal",    0x0410},
  {"gold",    0xFEA0}, {"navy",    0x0010}, {"maroon",  0x7800},
  {"olive",   0x7BE0}, {"coral",   0xFBEA}, {"salmon",  0xFC0E},
  {"sky",     0x867D}, {"forest",  0x2444}, {"crimson", 0xC904},
  {"indigo",  0x4810}, {"violet",  0x9199}, {"lime",    0x87E0},
  {nullptr,   0x0000}
};

// ---- System prompt ----
const char* SYSTEM_PROMPT =
  "You are an autonomous agent running on an ESP32 microcontroller with a 1.14\" color TFT display. "
  "You have tools to interact with hardware, network, and the display background color. "
  "Your final text response is always shown on the display automatically. "
  "Use set_display_color to set the background color before your final response. "
  "Keep your final response short (under 100 chars) so it fits on the small screen. "
  "Be concise. Call tools one at a time.";

// ---- Globals ----
TFT_eSPI tft = TFT_eSPI();
volatile bool wifiConnected = false;
volatile bool wifiNeedsReconnect = false;
volatile int  disconnectCount = 0;
volatile int  lastDisconnectReason = 0;
unsigned long lastReconnectAttempt = 0;
int reconnectFailures = 0;
const int MAX_RECONNECT_FAILURES = 5;
bool wifiEventRegistered = false;
uint64_t configuredInputs = 0, configuredOutputs = 0;
bool agentRunning = false;
uint16_t resultBgColor = COL_IDLE;  // background for final answer display

// =============================================
// Display helpers
// =============================================

void showScreen(uint16_t bg, const char* text) {
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextSize(1);
  tft.setTextFont(2);
  tft.setTextWrap(true);
  tft.setCursor(4, 4);
  tft.print(text);
}

void showStatus() {
  tft.fillScreen(COL_IDLE);
  tft.setTextColor(TFT_WHITE, COL_IDLE);
  tft.setTextSize(1);
  tft.setTextFont(2);
  tft.setTextWrap(true);
  tft.setCursor(4, 4);
  tft.printf("Heap: %d\n", ESP.getFreeHeap());
  tft.printf("Up: %lus\n", millis() / 1000);
  tft.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  tft.printf("\nL:Status  R:Haiku");
}

// =============================================
// WiFi (AU ch13, PMF, band steering retries)
// =============================================

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected = false;
      wifiNeedsReconnect = true;
      disconnectCount++;
      lastDisconnectReason = info.wifi_sta_disconnected.reason;
      Serial.printf("[wifi] Disconnect #%d reason=%d\n",
                    disconnectCount, lastDisconnectReason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      wifiNeedsReconnect = false;
      Serial.printf("[wifi] Connected! IP: %s\n",
                    WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

void applyWiFiConfig() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // disable modem sleep — improves reliability
  WiFi.setAutoReconnect(true);   // SDK-level auto-reconnect

  if (!wifiEventRegistered) {
    WiFi.onEvent(WiFiEvent);
    wifiEventRegistered = true;
  }

  wifi_country_t country = {
    .cc = "AU", .schan = 1, .nchan = 13,
    .policy = WIFI_COUNTRY_POLICY_MANUAL
  };
  esp_wifi_set_country(&country);

  wifi_config_t cfg = {};
  strncpy((char*)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
  strncpy((char*)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

bool waitForWiFi(uint32_t timeoutMs) {
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  return wifiConnected;
}

bool hardResetWiFiStack() {
  Serial.println("[wifi] Performing full WiFi stack reset...");
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(300);

  wifiConnected = false;
  wifiNeedsReconnect = true;
  applyWiFiConfig();

  esp_wifi_connect();
  bool ok = waitForWiFi(30000);
  if (ok) {
    reconnectFailures = 0;
    wifiNeedsReconnect = false;
    Serial.println("[wifi] Hard reset reconnect OK");
  } else {
    Serial.println("[wifi] Hard reset reconnect FAILED");
  }
  return ok;
}

bool attemptWiFiReconnect(uint32_t waitMs) {
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN && err != ESP_ERR_WIFI_STATE) {
    Serial.printf("[wifi] esp_wifi_connect error: %s (0x%x)\n", esp_err_to_name(err), err);
  }

  if (waitForWiFi(waitMs)) {
    reconnectFailures = 0;
    wifiNeedsReconnect = false;
    return true;
  }

  reconnectFailures++;
  Serial.printf("[wifi] Reconnect failed (%d/%d)\n",
                reconnectFailures, MAX_RECONNECT_FAILURES);
  if (reconnectFailures >= MAX_RECONNECT_FAILURES) {
    reconnectFailures = 0;
    return hardResetWiFiStack();
  }
  return false;
}

bool connectWiFi() {
  applyWiFiConfig();

  Serial.printf("[wifi] Connecting to %s...\n", WIFI_SSID);
  showScreen(COL_IDLE, "Connecting WiFi...");
  return attemptWiFiReconnect(30000);
}

// =============================================
// Tool Handlers
// =============================================

String escapeJsonString(const String& s) {
  String out = "\"";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') continue;
    else out += c;
  }
  return out + "\"";
}

String readHttpBodyLimited(HTTPClient& http, size_t maxBytes) {
  WiFiClient* stream = http.getStreamPtr();
  String body;
  body.reserve(maxBytes);

  unsigned long start = millis();
  unsigned long lastData = millis();

  while ((millis() - start) < 12000) {
    while (stream->available()) {
      int c = stream->read();
      if (c < 0) break;
      if (body.length() < maxBytes) body += (char)c;
      lastData = millis();
    }

    if (!http.connected() && !stream->available()) break;
    if ((millis() - lastData) > 2000) break;
    delay(1);
  }
  return body;
}

String handleHttpRequest(JsonObject args) {
  const char* url = args["url"];
  const char* method = args["method"] | "GET";
  bool isHttps = (strncmp(url, "https", 5) == 0);

  HTTPClient http;
  WiFiClient* client = nullptr;
  WiFiClientSecure* secClient = nullptr;

  if (isHttps) {
    secClient = new WiFiClientSecure();
    if (!secClient) return "{\"status\":-1,\"body\":\"alloc failed\"}";
    secClient->setInsecure();
    client = secClient;
  } else {
    client = new WiFiClient();
    if (!client) return "{\"status\":-1,\"body\":\"alloc failed\"}";
  }

  int code = -1;
  String body = "begin failed";
  bool beginOk = false;

  {
    HTTPClient http;
    if (http.begin(*client, url)) {
      beginOk = true;
      http.setConnectTimeout(10000);
      http.setTimeout(10000);
      http.setReuse(false);   // Force connection close per request
      http.useHTTP10(true);   // Simpler close semantics on constrained stacks
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Connection", "close");

      if (args["headers"].is<JsonObject>()) {
        for (JsonPair h : args["headers"].as<JsonObject>()) {
          http.addHeader(h.key().c_str(), h.value().as<const char*>());
        }
      }

      if (strcmp(method, "POST") == 0)
        code = http.POST(args["body"] | "");
      else if (strcmp(method, "PUT") == 0)
        code = http.PUT(args["body"] | "");
      else if (strcmp(method, "DELETE") == 0)
        code = http.sendRequest("DELETE");
      else
        code = http.GET();

      body = (code > 0) ? readHttpBodyLimited(http, 1024)
                        : HTTPClient::errorToString(code);

      // Clean up network state before object destruction
      http.end();
    }
  }

  client->stop();
  delay(20);
  if (secClient) delete secClient;
  else delete client;
  delay(20);

  if (!beginOk) return "{\"status\":-1,\"body\":\"begin failed\"}";
  return "{\"status\":" + String(code) + ",\"body\":" + escapeJsonString(body) + "}";
}

String handleSetDisplayColor(JsonObject args) {
  const char* colorName = args["color"] | "black";
  for (int i = 0; COLOR_MAP[i].name; i++) {
    if (strcasecmp(colorName, COLOR_MAP[i].name) == 0) {
      resultBgColor = COLOR_MAP[i].color;
      Serial.printf("[display] Color set to %s (0x%04X)\n", colorName, resultBgColor);
      return String("color set to ") + colorName;
    }
  }
  // Unknown color — default to black
  resultBgColor = COL_IDLE;
  return String("unknown color '") + colorName + "', using black";
}

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
    int pin = args["pin"];
    int val = args["value"];
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
           ",\"uptime\":" + String(millis() / 1000) +
           ",\"wifi\":\"" + WiFi.localIP().toString() + "\"}";
  }
  if (strcmp(name, "http_request") == 0) {
    return handleHttpRequest(args);
  }
  if (strcmp(name, "set_display_color") == 0) {
    return handleSetDisplayColor(args);
  }
  return "unknown tool: " + String(name);
}

// =============================================
// Tool Definitions (sent in every API request)
// =============================================

void buildTools(JsonArray tools) {
  auto addTool = [&](const char* name, const char* desc,
                     const char* params) {
    JsonObject t = tools.add<JsonObject>();
    t["type"] = "function";
    t["function"]["name"] = name;
    t["function"]["description"] = desc;
    JsonDocument p;
    deserializeJson(p, params);
    t["function"]["parameters"] = p.as<JsonObject>();
  };

  addTool("pin_read", "Read digital GPIO pin (returns 0 or 1)",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}");
  addTool("pin_write", "Set GPIO pin HIGH (1) or LOW (0)",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\",\"enum\":[0,1]}},\"required\":[\"pin\",\"value\"]}");
  addTool("adc_read", "Read analog voltage (0-3.3V) from ADC pin",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},\"required\":[\"pin\"]}");
  addTool("get_status", "Get system status (heap, uptime, wifi IP)",
    "{\"type\":\"object\",\"properties\":{}}");
  addTool("http_request", "Make an HTTP request. Returns status code and body (max 1KB).",
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Full URL\"},\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\",\"PUT\",\"DELETE\"],\"description\":\"HTTP method (default GET)\"},\"body\":{\"type\":\"string\",\"description\":\"Request body for POST/PUT\"},\"headers\":{\"type\":\"object\",\"description\":\"Additional headers\"}},\"required\":[\"url\"]}");
  addTool("set_display_color", "Set the background color of the TFT display for showing the final response. Available colors: black, white, red, green, blue, yellow, cyan, magenta, orange, purple, pink, teal, gold, navy, maroon, olive, coral, salmon, sky, forest, crimson, indigo, violet, lime.",
    "{\"type\":\"object\",\"properties\":{\"color\":{\"type\":\"string\",\"description\":\"Color name\"}},\"required\":[\"color\"]}");
}

// =============================================
// HTTP POST to OpenRouter
// =============================================

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[wifi] Not connected, triggering reconnect...");
  showScreen(COL_ERROR, "WiFi reconnecting...");
  return attemptWiFiReconnect(15000);
}

int postChat(const String& body, JsonDocument& resp) {
  if (!ensureWiFi()) return -1;

  Serial.printf("[api] heap before POST: %d\n", ESP.getFreeHeap());
  WiFiClientSecure* client = new WiFiClientSecure();
  if (!client) {
    Serial.println("[api] alloc failed");
    return -1;
  }
  client->setInsecure();
  client->setTimeout(30);

  int code = -1;
  bool beginOk = false;

  {
    HTTPClient http;
    if (http.begin(*client, API_URL)) {
      beginOk = true;
      http.setConnectTimeout(15000);
      http.setTimeout(30000);
      http.setReuse(false);   // No keep-alive reuse between turns
      http.useHTTP10(true);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + API_KEY);
      http.addHeader("Connection", "close");

      Serial.printf("[api] POST %d bytes...\n", body.length());
      code = http.POST(body);
      Serial.printf("[api] Response: %d\n", code);

      if (code == 200) {
        String raw = http.getString();
        DeserializationError err = deserializeJson(resp, raw);
        if (err) Serial.printf("[api] JSON parse error: %s\n", err.c_str());
      } else if (code > 0) {
        String errBody = http.getString();
        Serial.printf("[api] Error body: %s\n",
                      errBody.substring(0, 200).c_str());
      }

      http.end();
    } else {
      Serial.println("[api] http.begin failed");
    }
  }

  client->stop();
  delete client;
  delay(20);

  if (!beginOk) {
    wifiNeedsReconnect = true;
    return -1;
  }

  if (code <= 0) {
    wifiNeedsReconnect = true;
  }

  Serial.printf("[api] heap after: %d\n", ESP.getFreeHeap());
  return code;
}

// =============================================
// Agent Loop
// =============================================

String agentLoop(const char* userPrompt) {
  Serial.printf("[agent] Prompt: %s\n", userPrompt);
  Serial.printf("[agent] Heap before: %d\n", ESP.getFreeHeap());

  // Reset result color to black for each run
  resultBgColor = COL_IDLE;
  showScreen(COL_THINKING, "Thinking...");

  JsonDocument doc;
  JsonArray msgs = doc["messages"].to<JsonArray>();

  JsonObject sysMsg = msgs.add<JsonObject>();
  sysMsg["role"] = "system";
  sysMsg["content"] = SYSTEM_PROMPT;

  JsonObject userMsg = msgs.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = userPrompt;

  doc["model"] = MODEL;
  doc["max_tokens"] = MAX_TOKENS;
  doc["parallel_tool_calls"] = false;
  buildTools(doc["tools"].to<JsonArray>());

  for (int i = 0; i < MAX_ITERATIONS; i++) {
    Serial.printf("[agent] Iteration %d/%d\n", i + 1, MAX_ITERATIONS);

    showScreen(COL_THINKING, "Thinking...");

    String body;
    serializeJson(doc, body);

    JsonDocument resp;
    int code = postChat(body, resp);
    if (code != 200) {
      // Retry once after a pause
      Serial.printf("[agent] postChat failed (%d), retrying in 2s...\n", code);
      showScreen(COL_ERROR, ("Retry... (" + String(code) + ")").c_str());
      delay(2000);
      code = postChat(body, resp);
    }
    if (code != 200) {
      showScreen(COL_ERROR, ("Error: HTTP " + String(code)).c_str());
      return "[error] HTTP " + String(code);
    }

    const char* finish = resp["choices"][0]["finish_reason"];
    if (!finish) {
      showScreen(COL_ERROR, "Error: no finish_reason");
      return "[error] No finish_reason in response";
    }

    JsonObject msg = resp["choices"][0]["message"];

    // ---- STOP or LENGTH: final answer ----
    if (strcmp(finish, "stop") == 0 || strcmp(finish, "length") == 0) {
      String content = msg["content"].as<String>();
      Serial.printf("[agent] Done (%s). Heap after: %d\n", finish, ESP.getFreeHeap());
      // Always show final answer on the result background color
      showScreen(resultBgColor, content.c_str());
      return content;
    }

    // ---- TOOL CALLS: execute and continue ----
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

        const char* fnName = call["function"]["name"];
        const char* fnArgs = call["function"]["arguments"];
        Serial.printf("[tool] %s(%s) heap=%d\n", fnName, fnArgs, ESP.getFreeHeap());

        // Show tool name on green background
        showScreen(COL_TOOL, fnName);

        JsonDocument argsDoc;
        deserializeJson(argsDoc, fnArgs);
        String result = execTool(fnName, argsDoc.as<JsonObject>());
        Serial.printf("[tool] -> %s\n", result.c_str());

        JsonObject toolMsg = msgs.add<JsonObject>();
        toolMsg["role"] = "tool";
        toolMsg["tool_call_id"] = call["id"];
        toolMsg["content"] = result;
      }
      continue;
    }

    showScreen(COL_ERROR, ("Error: " + String(finish)).c_str());
    return "[error] Unexpected finish_reason: " + String(finish);
  }
  showScreen(COL_ERROR, "Error: max iterations");
  return "[error] Max iterations reached";
}

// =============================================
// Setup & Loop
// =============================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Init display
  tft.init();
  tft.setRotation(1);  // landscape: 240x135
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  tft.println("ESP32 Agent");
  tft.println("Booting...");

  // Init buttons
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT);  // GPIO35 is input-only, has external pullup

  Serial.println("\n=============================");
  Serial.println("  ESP32 Agent — Lite Tier");
  Serial.println("=============================");
  Serial.printf("Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Model: %s\n\n", MODEL);

  if (!connectWiFi()) {
    Serial.println("[wifi] FAILED. Restarting in 10s...");
    showScreen(COL_ERROR, "WiFi FAILED\nRestarting...");
    delay(10000);
    ESP.restart();
  }

  Serial.printf("Heap after WiFi: %d bytes\n", ESP.getFreeHeap());
  Serial.println("\nReady. Type a prompt or press a button.");

  showStatus();
}

void loop() {
  // WiFi reconnect — with 5s cooldown between attempts
  bool wifiLost = (WiFi.status() != WL_CONNECTED) || !wifiConnected || wifiNeedsReconnect;
  if (wifiLost && !agentRunning) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      Serial.printf("[wifi] Reconnecting (reason=%d)...\n", lastDisconnectReason);
      showScreen(COL_ERROR, "WiFi reconnecting...");
      if (attemptWiFiReconnect(15000)) {
        Serial.println("[wifi] Reconnected OK");
        showStatus();
      }
    }
  }

  // Button input (ignore while agent is running)
  if (!agentRunning) {
    if (digitalRead(BTN_LEFT) == LOW) {
      delay(50);
      if (digitalRead(BTN_LEFT) == LOW) {
        agentRunning = true;
        Serial.println("[btn] Left pressed — Status");
        String answer = agentLoop(PROMPT_LEFT);
        Serial.println("---");
        Serial.println(answer);
        Serial.println("---");
        Serial.printf("Heap: %d | Uptime: %lus\n", ESP.getFreeHeap(), millis() / 1000);
        agentRunning = false;
        while (digitalRead(BTN_LEFT) == LOW) delay(10);
      }
    }

    if (digitalRead(BTN_RIGHT) == LOW) {
      delay(50);
      if (digitalRead(BTN_RIGHT) == LOW) {
        agentRunning = true;
        Serial.println("[btn] Right pressed — Haiku");
        String answer = agentLoop(PROMPT_RIGHT);
        Serial.println("---");
        Serial.println(answer);
        Serial.println("---");
        Serial.printf("Heap: %d | Uptime: %lus\n", ESP.getFreeHeap(), millis() / 1000);
        agentRunning = false;
        while (digitalRead(BTN_RIGHT) == LOW) delay(10);
      }
    }
  }

  // Serial input → agent loop
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      agentRunning = true;
      Serial.println("---");
      String answer = agentLoop(input.c_str());
      Serial.println("---");
      Serial.println(answer);
      Serial.println("---");
      Serial.printf("Heap: %d | Uptime: %lus\n",
                    ESP.getFreeHeap(), millis() / 1000);
      agentRunning = false;
    }
  }
}
