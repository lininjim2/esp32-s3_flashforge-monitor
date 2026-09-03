#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// --- Network Setup ---
const bool USE_STATIC_IP = false;

IPAddress local_IP(0, 0, 0, 0);
IPAddress gateway(0, 0, 0, 0);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

const uint16_t printer_tcp_port = 8899;

// --- Multi-Printer & IFS Storage ---
struct PrinterEntry {
  String name;
  String ip;
};

struct IfsSlot {
  String material;
  String color;
};

std::vector<PrinterEntry> printerList;
int activePrinterIndex = 0;
IfsSlot ifsSlots[4] = {
  {"PLA", "#ffffff"},
  {"PETG", "#2563eb"},
  {"PLA", "#16a34a"},
  {"PETG", "#dc2626"}
};

SemaphoreHandle_t printerMutex;
SemaphoreHandle_t tcpMutex;

WebServer server(80);

// --- Telemetry & State Cache ---
struct PrinterData {
  String status = "READY";
  String filename = "None";
  int progress = 0;
  int current_layer = 0;
  int total_layers = 0;
  float nozzle_temp = 0.0;
  float nozzle_target = 0.0;
  float bed_temp = 0.0;
  float bed_target = 0.0;
  float ifs_temp = 0.0;
  bool light_on = false;
  bool part_fan = false;
  bool chamber_fan = false;
  unsigned long last_update = 0;
} printer;

// --- NVS Flash Storage ---
void savePrintersToNVS() {
  Preferences p;
  if (p.begin("ad5x_cfg", false)) {
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto &item : printerList) {
      JsonObject obj = arr.createNestedObject();
      obj["name"] = item.name;
      obj["ip"] = item.ip;
    }
    String json;
    serializeJson(doc, json);
    p.putString("printers", json);
    p.putInt("active_idx", activePrinterIndex);
    p.end();
  }
}

void saveIfsToNVS() {
  Preferences p;
  if (p.begin("ad5x_ifs", false)) {
    for (int i = 0; i < 4; i++) {
      p.putString(String("m" + String(i)).c_str(), ifsSlots[i].material);
      p.putString(String("c" + String(i)).c_str(), ifsSlots[i].color);
    }
    p.end();
  }
}

void loadIfsFromNVS() {
  Preferences p;
  if (p.begin("ad5x_ifs", true)) {
    for (int i = 0; i < 4; i++) {
      String m = p.getString(String("m" + String(i)).c_str(), "");
      String c = p.getString(String("c" + String(i)).c_str(), "");
      if (m.length() > 0) ifsSlots[i].material = m;
      if (c.length() > 0) ifsSlots[i].color = c;
    }
    p.end();
  }
}

void loadPrintersFromNVS() {
  Preferences p;
  printerList.clear();
  if (p.begin("ad5x_cfg", true)) {
    String json = p.getString("printers", "");
    activePrinterIndex = p.getInt("active_idx", 0);
    p.end();

    if (json.length() > 0) {
      DynamicJsonDocument doc(2048);
      if (deserializeJson(doc, json) == DeserializationError::Ok && doc.is<JsonArray>()) {
        for (JsonObject obj : doc.as<JsonArray>()) {
          printerList.push_back({obj["name"].as<String>(), obj["ip"].as<String>()});
        }
      }
    }
  }

  if (printerList.empty()) {
    printerList.push_back({"AD5X Jim", "192.168.1.124"});
    activePrinterIndex = 0;
    savePrintersToNVS();
  } else {
    if (activePrinterIndex < 0 || activePrinterIndex >= (int)printerList.size()) {
      activePrinterIndex = 0;
    }
  }
}

String getActiveIp() {
  String ip = "";
  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(1000))) {
    if (!printerList.empty()) {
      if (activePrinterIndex < 0 || activePrinterIndex >= (int)printerList.size()) {
        activePrinterIndex = 0;
      }
      ip = printerList[activePrinterIndex].ip;
    }
    xSemaphoreGive(printerMutex);
  }
  return ip;
}

// --- TCP Helpers ---
String sendAndReceiveTcp(WiFiClient &client, const char* cmd, unsigned long timeoutMs = 1200) {
  while (client.available()) client.read();
  client.print(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = client.read();
      resp += c;
    }
    if (resp.indexOf("ok") != -1 || resp.indexOf("\n") != -1) break;
    delay(10);
  }
  return resp;
}

bool sendPrinterTcpCommand(const char* cmd) {
  String targetIp = getActiveIp();
  if (targetIp.length() == 0) return false;

  if (!xSemaphoreTake(tcpMutex, pdMS_TO_TICKS(2500))) return false;

  WiFiClient client;
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 2000)) {
    xSemaphoreGive(tcpMutex);
    return false;
  }

  sendAndReceiveTcp(client, "~M601 S1\r\n");
  sendAndReceiveTcp(client, cmd);

  client.stop();
  xSemaphoreGive(tcpMutex);
  return true;
}

bool sendPrinterTcpCommands(const std::vector<String>& commands, int delayBetweenMs = 120) {
  bool allOk = true;
  for (const String& cmd : commands) {
    if (!sendPrinterTcpCommand(cmd.c_str())) allOk = false;
    if (delayBetweenMs > 0) delay(delayBetweenMs);
  }
  return allOk;
}

// --- Background Telemetry Poller (Core 0) ---
void updatePrinterTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;

  String targetIp = getActiveIp();
  if (targetIp.length() == 0) return;

  if (!xSemaphoreTake(tcpMutex, pdMS_TO_TICKS(1500))) return;

  WiFiClient client;
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 1800)) {
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(1000))) {
      printer.status = "OFFLINE";
      xSemaphoreGive(printerMutex);
    }
    xSemaphoreGive(tcpMutex);
    return;
  }

  // 1. Handshake
  sendAndReceiveTcp(client, "~M601 S1\r\n");

  // 2. Machine Status & Hardware LED State (M119)
  String statusResp = sendAndReceiveTcp(client, "~M119\r\n");
  String parsedStatus = "ONLINE";
  bool hwLed = false;

  if (statusResp.indexOf("MachineStatus: READY") != -1 || statusResp.indexOf("MachineStatus: IDLE") != -1) {
    parsedStatus = "READY";
  } else if (statusResp.indexOf("BUILDING") != -1 || statusResp.indexOf("PRINTING") != -1) {
    parsedStatus = "PRINTING";
  } else if (statusResp.indexOf("PAUSED") != -1) {
    parsedStatus = "PAUSED";
  } else if (statusResp.indexOf("COMPLETE") != -1) {
    parsedStatus = "COMPLETED";
  }

  int ledPos = statusResp.indexOf("LED:");
  if (ledPos != -1) {
    int valPos = ledPos + 4;
    while (valPos < statusResp.length() && (statusResp.charAt(valPos) == ' ' || statusResp.charAt(valPos) == '\t')) valPos++;
    if (valPos < statusResp.length()) hwLed = (statusResp.charAt(valPos) == '1');
  }

  // 3. Temperatures & Targets (M105)
  String tempResp = sendAndReceiveTcp(client, "~M105\r\n");
  float nozzle = 0.0, nozzle_tgt = 0.0;
  float bed = 0.0, bed_tgt = 0.0;
  float t1_val = 0.0;

  int tIndex = tempResp.indexOf("T0:");
  if (tIndex != -1) {
    int slash = tempResp.indexOf("/", tIndex);
    int space = tempResp.indexOf(" ", tIndex + 3);
    if (slash != -1 && (space == -1 || slash < space)) {
      nozzle = tempResp.substring(tIndex + 3, slash).toFloat();
      int nextSpace = tempResp.indexOf(" ", slash);
      if (nextSpace != -1) nozzle_tgt = tempResp.substring(slash + 1, nextSpace).toFloat();
      else nozzle_tgt = tempResp.substring(slash + 1).toFloat();
    } else if (space != -1) {
      nozzle = tempResp.substring(tIndex + 3, space).toFloat();
    }
  }

  int t1Index = tempResp.indexOf("T1:");
  if (t1Index != -1) {
    int slash = tempResp.indexOf("/", t1Index);
    int space = tempResp.indexOf(" ", t1Index + 3);
    if (slash != -1 && (space == -1 || slash < space)) {
      t1_val = tempResp.substring(t1Index + 3, slash).toFloat();
    } else if (space != -1) {
      t1_val = tempResp.substring(t1Index + 3, space).toFloat();
    }
  }

  int bIndex = tempResp.indexOf("B:");
  if (bIndex != -1) {
    int slash = tempResp.indexOf("/", bIndex);
    int space = tempResp.indexOf(" ", bIndex + 2);
    if (slash != -1 && (space == -1 || slash < space)) {
      bed = tempResp.substring(bIndex + 2, slash).toFloat();
      int nextSpace = tempResp.indexOf(" ", slash);
      if (nextSpace != -1) bed_tgt = tempResp.substring(slash + 1, nextSpace).toFloat();
      else bed_tgt = tempResp.substring(slash + 1).toFloat();
    } else if (space != -1) {
      bed = tempResp.substring(bIndex + 2, space).toFloat();
    }
  }

  // 4. Progress (M27)
  String progResp = sendAndReceiveTcp(client, "~M27\r\n");
  int progress = 0;
  int byteIdx = progResp.indexOf("byte ");
  int slashIdx = progResp.indexOf("/", byteIdx);
  if (byteIdx != -1 && slashIdx != -1) {
    long curBytes = progResp.substring(byteIdx + 5, slashIdx).toInt();
    long totBytes = progResp.substring(slashIdx + 1).toInt();
    if (totBytes > 0) progress = (int)((curBytes * 100) / totBytes);
  }

  client.stop();
  xSemaphoreGive(tcpMutex);

  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(1000))) {
    printer.status = parsedStatus;
    printer.nozzle_temp = nozzle;
    printer.nozzle_target = nozzle_tgt;
    printer.bed_temp = bed;
    printer.bed_target = bed_tgt;
    printer.ifs_temp = (t1_val > 0.0) ? t1_val : bed;
    printer.progress = progress;
    printer.light_on = hwLed;
    printer.last_update = millis();
    xSemaphoreGive(printerMutex);
  }
}

void telemetryTask(void *pvParameters) {
  for (;;) {
    updatePrinterTelemetry();
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// --- Dashboard UI Template (PROGMEM) ---
static const char INDEX_HTML[] PROGMEM = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>FlashForge AD5X</title><style>"
":root{--bg-page:#070b14;--bg-card:#121826;--bg-sub:#1a2235;--btn-bg:#1e293b;--btn-hover:#334155;--accent-blue:#0ea5e9;--accent-green:#22c55e;--accent-amber:#f59e0b;--accent-red:#ef4444;--text-main:#f8fafc;--text-muted:#64748b;--border:#1e293b;}"
"body{background-color:var(--bg-page);color:var(--text-main);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;justify-content:center;margin:0;padding:12px;box-sizing:border-box;}"
".app-container{width:100%;max-width:440px;display:flex;flex-direction:column;gap:12px;}"
".top-bar{background:var(--bg-card);border:1px solid var(--border);border-radius:28px;padding:8px 14px;display:flex;align-items:center;justify-content:space-between;}"
".top-title{display:flex;align-items:center;gap:10px;font-weight:700;font-size:0.95rem;overflow:hidden;}"
".select-printer{background:transparent;color:#fff;border:none;font-weight:700;font-size:0.95rem;cursor:pointer;outline:none;}"
".cam-card{position:relative;width:100%;height:250px;background:#000;border-radius:18px;overflow:hidden;border:1px solid var(--border);box-shadow:0 8px 24px rgba(0,0,0,0.6);}"
".cam-card img{width:100%;height:100%;object-fit:cover;}"
".cam-hud-top{position:absolute;top:10px;right:10px;display:flex;gap:6px;background:rgba(18,24,38,0.75);backdrop-filter:blur(8px);border-radius:20px;padding:4px 6px;border:1px solid rgba(255,255,255,0.1);}"
".cam-hud-btn{background:transparent;border:none;color:#fff;font-size:1rem;cursor:pointer;padding:6px 8px;border-radius:50%;display:flex;align-items:center;justify-content:center;transition:background 0.2s;}"
".cam-hud-btn:hover{background:rgba(255,255,255,0.15);}"
".cam-hud-btn.active{color:var(--accent-amber);text-shadow:0 0 10px rgba(245,158,11,0.8);}"
".cam-badge-bottom{position:absolute;bottom:12px;left:12px;display:flex;align-items:center;gap:6px;background:rgba(18,24,38,0.75);backdrop-filter:blur(8px);padding:5px 10px;border-radius:14px;font-size:0.75rem;font-weight:800;letter-spacing:0.5px;border:1px solid rgba(255,255,255,0.1);}"
".dot-ready{width:8px;height:8px;border-radius:50%;background-color:var(--accent-green);box-shadow:0 0 8px var(--accent-green);}"
".sec-header{display:flex;justify-content:space-between;align-items:center;font-size:0.75rem;font-weight:800;color:var(--text-muted);letter-spacing:1px;text-transform:uppercase;margin-top:4px;}"
".sec-link{color:var(--accent-blue);text-decoration:none;cursor:pointer;font-weight:700;font-size:0.8rem;}"
".ifs-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;}"
".ifs-card{background:var(--bg-card);border:1px solid var(--border);border-radius:14px;padding:10px 8px;display:flex;flex-direction:column;gap:4px;position:relative;overflow:hidden;}"
".ifs-slot{font-size:0.75rem;font-weight:700;color:#94a3b8;display:flex;align-items:center;justify-content:space-between;}"
".ifs-color-dot{width:8px;height:8px;border-radius:50%;border:1px solid rgba(255,255,255,0.3);}"
".ifs-mat{font-size:0.75rem;font-weight:600;color:var(--text-muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
".ifs-temp{font-size:1.1rem;font-weight:800;color:#fff;margin-top:4px;}"
".temp-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}"
".temp-card{background:var(--bg-card);border:1px solid var(--border);border-radius:16px;padding:14px;position:relative;overflow:hidden;}"
".temp-sub{font-size:0.75rem;font-weight:700;color:var(--text-muted);}"
".temp-val{font-size:1.6rem;font-weight:800;color:#fff;margin:8px 0 10px 0;}"
".temp-accent{position:absolute;bottom:0;left:0;right:0;height:4px;background:linear-gradient(90deg,var(--accent-blue),transparent);}"
".panel{background:var(--bg-card);border:1px solid var(--border);border-radius:16px;padding:12px;display:flex;flex-direction:column;gap:8px;}"
".btn-grid-4{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;}"
".btn-grid-2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}"
"button{padding:9px;background:var(--btn-bg);color:var(--text-main);border:1px solid var(--border);border-radius:8px;font-weight:700;font-size:0.8rem;cursor:pointer;transition:all 0.15s;}"
"button:hover{background:var(--btn-hover);}"
"button.warn{background:#b45309;border-color:#b45309;}button.warn:hover{background:#d97706;}"
"button.danger{background:#991b1b;border-color:#991b1b;}button.danger:hover{background:#dc2626;}"
"button.blue{background:#2563eb;border-color:#2563eb;}button.blue:hover{background:#1d4ed8;}"
".btn-preheat-active{background:var(--accent-red) !important;border-color:var(--accent-red) !important;box-shadow:0 0 10px rgba(239,68,68,0.6) !important;}"
".btn-motion-active{background:var(--accent-amber) !important;border-color:var(--accent-amber) !important;box-shadow:0 0 10px rgba(245,158,11,0.6) !important;}"
".btn-active{background:var(--accent-green) !important;border-color:var(--accent-green) !important;box-shadow:0 0 8px rgba(34,197,94,0.6) !important;}"
".mgmt-panel{display:none;background:var(--bg-card);border:1px solid var(--border);border-radius:14px;padding:12px;}"
".mgmt-panel input,.mgmt-panel select{width:100%;padding:8px;margin-bottom:8px;background:var(--bg-page);border:1px solid var(--border);border-radius:6px;color:#fff;box-sizing:border-box;}"
".ifs-input-row{display:grid;grid-template-columns:1fr 50px;gap:8px;margin-bottom:6px;align-items:center;}"
".p-item{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid var(--border);font-size:0.85rem;}"
".footer{display:flex;justify-content:flex-end;align-items:center;font-size:0.75rem;color:var(--text-muted);padding-top:4px;}"
".pill-btn{cursor:pointer;margin-left:6px;padding:2px 8px;background:var(--btn-bg);border-radius:6px;font-size:0.7rem;font-weight:700;color:#fff;}"
"</style></head><body>"
"<div class='app-container'>"

"<div class='top-bar'>"
"<div class='top-title'><span class='dot-ready'></span><select id='printerSelect' class='select-printer' onchange='switchPrinter(this.value)'></select></div>"
"<button onclick='togglePrinterMgmt()' style='padding:4px 10px;border-radius:14px;font-size:0.75rem;'>&#9881; Manage</button>"
"</div>"

"<div id='printerMgmtBox' class='mgmt-panel'>"
"<div style='font-size:0.85rem;font-weight:700;margin-bottom:6px;'>Add FlashForge Printer</div>"
"<input type='text' id='newName' placeholder='Printer Name (e.g. AD5X Jim)'>"
"<input type='text' id='newIp' placeholder='IP Address (e.g. 192.168.1.124)'>"
"<button onclick='addPrinter()' class='blue' style='width:100%;margin-bottom:8px;'>Save Printer</button>"
"<div id='printerListContainer'></div>"
"</div>"

"<div class='cam-card'>"
"<img id='stream' src='' alt='Live Camera Feed' onerror='this.alt=\"Camera Feed Offline\";'>"
"<div class='cam-hud-top'>"
"<button id='btnHudLight' class='cam-hud-btn' onclick='sendAction(\"toggle_light\")' title='Chamber Light'>💡</button>"
"<button class='cam-hud-btn' onclick='takeSnapshot()' title='Snapshot'>📷</button>"
"<button class='cam-hud-btn' onclick='reloadStream()' title='Refresh Stream'>🔄</button>"
"<button class='cam-hud-btn' onclick='toggleFullscreen()' title='Fullscreen'>⛶</button>"
"</div>"
"<div class='cam-badge-bottom'><span class='dot-ready' id='statusDot'></span><span id='statusBadge'>READY</span></div>"
"</div>"

"<div class='sec-header'><span>IFS Slots</span><span class='sec-link' onclick='toggleIfsMgmt()'>MANAGE</span></div>"

"<div id='ifsMgmtBox' class='mgmt-panel'>"
"<div style='font-size:0.85rem;font-weight:700;margin-bottom:8px;'>Configure IFS Filament Slots</div>"
"<div class='ifs-input-row'><input type='text' id='s0_mat' placeholder='Slot 1 Material (e.g. PLA)'><input type='color' id='s0_col' value='#ffffff'></div>"
"<div class='ifs-input-row'><input type='text' id='s1_mat' placeholder='Slot 2 Material (e.g. PETG)'><input type='color' id='s1_col' value='#2563eb'></div>"
"<div class='ifs-input-row'><input type='text' id='s2_mat' placeholder='Slot 3 Material (e.g. PLA)'><input type='color' id='s2_col' value='#16a34a'></div>"
"<div class='ifs-input-row'><input type='text' id='s3_mat' placeholder='Slot 4 Material (e.g. PETG)'><input type='color' id='s3_col' value='#dc2626'></div>"
"<button onclick='saveIfsSlots()' class='blue' style='width:100%;margin-top:4px;'>Save Spools</button>"
"</div>"

"<div class='ifs-grid'>"
"<div class='ifs-card'><div class='ifs-slot'><span>Slot 1</span><span id='c0_dot' class='ifs-color-dot'></span></div><span class='ifs-mat' id='s0_lbl'>PLA</span><span class='ifs-temp' id='s0_t'>--°</span></div>"
"<div class='ifs-card'><div class='ifs-slot'><span>Slot 2</span><span id='c1_dot' class='ifs-color-dot'></span></div><span class='ifs-mat' id='s1_lbl'>PETG</span><span class='ifs-temp' id='s1_t'>--°</span></div>"
"<div class='ifs-card'><div class='ifs-slot'><span>Slot 3</span><span id='c2_dot' class='ifs-color-dot'></span></div><span class='ifs-mat' id='s2_lbl'>PLA</span><span class='ifs-temp' id='s2_t'>--°</span></div>"
"<div class='ifs-card'><div class='ifs-slot'><span>Slot 4</span><span id='c3_dot' class='ifs-color-dot'></span></div><span class='ifs-mat' id='s3_lbl'>PETG</span><span class='ifs-temp' id='s3_t'>--°</span></div>"
"</div>"

"<div class='sec-header'><span>Temperatures</span></div>"
"<div class='temp-grid'>"
"<div class='temp-card'><div class='temp-sub'><span>Extruder</span></div><div class='temp-val' id='nozzleVal'>--°</div><div class='temp-accent'></div></div>"
"<div class='temp-card'><div class='temp-sub'><span>Bed</span></div><div class='temp-val' id='bedVal'>--°</div><div class='temp-accent'></div></div>"
"</div>"

"<div class='sec-header'><span>Quick Controls</span></div>"
"<div class='panel'>"
"<div class='btn-grid-4'>"
"<button id='btnCool' class='blue' onclick='triggerPreheat(\"cooldown\")'>Cool</button>"
"<button id='btnPla' onclick='triggerPreheat(\"preheat_pla\")'>PLA</button>"
"<button id='btnPetg' onclick='triggerPreheat(\"preheat_petg\")'>PETG</button>"
"<button id='btnBedSoak' onclick='triggerPreheat(\"bed_soak\")'>Bed 60°</button>"
"</div>"
"<div class='btn-grid-4'>"
"<button onclick='triggerMotion(\"home\", this)'>Home</button>"
"<button onclick='triggerMotion(\"lower_bed\", this)'>Lower</button>"
"<button onclick='triggerMotion(\"center_head\", this)'>Center</button>"
"<button onclick='triggerMotion(\"motors_off\", this)'>Unlock</button>"
"</div>"
"<div class='btn-grid-2'>"
"<button id='btnPartFan' onclick='sendAction(\"toggle_fan\")'>Aux Fan: OFF</button>"
"<button id='btnChamberFan' onclick='sendAction(\"toggle_chamber_fan\")'>Filter Fan: OFF</button>"
"</div>"
"<div class='btn-grid-2'>"
"<button id='btnPause' class='warn' onclick='togglePauseResume()'>Pause Print</button>"
"<button class='danger' onclick='cancelPrint()'>Cancel Print</button>"
"</div>"
"</div>"

"<div class='footer'>Updated:&nbsp;<span id='updated'>--</span><span id='timeFmt' class='pill-btn' onclick='toggleTimeFormat()'>24h</span></div>"

"</div><script>"
"var activeIp='';"
"var currentStatus='READY';"
"var activePreheat='';"
"var use24h=(localStorage.getItem('timeFormat24h')!=='false');"

"function updateTimeFmtLabel(){var b=document.getElementById('timeFmt');if(b)b.innerText=use24h?'24h':'12h';}"
"function toggleTimeFormat(){use24h=!use24h;localStorage.setItem('timeFormat24h',use24h);updateTimeFmtLabel();fetchData();}"
"function togglePrinterMgmt(){var m=document.getElementById('printerMgmtBox');m.style.display=(m.style.display==='block')?'none':'block';}"
"function toggleIfsMgmt(){var m=document.getElementById('ifsMgmtBox');m.style.display=(m.style.display==='block')?'none':'block';}"

"function reloadStream(){if(!activeIp)return;var img=document.getElementById('stream');img.src='http://'+activeIp+':8080/?action=stream&t='+new Date().getTime();}"
"function takeSnapshot(){if(!activeIp)return;var a=document.createElement('a');a.href='http://'+activeIp+':8080/?action=snapshot&t='+new Date().getTime();a.download='ad5x_snap_'+Date.now()+'.jpg';a.target='_blank';document.body.appendChild(a);a.click();document.body.removeChild(a);}"
"function toggleFullscreen(){var el=document.querySelector('.cam-card');if(!document.fullscreenElement){if(el.requestFullscreen)el.requestFullscreen();}else{if(document.exitFullscreen)document.exitFullscreen();}}"

"function switchPrinter(idx){fetch('/api/printers/select?idx='+idx,{method:'POST'}).then(function(){loadPrinters(true);setTimeout(fetchData,500);});}"
"function addPrinter(){var n=document.getElementById('newName').value.trim();var ip=document.getElementById('newIp').value.trim();if(!n||!ip){alert('Enter Name and IP');return;}fetch('/api/printers/add?name='+encodeURIComponent(n)+'&ip='+encodeURIComponent(ip),{method:'POST'}).then(function(){document.getElementById('newName').value='';document.getElementById('newIp').value='';loadPrinters(true);setTimeout(fetchData,600);});}"
"function deletePrinter(idx){if(confirm('Delete printer?')){fetch('/api/printers/delete?idx='+idx,{method:'POST'}).then(function(){loadPrinters(true);});}}"

"function loadPrinters(refreshFeed){fetch('/api/printers').then(function(r){return r.json();}).then(function(d){var sel=document.getElementById('printerSelect');var c=document.getElementById('printerListContainer');sel.innerHTML='';c.innerHTML='';if(!d.printers||d.printers.length===0){document.getElementById('printerMgmtBox').style.display='block';activeIp='';var opt=document.createElement('option');opt.text='-- Add Printer --';sel.appendChild(opt);return;}d.printers.forEach(function(p,i){var opt=document.createElement('option');opt.value=i;opt.text=p.name;if(i===d.active){opt.selected=true;activeIp=p.ip;}sel.appendChild(opt);var itm=document.createElement('div');itm.className='p-item';itm.innerHTML='<span><b>'+p.name+'</b> ('+p.ip+')</span>';var delBtn=document.createElement('button');delBtn.className='danger';delBtn.style.padding='4px 8px';delBtn.innerText='Del';delBtn.onclick=function(){deletePrinter(i);};itm.appendChild(delBtn);c.appendChild(itm);});if(refreshFeed||!document.getElementById('stream').src){reloadStream();}});}"

"function loadIfs(){fetch('/api/ifs').then(function(r){return r.json();}).then(function(d){for(var i=0;i<4;i++){var m=d['s'+i+'_mat'];var c=d['s'+i+'_col'];var mi=document.getElementById('s'+i+'_mat');var ci=document.getElementById('s'+i+'_col');var ml=document.getElementById('s'+i+'_lbl');var cd=document.getElementById('c'+i+'_dot');if(mi)mi.value=m;if(ci)ci.value=c;if(ml)ml.innerText=m;if(cd)cd.style.backgroundColor=c;}});}"
"function saveIfsSlots(){var q='?';for(var i=0;i<4;i++){q+='s'+i+'_mat='+encodeURIComponent(document.getElementById('s'+i+'_mat').value)+'&s'+i+'_col='+encodeURIComponent(document.getElementById('s'+i+'_col').value)+'&';}fetch('/api/ifs/save'+q,{method:'POST'}).then(function(){loadIfs();toggleIfsMgmt();});}"

"function sendAction(act){return fetch('/api/cmd?action='+act,{method:'POST'}).then(function(){setTimeout(fetchData,500);});}"
"function triggerMotion(act,btn){if(btn)btn.classList.add('btn-motion-active');sendAction(act).then(function(){setTimeout(function(){if(btn)btn.classList.remove('btn-motion-active');},1200);}).catch(function(){if(btn)btn.classList.remove('btn-motion-active');});}"

"function updatePreheatButtons(){var p=document.getElementById('btnPla');var pt=document.getElementById('btnPetg');var b=document.getElementById('btnBedSoak');if(p)p.classList.toggle('btn-preheat-active',activePreheat==='preheat_pla');if(pt)pt.classList.toggle('btn-preheat-active',activePreheat==='preheat_petg');if(b)b.classList.toggle('btn-preheat-active',activePreheat==='bed_soak');}"
"function triggerPreheat(mode){if(mode==='cooldown'){activePreheat='';}else{activePreheat=mode;}updatePreheatButtons();sendAction(mode);}"
"function togglePauseResume(){var t=(currentStatus==='PAUSED')?'resume_print':'pause_print';sendAction(t);}"
"function cancelPrint(){if(confirm('Stop current print?')){sendAction('cancel_print');}}"

"function fetchData(){fetch('/api/status').then(function(r){return r.json();}).then(function(d){currentStatus=d.status;document.getElementById('statusBadge').innerText=d.status;"
"var nStr=d.nozzle+'°';if(d.nozzle_target>0)nStr+=' / '+d.nozzle_target+'°';document.getElementById('nozzleVal').innerText=nStr;"
"var bStr=d.bed+'°';if(d.bed_target>0)bStr+=' / '+d.bed_target+'°';document.getElementById('bedVal').innerText=bStr;"
"var tStr=(d.ifs_temp>0)?(Math.round(d.ifs_temp)+'°'):'--°';"
"for(var i=0;i<4;i++){var el=document.getElementById('s'+i+'_t');if(el)el.innerText=tStr;}"
"var hudBulb=document.getElementById('btnHudLight');if(d.light){hudBulb.classList.add('active');}else{hudBulb.classList.remove('active');}"
"var partBtn=document.getElementById('btnPartFan');if(d.part_fan){partBtn.classList.add('btn-active');partBtn.innerText='Aux Fan: ON';}else{partBtn.classList.remove('btn-active');partBtn.innerText='Aux Fan: OFF';}"
"var chBtn=document.getElementById('btnChamberFan');if(d.chamber_fan){chBtn.classList.add('btn-active');chBtn.innerText='Filter Fan: ON';}else{chBtn.classList.remove('btn-active');chBtn.innerText='Filter Fan: OFF';}"
"var p=document.getElementById('btnPause');if(d.status==='PAUSED'){p.innerText='Resume Print';}else{p.innerText='Pause Print';}"
"if(d.nozzle_target>=220&&d.nozzle_target<=230&&d.bed_target>=60&&d.bed_target<=70){activePreheat='preheat_pla';}else if(d.nozzle_target>=230&&d.nozzle_target<=240&&d.bed_target>=70&&d.bed_target<=80){activePreheat='preheat_petg';}else if(d.nozzle_target===0&&d.bed_target>=55&&d.bed_target<=65){activePreheat='bed_soak';}else if(d.nozzle_target===0&&d.bed_target===0){activePreheat='';}"
"updatePreheatButtons();"
"var now=new Date();document.getElementById('updated').innerText=now.toLocaleTimeString([],{hour12:!use24h});}).catch(function(){document.getElementById('statusBadge').innerText='OFFLINE';});}"

"updateTimeFmtLabel();loadPrinters(true);loadIfs();setInterval(fetchData,3000);fetchData();"
"</script></body></html>";

void setup() {
  Serial.begin(115200);
  delay(2000);

  printerMutex = xSemaphoreCreateMutex();
  tcpMutex = xSemaphoreCreateMutex();
  loadPrintersFromNVS();
  loadIfsFromNVS();

  Serial.println("\n[AD5X-VIEW] Starting Station...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  if (USE_STATIC_IP) {
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("[WARN] Static IP failed, using DHCP");
    }
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[ERROR] Wi-Fi connection failed!");
    return;
  }

  Serial.println("\n[OK] Connected!");
  Serial.print("Dashboard URL: http://");
  Serial.println(WiFi.localIP());

  // Web routes
  server.on("/", HTTP_GET, [](){
    server.send_P(200, "text/html", INDEX_HTML);
  });

  // Printer Management Endpoints
  server.on("/api/printers", HTTP_GET, [](){
    DynamicJsonDocument doc(2048);
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
      doc["active"] = activePrinterIndex;
      JsonArray arr = doc.createNestedArray("printers");
      for (const auto &p : printerList) {
        JsonObject obj = arr.createNestedObject();
        obj["name"] = p.name;
        obj["ip"] = p.ip;
      }
      xSemaphoreGive(printerMutex);
    }
    String resp;
    serializeJson(doc, resp);
    server.send(200, "application/json", resp);
  });

  server.on("/api/printers/select", HTTP_ANY, [](){
    if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
        if (idx >= 0 && idx < (int)printerList.size()) {
          activePrinterIndex = idx;
          savePrintersToNVS();
        }
        xSemaphoreGive(printerMutex);
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/printers/add", HTTP_ANY, [](){
    if (server.hasArg("name") && server.hasArg("ip")) {
      String n = server.arg("name");
      String ip = server.arg("ip");
      if (n.length() > 0 && ip.length() > 0) {
        if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
          printerList.push_back({n, ip});
          activePrinterIndex = printerList.size() - 1;
          savePrintersToNVS();
          xSemaphoreGive(printerMutex);
        }
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/printers/delete", HTTP_ANY, [](){
    if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
        if (idx >= 0 && idx < (int)printerList.size()) {
          printerList.erase(printerList.begin() + idx);
          if (printerList.empty()) activePrinterIndex = -1;
          else activePrinterIndex = 0;
          savePrintersToNVS();
        }
        xSemaphoreGive(printerMutex);
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  // IFS Management Endpoints
  server.on("/api/ifs", HTTP_GET, [](){
    DynamicJsonDocument doc(512);
    for (int i = 0; i < 4; i++) {
      doc["s" + String(i) + "_mat"] = ifsSlots[i].material;
      doc["s" + String(i) + "_col"] = ifsSlots[i].color;
    }
    String resp;
    serializeJson(doc, resp);
    server.send(200, "application/json", resp);
  });

  server.on("/api/ifs/save", HTTP_ANY, [](){
    for (int i = 0; i < 4; i++) {
      String mKey = "s" + String(i) + "_mat";
      String cKey = "s" + String(i) + "_col";
      if (server.hasArg(mKey)) ifsSlots[i].material = server.arg(mKey);
      if (server.hasArg(cKey)) ifsSlots[i].color = server.arg(cKey);
    }
    saveIfsToNVS();
    server.send(200, "application/json", "{\"success\":true}");
  });

  // Telemetry Endpoint
  server.on("/api/status", HTTP_GET, [](){
    DynamicJsonDocument doc(512);
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
      doc["status"] = printer.status;
      doc["file"] = printer.filename;
      doc["progress"] = printer.progress;
      doc["layer"] = printer.current_layer;
      doc["total_layers"] = printer.total_layers;
      doc["nozzle"] = printer.nozzle_temp;
      doc["nozzle_target"] = printer.nozzle_target;
      doc["bed"] = printer.bed_temp;
      doc["bed_target"] = printer.bed_target;
      doc["ifs_temp"] = printer.ifs_temp;
      doc["light"] = printer.light_on;
      doc["part_fan"] = printer.part_fan;
      doc["chamber_fan"] = printer.chamber_fan;
      xSemaphoreGive(printerMutex);
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // Unified Command Router
  server.on("/api/cmd", HTTP_ANY, [](){
    if (server.hasArg("action")) {
      String act = server.arg("action");
      bool ok = false;

      if (act == "toggle_light") ok = sendPrinterTcpCommand("~M146\r\n");
      else if (act == "pause_print") ok = sendPrinterTcpCommand("~M25\r\n");
      else if (act == "resume_print") ok = sendPrinterTcpCommand("~M24\r\n");
      else if (act == "cancel_print") ok = sendPrinterTcpCommand("~M26\r\n");
      else if (act == "cooldown") {
        std::vector<String> cmds = {"~M104 S0\r\n", "~M140 S0\r\n"};
        ok = sendPrinterTcpCommands(cmds, 150);
      }
      else if (act == "preheat_pla") {
        std::vector<String> cmds = {"~M104 S225\r\n", "~M140 S65\r\n"};
        ok = sendPrinterTcpCommands(cmds, 150);
      }
      else if (act == "preheat_petg") {
        std::vector<String> cmds = {"~M104 S235\r\n", "~M140 S75\r\n"};
        ok = sendPrinterTcpCommands(cmds, 150);
      }
      else if (act == "bed_soak") ok = sendPrinterTcpCommand("~M140 S60\r\n");
      else if (act == "home") ok = sendPrinterTcpCommand("~G28\r\n");
      else if (act == "lower_bed") {
        std::vector<String> cmds = {"~G91\r\n", "~G1 Z50 F1200\r\n", "~G90\r\n"};
        ok = sendPrinterTcpCommands(cmds, 100);
      }
      else if (act == "center_head") {
        std::vector<String> cmds = {"~G90\r\n", "~G1 X110 Y110 Z50 F3600\r\n"};
        ok = sendPrinterTcpCommands(cmds, 100);
      }
      else if (act == "motors_off") ok = sendPrinterTcpCommand("~M84\r\n");
      else if (act == "toggle_fan") {
        bool nextState = !printer.part_fan;
        ok = sendPrinterTcpCommand(nextState ? "~M106 P1 S255\r\n" : "~M107 P1\r\n");
        if (ok && xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
          printer.part_fan = nextState;
          xSemaphoreGive(printerMutex);
        }
      }
      else if (act == "toggle_chamber_fan") {
        bool nextState = !printer.chamber_fan;
        ok = sendPrinterTcpCommand(nextState ? "~M106 P2 S255\r\n" : "~M107 P2\r\n");
        if (ok && xSemaphoreTake(printerMutex, pdMS_TO_TICKS(2000))) {
          printer.chamber_fan = nextState;
          xSemaphoreGive(printerMutex);
        }
      }

      if (ok) {
        server.send(200, "application/json", "{\"success\":true}");
        return;
      }
      server.send(500, "application/json", "{\"error\":\"Execution failed\"}");
      return;
    }
    server.send(400, "application/json", "{\"error\":\"Missing action\"}");
  });

  server.begin();
  Serial.println("[OK] HTTP Server Running!");

  xTaskCreatePinnedToCore(
    telemetryTask,
    "TelemetryTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  server.handleClient();
}