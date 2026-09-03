#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>

// --- Wi-Fi Credentials (2.4GHz Only) ---
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

// --- Multi-Printer Storage Structure ---
struct PrinterEntry {
  String name;
  String ip;
};

std::vector<PrinterEntry> printerList;
int activePrinterIndex = 0;
Preferences prefs;
SemaphoreHandle_t printerMutex;
SemaphoreHandle_t tcpMutex;

// --- Web Server on Port 80 ---
WebServer server(80);

// --- Telemetry & State Cache ---
struct PrinterData {
  String status = "Initializing...";
  String filename = "None";
  int progress = 0;
  int current_layer = 0;
  int total_layers = 0;
  float nozzle_temp = 0.0;
  float bed_temp = 0.0;
  bool light_on = false;
  bool part_fan = false;
  bool chamber_fan = false;
  unsigned long last_update = 0;
} printer;

// --- Load & Save Printers from ESP32 NVS Flash ---
void loadPrintersFromNVS() {
  prefs.begin("ad5x_view", false);
  String json = prefs.getString("printers", "");
  activePrinterIndex = prefs.getInt("active_idx", 0);

  printerList.clear();
  if (json.length() > 0) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, json) == DeserializationError::Ok && doc.is<JsonArray>()) {
      for (JsonObject obj : doc.as<JsonArray>()) {
        printerList.push_back({obj["name"].as<String>(), obj["ip"].as<String>()});
      }
    }
  }

  // Force active index to 0 whenever at least one printer is saved
  if (printerList.empty()) {
    activePrinterIndex = -1;
  } else if (activePrinterIndex >= (int)printerList.size() || activePrinterIndex < 0) {
    activePrinterIndex = 0;
  }
}

void savePrintersToNVS() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &p : printerList) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = p.name;
    obj["ip"] = p.ip;
  }
  String json;
  serializeJson(doc, json);
  prefs.putString("printers", json);
  prefs.putInt("active_idx", activePrinterIndex);
}

// Safely retrieve active printer IP across cores
String getActiveIp() {
  String ip = "";
  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
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

// --- Helper: Send TCP Command and Read Response ---
String sendAndReceiveTcp(WiFiClient &client, const char* cmd) {
  client.print(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 1500) {
    while (client.available()) {
      char c = client.read();
      resp += c;
    }
    if (resp.indexOf("ok") != -1 || resp.indexOf("\n") != -1) {
      break;
    }
    delay(15);
  }
  return resp;
}

// --- Send Sequence of Commands safely over TCP ---
bool sendPrinterTcpCommands(const std::vector<String>& commands) {
  String targetIp = getActiveIp();
  if (targetIp.length() == 0) return false;

  // Wait for background telemetry task to clear port 8899
  if (!xSemaphoreTake(tcpMutex, pdMS_TO_TICKS(3000))) {
    return false;
  }

  WiFiClient client;
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 2500)) {
    Serial.println("[TCP] Connection to printer failed");
    xSemaphoreGive(tcpMutex);
    return false;
  }

  // Flashforge handshake
  sendAndReceiveTcp(client, "~M601 S1\r\n");

  // Send each G-code command with breathing room
  for (const String& cmd : commands) {
    sendAndReceiveTcp(client, cmd.c_str());
    delay(40);
  }

  client.stop();
  xSemaphoreGive(tcpMutex);
  return true;
}

bool sendPrinterTcpCommand(const char* cmd) {
  std::vector<String> cmds;
  cmds.push_back(String(cmd));
  return sendPrinterTcpCommands(cmds);
}

// --- Background Telemetry Poller (Core 0) ---
void updatePrinterTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;

  String targetIp = getActiveIp();
  if (targetIp.length() == 0) {
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
      printer.status = "No Printer Configured";
      printer.nozzle_temp = 0.0;
      printer.bed_temp = 0.0;
      printer.progress = 0;
      xSemaphoreGive(printerMutex);
    }
    return;
  }

  if (!xSemaphoreTake(tcpMutex, pdMS_TO_TICKS(1000))) {
    return; // Wait for manual user commands to complete
  }

  WiFiClient client;
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 2000)) {
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
      printer.status = "Offline";
      xSemaphoreGive(printerMutex);
    }
    xSemaphoreGive(tcpMutex);
    return;
  }

  // 1. Handshake
  sendAndReceiveTcp(client, "~M601 S1\r\n");

  // 2. Machine Status (M119)
  String statusResp = sendAndReceiveTcp(client, "~M119\r\n");
  String parsedStatus = "Online";
  if (statusResp.indexOf("READY") != -1 || statusResp.indexOf("IDLE") != -1) {
    parsedStatus = "Idle";
  } else if (statusResp.indexOf("BUILDING") != -1 || statusResp.indexOf("PRINTING") != -1) {
    parsedStatus = "Printing";
  } else if (statusResp.indexOf("PAUSED") != -1) {
    parsedStatus = "Paused";
  } else if (statusResp.indexOf("COMPLETE") != -1) {
    parsedStatus = "Completed";
  }

  // 3. Temperatures (M105)
  String tempResp = sendAndReceiveTcp(client, "~M105\r\n");
  float nozzle = 0.0, bed = 0.0;
  int tIndex = tempResp.indexOf("T0:");
  if (tIndex != -1) {
    int spaceIdx = tempResp.indexOf(" ", tIndex);
    if (spaceIdx != -1) {
      nozzle = tempResp.substring(tIndex + 3, spaceIdx).toFloat();
    }
  }
  int bIndex = tempResp.indexOf("B:");
  if (bIndex != -1) {
    int spaceIdx = tempResp.indexOf(" ", bIndex);
    if (spaceIdx != -1) {
      bed = tempResp.substring(bIndex + 2, spaceIdx).toFloat();
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
    if (totBytes > 0) {
      progress = (int)((curBytes * 100) / totBytes);
    }
  }

  client.stop();
  xSemaphoreGive(tcpMutex);

  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
    printer.status = parsedStatus;
    printer.nozzle_temp = nozzle;
    printer.bed_temp = bed;
    printer.progress = progress;
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
"<title>AD5X Monitor</title><style>"
":root{--bg-body:#0f172a;--bg-card:#1e293b;--btn-bg:#334155;--btn-hover:#475569;--btn-warn:#b45309;--btn-warn-hover:#d97706;--btn-danger:#991b1b;--btn-danger-hover:#dc2626;--btn-success:#16a34a;--btn-blue:#2563eb;--btn-blue-hover:#1d4ed8;--text-main:#f8fafc;--text-muted:#94a3b8;--border-color:#334155;}"
"body{background-color:var(--bg-body);color:var(--text-main);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:16px;box-sizing:border-box;}"
".card{background-color:var(--bg-card);border:1px solid var(--border-color);border-radius:12px;width:100%;max-width:440px;padding:18px;box-shadow:0 10px 25px rgba(0,0,0,0.5);}"
".header-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;gap:6px;}"
".select-printer{background:#0f172a;color:#f8fafc;border:1px solid var(--border-color);border-radius:6px;padding:7px 9px;font-size:0.9rem;font-weight:600;flex-grow:1;}"
".btn-sm{padding:7px 9px;background:var(--btn-bg);color:#f8fafc;border:none;border-radius:6px;cursor:pointer;font-size:0.75rem;white-space:nowrap;font-weight:600;}"
".btn-sm:hover{background:var(--btn-hover);}"
".cam-frame{width:100%;height:240px;border-radius:8px;overflow:hidden;background:#000;display:flex;align-items:center;justify-content:center;border:1px solid var(--border-color);}"
".cam-frame img{width:100%;height:100%;object-fit:cover;}"
".cam-actions{display:flex;gap:6px;margin-top:6px;margin-bottom:12px;}"
".btn-grid-4{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-bottom:10px;}"
".btn-grid-2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px;}"
"button{padding:9px;background:var(--btn-bg);color:var(--text-main);border:none;border-radius:6px;font-weight:600;font-size:0.8rem;cursor:pointer;transition:background 0.15s,color 0.15s;}"
"button:hover{background:var(--btn-hover);}"
".btn-active{background-color:var(--btn-success) !important;color:#ffffff !important;box-shadow:0 0 8px rgba(22,163,74,0.6);}"
"button.warn{background:var(--btn-warn);}button.warn:hover{background:var(--btn-warn-hover);}"
"button.danger{background:var(--btn-danger);}button.danger:hover{background:var(--btn-danger-hover);}"
"button.blue{background:var(--btn-blue);}button.blue:hover{background:var(--btn-blue-hover);}"
".sec-label{font-size:0.75rem;font-weight:700;color:var(--text-muted);text-transform:uppercase;letter-spacing:0.5px;margin:10px 0 4px 0;}"
".row{display:flex;justify-content:space-between;margin-bottom:5px;font-size:0.85rem;}"
".row.col{flex-direction:column;gap:3px;}"
".label{color:var(--text-muted);}.val{font-weight:600;}"
".file-box{word-break:break-all;background:#0f172a;padding:5px;border-radius:4px;border:1px solid var(--border-color);font-size:0.75rem;}"
".footer{display:flex;justify-content:flex-end;align-items:center;margin-top:12px;padding-top:8px;border-top:1px solid var(--border-color);font-size:0.75rem;color:var(--text-muted);}"
".pill-btn{cursor:pointer;margin-left:6px;padding:2px 6px;background:var(--btn-bg);color:#f8fafc;border-radius:4px;font-size:0.7rem;font-weight:600;user-select:none;}"
".mgmt-panel{display:none;background:#0f172a;border:1px solid var(--border-color);border-radius:8px;padding:12px;margin-bottom:12px;}"
".mgmt-panel input{width:100%;padding:7px;margin-bottom:6px;background:#1e293b;border:1px solid var(--border-color);border-radius:4px;color:#fff;box-sizing:border-box;}"
".p-list-item{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid #1e293b;font-size:0.8rem;}"
"</style></head><body><div class='card'>"
"<div class='header-row'>"
"<select id='printerSelect' class='select-printer' onchange='switchPrinter(this.value)'></select>"
"<button id='btnToggleCam' class='btn-sm' onclick='toggleCamera()'>📷 Cam</button>"
"<button class='btn-sm' onclick='toggleMgmt()'>&#9881; Manage</button>"
"</div>"
"<div id='mgmtBox' class='mgmt-panel'>"
"<div style='font-size:0.85rem;font-weight:700;margin-bottom:6px;'>Add New Printer</div>"
"<input type='text' id='newName' placeholder='Printer Name (e.g. AD5X Lab)'>"
"<input type='text' id='newIp' placeholder='IP Address (e.g. 192.168.1.125)'>"
"<button onclick='addPrinter()' class='blue' style='width:100%;margin-bottom:10px;'>Save Printer</button>"
"<div style='font-size:0.8rem;font-weight:700;margin-bottom:4px;'>Configured Printers</div>"
"<div id='printerListContainer'></div>"
"</div>"
"<div id='camWrapper'>"
"<div id='camFrame' class='cam-frame'><img id='stream' src='' alt='Live Stream' onerror='this.alt=\"No camera feed / offline\";'></div>"
"<div class='cam-actions'>"
"<button class='btn-sm' style='flex:1;' onclick='reloadStream()'>🔄 Refresh</button>"
"<button class='btn-sm' style='flex:1;' onclick='takeSnapshot()'>💾 Snapshot</button>"
"<button class='btn-sm' style='flex:1;' onclick='toggleFullscreen()'>⛶ Fullscreen</button>"
"</div>"
"</div>"
"<div class='sec-label'>Core Controls</div>"
"<div class='btn-grid-2'>"
"<button id='btnLed' onclick='sendAction(\"toggle_light\")'>Toggle LEDs</button>"
"<button id='btnPause' class='warn' onclick='togglePauseResume()'>Pause Print</button>"
"<button class='danger' onclick='cancelPrint()'>Cancel Print</button>"
"<button id='btnFind' onclick='findPrinter()'>💡 Flash Lights</button>"
"</div>"
"<div class='sec-label'>Temperatures</div>"
"<div class='btn-grid-4'>"
"<button class='blue' onclick='sendAction(\"cooldown\")'>Cool Off</button>"
"<button onclick='sendAction(\"preheat_pla\")'>PLA</button>"
"<button onclick='sendAction(\"preheat_petg\")'>PETG</button>"
"<button onclick='sendAction(\"bed_soak\")'>Bed 60°</button>"
"</div>"
"<div class='sec-label'>Motion & Fans</div>"
"<div class='btn-grid-4'>"
"<button onclick='sendAction(\"home\")'>Home (G28)</button>"
"<button onclick='sendAction(\"lower_bed\")'>Lower Bed</button>"
"<button onclick='sendAction(\"center_head\")'>Center</button>"
"<button onclick='sendAction(\"motors_off\")'>Unlock</button>"
"</div>"
"<div class='btn-grid-2' style='margin-bottom:10px;'>"
"<button id='btnPartFan' onclick='sendAction(\"toggle_fan\")'>Aux Fan: OFF</button>"
"<button id='btnChamberFan' onclick='sendAction(\"toggle_chamber_fan\")'>Filter Fan: OFF</button>"
"</div>"
"<div class='row'><span class='label'>Status:</span><span id='status' class='val'>--</span></div>"
"<div class='row col'><span class='label'>File:</span><div id='file' class='file-box'>--</div></div>"
"<div class='row'><span class='label'>Progress:</span><span id='progress' class='val'>--%</span></div>"
"<div class='row'><span class='label'>Layer:</span><span id='layer' class='val'>-- / --</span></div>"
"<div class='row'><span class='label'>Nozzle:</span><span id='nozzle' class='val'>--&deg;C</span></div>"
"<div class='row'><span class='label'>Bed:</span><span id='bed' class='val'>--&deg;C</span></div>"
"<div class='row'><span class='label'>Light:</span><span id='light' class='val'>--</span></div>"
"<div class='footer'>Updated:&nbsp;<span id='updated'>--</span><span id='timeFmt' class='pill-btn' onclick='toggleTimeFormat()'>12h</span></div>"
"</div><script>"
"var currentStatus='Offline';"
"var activeIp='';"
"var use24h=(localStorage.getItem('timeFormat24h')==='true');"
"var camVisible=(localStorage.getItem('camVisible')!=='false');"
"function updateTimeFmtLabel(){var b=document.getElementById('timeFmt');if(b)b.innerText=use24h?'24h':'12h';}"
"function toggleTimeFormat(){use24h=!use24h;localStorage.setItem('timeFormat24h',use24h);updateTimeFmtLabel();fetchData();}"
"function updateCamVisibility(){var w=document.getElementById('camWrapper');var b=document.getElementById('btnToggleCam');if(camVisible){w.style.display='block';b.innerText='📷 Hide Cam';reloadStream();}else{w.style.display='none';b.innerText='📷 Show Cam';var img=document.getElementById('stream');if(img)img.src='';}}"
"function toggleCamera(){camVisible=!camVisible;localStorage.setItem('camVisible',camVisible);updateCamVisibility();}"
"function toggleMgmt(){var m=document.getElementById('mgmtBox');m.style.display=(m.style.display==='block')?'none':'block';}"
"function reloadStream(){if(!activeIp||!camVisible)return;var img=document.getElementById('stream');img.src='http://'+activeIp+':8080/?action=stream&t='+new Date().getTime();}"
"function takeSnapshot(){if(!activeIp)return;var a=document.createElement('a');a.href='http://'+activeIp+':8080/?action=snapshot&t='+new Date().getTime();a.download='ad5x_snap_'+Date.now()+'.jpg';a.target='_blank';document.body.appendChild(a);a.click();document.body.removeChild(a);}"
"function toggleFullscreen(){var el=document.getElementById('camFrame');if(!document.fullscreenElement){if(el.requestFullscreen)el.requestFullscreen();else if(el.webkitRequestFullscreen)el.webkitRequestFullscreen();}else{if(document.exitFullscreen)document.exitFullscreen();}}"
"function switchPrinter(idx){fetch('/api/printers/select?idx='+idx,{method:'POST'}).then(function(){loadPrinters(true);fetchData();});}"
"function addPrinter(){var n=document.getElementById('newName').value.trim();var ip=document.getElementById('newIp').value.trim();if(!n||!ip){alert('Enter Name and IP');return;}fetch('/api/printers/add?name='+encodeURIComponent(n)+'&ip='+encodeURIComponent(ip),{method:'POST'}).then(function(){document.getElementById('newName').value='';document.getElementById('newIp').value='';loadPrinters(true);});}"
"function deletePrinter(idx){if(confirm('Delete this printer?')){fetch('/api/printers/delete?idx='+idx,{method:'POST'}).then(function(){loadPrinters(true);});}}"
"function loadPrinters(refreshFeed){fetch('/api/printers').then(function(r){return r.json();}).then(function(d){var sel=document.getElementById('printerSelect');var c=document.getElementById('printerListContainer');sel.innerHTML='';c.innerHTML='';if(!d.printers||d.printers.length===0){document.getElementById('mgmtBox').style.display='block';activeIp='';var opt=document.createElement('option');opt.text='-- Add Printer in Manage --';sel.appendChild(opt);c.innerHTML='<div style=\"color:#94a3b8;padding:4px 0;\">No printers saved yet.</div>';var img=document.getElementById('stream');if(img)img.src='';return;}if(d.active<0||d.active>=d.printers.length){d.active=0;switchPrinter(0);}d.printers.forEach(function(p,i){var opt=document.createElement('option');opt.value=i;opt.text=p.name+' ('+p.ip+')';if(i===d.active){opt.selected=true;activeIp=p.ip;}sel.appendChild(opt);var item=document.createElement('div');item.className='p-list-item';item.innerHTML='<span><b>'+p.name+'</b> - '+p.ip+'</span>';var delBtn=document.createElement('button');delBtn.className='btn-sm';delBtn.style.background='#991b1b';delBtn.innerText='Del';delBtn.onclick=function(){deletePrinter(i);};item.appendChild(delBtn);c.appendChild(item);});if(refreshFeed||!document.getElementById('stream').src){reloadStream();}});}"
"function sendAction(act){fetch('/api/cmd?action='+act,{method:'POST'}).then(function(){setTimeout(fetchData,600);});}"
"function findPrinter(){var b=document.getElementById('btnFind');b.innerText='✨ Flashing...';sendAction('flash_lights');setTimeout(function(){b.innerText='💡 Flash Lights';},3000);}"
"function togglePauseResume(){var t=(currentStatus==='Paused')?'resume_print':'pause_print';sendAction(t);}"
"function cancelPrint(){if(confirm('Stop and cancel current print?')){sendAction('cancel_print');}}"
"function fetchData(){fetch('/api/status').then(function(r){return r.json();}).then(function(d){currentStatus=d.status;document.getElementById('status').innerText=d.status;document.getElementById('file').innerText=d.file;document.getElementById('progress').innerText=d.progress+'%';document.getElementById('layer').innerText=d.layer+' / '+d.total_layers;document.getElementById('nozzle').innerText=d.nozzle+'\\u00B0C';document.getElementById('bed').innerText=d.bed+'\\u00B0C';document.getElementById('light').innerText=d.light?'on':'off';var now=new Date();document.getElementById('updated').innerText=now.toLocaleTimeString([],{hour12:!use24h});var ledBtn=document.getElementById('btnLed');if(d.light){ledBtn.classList.add('btn-active');}else{ledBtn.classList.remove('btn-active');}var partBtn=document.getElementById('btnPartFan');if(d.part_fan){partBtn.classList.add('btn-active');partBtn.innerText='Aux Fan: ON';}else{partBtn.classList.remove('btn-active');partBtn.innerText='Aux Fan: OFF';}var chBtn=document.getElementById('btnChamberFan');if(d.chamber_fan){chBtn.classList.add('btn-active');chBtn.innerText='Filter Fan: ON';}else{chBtn.classList.remove('btn-active');chBtn.innerText='Filter Fan: OFF';}var p=document.getElementById('btnPause');if(d.status==='Paused'){p.innerText='Resume Print';}else{p.innerText='Pause Print';}}).catch(function(){document.getElementById('status').innerText='Offline';});}"
"updateTimeFmtLabel();updateCamVisibility();loadPrinters(true);setInterval(fetchData,3000);fetchData();"
"</script></body></html>";

void setup() {
  Serial.begin(115200);
  delay(2000);

  printerMutex = xSemaphoreCreateMutex();
  tcpMutex = xSemaphoreCreateMutex();
  loadPrintersFromNVS();

  Serial.println("\n[AD5X-VIEW] Starting Station...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  if (USE_STATIC_IP) {
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("[WARN] Static IP configuration failed, using DHCP");
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
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
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

  server.on("/api/printers/select", HTTP_POST, [](){
    if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
        if (idx >= 0 && idx < (int)printerList.size()) {
          activePrinterIndex = idx;
          savePrintersToNVS();
        }
        xSemaphoreGive(printerMutex);
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/printers/add", HTTP_POST, [](){
    if (server.hasArg("name") && server.hasArg("ip")) {
      String n = server.arg("name");
      String ip = server.arg("ip");
      if (n.length() > 0 && ip.length() > 0) {
        if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
          printerList.push_back({n, ip});
          activePrinterIndex = printerList.size() - 1;
          savePrintersToNVS();
          xSemaphoreGive(printerMutex);
        }
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/printers/delete", HTTP_POST, [](){
    if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
        if (idx >= 0 && idx < (int)printerList.size()) {
          printerList.erase(printerList.begin() + idx);
          if (printerList.empty()) {
            activePrinterIndex = -1;
          } else {
            activePrinterIndex = 0;
          }
          savePrintersToNVS();
        }
        xSemaphoreGive(printerMutex);
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  // Telemetry status endpoint
  server.on("/api/status", HTTP_GET, [](){
    DynamicJsonDocument doc(512);
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
      doc["status"] = printer.status;
      doc["file"] = printer.filename;
      doc["progress"] = printer.progress;
      doc["layer"] = printer.current_layer;
      doc["total_layers"] = printer.total_layers;
      doc["nozzle"] = printer.nozzle_temp;
      doc["bed"] = printer.bed_temp;
      doc["light"] = printer.light_on;
      doc["part_fan"] = printer.part_fan;
      doc["chamber_fan"] = printer.chamber_fan;
      xSemaphoreGive(printerMutex);
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // Action Router
  server.on("/api/cmd", HTTP_POST, [](){
    if (server.hasArg("action")) {
      String act = server.arg("action");
      bool ok = false;

      if (act == "toggle_light") {
        ok = sendPrinterTcpCommand("~M146\r\n");
        if (ok && xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
          printer.light_on = !printer.light_on;
          xSemaphoreGive(printerMutex);
        }
      }
      else if (act == "flash_lights") {
        std::vector<String> flashCmds;
        for (int i = 0; i < 4; i++) {
          flashCmds.push_back("~M146\r\n");
          flashCmds.push_back("~M146\r\n");
        }
        ok = sendPrinterTcpCommands(flashCmds);
      }
      else if (act == "pause_print") ok = sendPrinterTcpCommand("~M25\r\n");
      else if (act == "resume_print") ok = sendPrinterTcpCommand("~M24\r\n");
      else if (act == "cancel_print") ok = sendPrinterTcpCommand("~M26\r\n");
      else if (act == "cooldown") {
        std::vector<String> cmds = {"~M104 S0\r\n", "~M140 S0\r\n"};
        ok = sendPrinterTcpCommands(cmds);
      }
      else if (act == "preheat_pla") {
        std::vector<String> cmds = {"~M104 S210\r\n", "~M140 S60\r\n"};
        ok = sendPrinterTcpCommands(cmds);
      }
      else if (act == "preheat_petg") {
        std::vector<String> cmds = {"~M104 S240\r\n", "~M140 S80\r\n"};
        ok = sendPrinterTcpCommands(cmds);
      }
      else if (act == "bed_soak") ok = sendPrinterTcpCommand("~M140 S60\r\n");
      else if (act == "home") ok = sendPrinterTcpCommand("~G28\r\n");
      else if (act == "lower_bed") {
        std::vector<String> cmds = {"~G91\r\n", "~G1 Z50 F1200\r\n", "~G90\r\n"};
        ok = sendPrinterTcpCommands(cmds);
      }
      else if (act == "center_head") {
        std::vector<String> cmds = {"~G90\r\n", "~G1 X110 Y110 Z50 F3600\r\n"};
        ok = sendPrinterTcpCommands(cmds);
      }
      else if (act == "motors_off") ok = sendPrinterTcpCommand("~M84\r\n");
      else if (act == "toggle_fan") {
        bool nextState = !printer.part_fan;
        ok = sendPrinterTcpCommand(nextState ? "~M106 P1 S255\r\n" : "~M107 P1\r\n");
        if (ok && xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
          printer.part_fan = nextState;
          xSemaphoreGive(printerMutex);
        }
      }
      else if (act == "toggle_chamber_fan") {
        bool nextState = !printer.chamber_fan;
        ok = sendPrinterTcpCommand(nextState ? "~M106 P2 S255\r\n" : "~M107 P2\r\n");
        if (ok && xSemaphoreTake(printerMutex, pdMS_TO_TICKS(200))) {
          printer.chamber_fan = nextState;
          xSemaphoreGive(printerMutex);
        }
      }

      if (ok) {
        server.send(200, "application/json", "{\"success\":true}");
        return;
      }
      server.send(500, "application/json", "{\"error\":\"Command execution failed\"}");
      return;
    }
    server.send(400, "application/json", "{\"error\":\"Missing action\"}");
  });

  server.begin();
  Serial.println("[OK] HTTP Server Running!");

  // Background telemetry poller on Core 0
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