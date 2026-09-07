#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>
#include <HTTPClient.h>
#include "config.h"

const uint16_t printer_tcp_port = 8899;

struct PrinterEntry {
  String name;
  String ip;
};

std::vector<PrinterEntry> printerList;
int activePrinterIndex = -1;
Preferences prefs;
SemaphoreHandle_t printerMutex;

WebServer server(80);

struct PrinterData {
  String status = "Connecting...";
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

void savePrintersToNVS();

void loadPrintersFromNVS() {
  prefs.begin("ad5x_view", false);

  if (FORCE_RESET_PRINTERS) {
    prefs.clear();
  }

  String json = prefs.getString("printers", "");
  activePrinterIndex = prefs.getInt("active_idx", -1);

  printerList.clear();
  if (json.length() > 0) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, json) == DeserializationError::Ok && doc.is<JsonArray>()) {
      for (JsonObject obj : doc.as<JsonArray>()) {
        printerList.push_back({obj["name"].as<String>(), obj["ip"].as<String>()});
      }
    }
  }

  if (printerList.empty()) {
    for (const auto &dp : DEFAULT_PRINTERS) {
      printerList.push_back({String(dp.name), String(dp.ip)});
    }
    if (!printerList.empty()) {
      activePrinterIndex = 0;
      savePrintersToNVS();
    }
  }

  if (printerList.empty()) {
    activePrinterIndex = -1;
  } else if (activePrinterIndex >= (int)printerList.size() || activePrinterIndex < 0) {
    activePrinterIndex = 0;
  }
}

void savePrintersToNVS() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("printers");
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

String getActiveIp() {
  String ip = "";
  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
    if (activePrinterIndex >= 0 && activePrinterIndex < (int)printerList.size()) {
      ip = printerList[activePrinterIndex].ip;
    }
    xSemaphoreGive(printerMutex);
  }
  return ip;
}

// Robust command sender that handles multi-line G-code sequentially
bool sendFastTcpCommand(const char* cmdStr) {
  String targetIp = getActiveIp();
  if (targetIp.length() == 0) return false;

  WiFiClient client;
  client.setTimeout(500);
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 1000)) {
    return false;
  }

  client.print("~M601 S1\r\n");
  delay(30);

  String multiCmd = String(cmdStr);
  int startIdx = 0;
  while (startIdx < multiCmd.length()) {
    int endIdx = multiCmd.indexOf('\n', startIdx);
    if (endIdx == -1) endIdx = multiCmd.length();
    String singleLine = multiCmd.substring(startIdx, endIdx);
    singleLine.trim();
    if (singleLine.length() > 0) {
      client.print(singleLine + "\r\n");
      delay(40);
    }
    startIdx = endIdx + 1;
  }

  delay(50);
  client.stop();
  return true;
}

void updatePrinterTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return;

  String targetIp = getActiveIp();
  if (targetIp.length() == 0) return;

  WiFiClient client;
  client.setTimeout(500);
  if (!client.connect(targetIp.c_str(), printer_tcp_port, 800)) {
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
      printer.status = "Offline";
      xSemaphoreGive(printerMutex);
    }
    return;
  }

  client.print("~M601 S1\r\n~M119\r\n");
  delay(30);
  String statusResp = "";
  while (client.available()) {
    statusResp += (char)client.read();
  }

  String parsedStatus = "Online";
  if (statusResp.indexOf("READY") != -1 || statusResp.indexOf("IDLE") != -1) parsedStatus = "Idle";
  else if (statusResp.indexOf("BUILDING") != -1 || statusResp.indexOf("PRINTING") != -1) parsedStatus = "Printing";
  else if (statusResp.indexOf("PAUSED") != -1) parsedStatus = "Paused";
  else if (statusResp.indexOf("COMPLETE") != -1) parsedStatus = "Completed";

  // Parse Layer info if present in status response
  int curLayer = 0, totLayer = 0;
  int layerIdx = statusResp.indexOf("layer:");
  if (layerIdx == -1) layerIdx = statusResp.indexOf("Layer:");
  if (layerIdx != -1) {
    curLayer = statusResp.substring(layerIdx + 6).toInt();
  }

  client.print("~M105\r\n");
  delay(30);
  String tempResp = "";
  while (client.available()) {
    tempResp += (char)client.read();
  }

  float nozzle = 0.0, bed = 0.0;
  int tIndex = tempResp.indexOf("T0:");
  if (tIndex != -1) nozzle = tempResp.substring(tIndex + 3, tempResp.indexOf(" ", tIndex)).toFloat();
  int bIndex = tempResp.indexOf("B:");
  if (bIndex != -1) bed = tempResp.substring(bIndex + 2, tempResp.indexOf(" ", bIndex)).toFloat();

  client.print("~M27\r\n");
  delay(30);
  String progResp = "";
  while (client.available()) {
    progResp += (char)client.read();
  }

  int progress = 0;
  int byteIdx = progResp.indexOf("byte ");
  int slashIdx = progResp.indexOf("/", byteIdx);
  if (byteIdx != -1 && slashIdx != -1) {
    long curBytes = progResp.substring(byteIdx + 5, slashIdx).toInt();
    long totBytes = progResp.substring(slashIdx + 1).toInt();
    if (totBytes > 0) progress = (int)((curBytes * 100) / totBytes);
  }

  if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
    printer.status = parsedStatus;
    printer.nozzle_temp = nozzle;
    printer.bed_temp = bed;
    printer.progress = progress;
    printer.current_layer = curLayer;
    printer.total_layers = totLayer;
    printer.last_update = millis();
    xSemaphoreGive(printerMutex);
  }

  client.stop();
}

void telemetryTask(void *pvParameters) {
  for (;;) {
    updatePrinterTelemetry();
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

TaskHandle_t streamTaskHandle = NULL;
String fileToStream = "";

void streamPrintTask(void *pvParameters) {
  String fileName = fileToStream;
  String fileUrl = "http://" + String(UNRAID_SERVER_IP) + ":" + String(NGINX_PORT) + "/" + fileName;
  String targetIp = getActiveIp();
  
  if (targetIp.length() > 0) {
    WiFiClient printClient;
    HTTPClient http;

    if (printClient.connect(targetIp.c_str(), printer_tcp_port)) {
      http.begin(fileUrl);
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
        int fileSize = http.getSize();
        
        printClient.print("~M601 S1\r\n");
        vTaskDelay(pdMS_TO_TICKS(50));

        char m28Command[100];
        sprintf(m28Command, "~M28 %d 0:/user/%s\r\n", fileSize, fileName.c_str());
        printClient.print(m28Command);
        vTaskDelay(pdMS_TO_TICKS(50));

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[1024]; 
        int bytesRemaining = fileSize;

        while (http.connected() && bytesRemaining > 0) {
          size_t size = stream->available();
          if (size) {
            int bytesToRead = (size > sizeof(buffer)) ? sizeof(buffer) : size;
            int bytesRead = stream->readBytes(buffer, bytesToRead);
            printClient.write(buffer, bytesRead);
            bytesRemaining -= bytesRead;
          }
          vTaskDelay(pdMS_TO_TICKS(1)); 
        }

        printClient.print("~M29\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        
        char m23Command[100];
        sprintf(m23Command, "~M23 0:/user/%s\r\n", fileName.c_str());
        printClient.print(m23Command);
        vTaskDelay(pdMS_TO_TICKS(100));

        printClient.print("~M24\r\n");
      }
      http.end();
      printClient.stop();
    }
  }
  
  streamTaskHandle = NULL;
  vTaskDelete(NULL);
}

static const char INDEX_HTML[] PROGMEM = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>"
"<title>AD5X Monitor</title><style>"
":root{--bg-body:#0f172a;--bg-card:#1e293b;--btn-bg:#334155;--btn-hover:#475569;--btn-warn:#b45309;--btn-warn-hover:#d97706;--btn-danger:#991b1b;--btn-danger-hover:#dc2626;--btn-success:#16a34a;--btn-blue:#2563eb;--btn-blue-hover:#1d4ed8;--text-main:#f8fafc;--text-muted:#94a3b8;--border-color:#334155;}"
"body{background-color:var(--bg-body);color:var(--text-main);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;padding:16px;box-sizing:border-box;}"
".card{background-color:var(--bg-card);border:1px solid var(--border-color);border-radius:12px;width:100%;max-width:440px;padding:18px;box-shadow:0 10px 25px rgba(0,0,0,0.5);box-sizing:border-box;transition:border-color 0.3s,box-shadow 0.3s;}"
".card.hot-border{border-color:#ef4444 !important;box-shadow:0 0 20px rgba(239,68,68,0.4) !important;}"
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
".temp-btn.active-preset, .ifs-btn.active-preset{background-color:var(--btn-blue) !important;color:#ffffff !important;box-shadow:0 0 8px rgba(37,99,235,0.6);}"
"button.warn{background:var(--btn-warn);}button.warn:hover{background:var(--btn-warn-hover);}"
"button.danger{background:var(--btn-danger);}button.danger:hover{background:var(--btn-danger-hover);}"
"button.blue{background:var(--btn-blue);}button.blue:hover{background:var(--btn-blue-hover);}"
".sec-label{font-size:0.75rem;font-weight:700;color:var(--text-muted);text-transform:uppercase;letter-spacing:0.5px;margin:10px 0 4px 0;}"
".row{display:flex;justify-content:space-between;margin-bottom:5px;font-size:0.85rem;}"
".row.col{flex-direction:column;gap:3px;}"
".label{color:var(--text-muted);}.val{font-weight:600;}"
".hot-warn{color:#ef4444 !important;font-weight:700;text-shadow:0 0 8px rgba(239,68,68,0.5);}"
".file-box{word-break:break-all;background:#0f172a;padding:5px;border-radius:4px;border:1px solid var(--border-color);font-size:0.75rem;}"
".footer{display:flex;justify-content:flex-end;align-items:center;margin-top:12px;padding-top:8px;border-top:1px solid var(--border-color);font-size:0.75rem;color:var(--text-muted);}"
".pill-btn{cursor:pointer;margin-left:6px;padding:2px 6px;background:var(--btn-bg);color:#f8fafc;border-radius:4px;font-size:0.7rem;font-weight:600;user-select:none;}"
".mgmt-panel{display:none;background:#0f172a;border:1px solid var(--border-color);border-radius:8px;padding:12px;margin-bottom:12px;}"
".mgmt-panel input{width:100%;padding:7px;margin-bottom:6px;background:#1e293b;border:1px solid var(--border-color);border-radius:4px;color:#fff;box-sizing:border-box;}"
".p-list-item{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid #1e293b;font-size:0.8rem;}"
".print-row{display:flex;gap:4px;margin-bottom:12px;align-items:center;}"
".temp-input{width:100%;background:#0f172a;border:1px solid var(--border-color);border-radius:4px;color:#fff;padding:4px;text-align:center;font-size:0.8rem;font-weight:600;}"
"</style></head><body><div class='card' id='mainCard'>"
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

"<div class='sec-label'>Network Print</div>"
"<div class='print-row'>"
"<select id='streamFileSelect' style='flex:1;min-width:0;padding:7px;border-radius:4px;border:1px solid var(--border-color);background:#1e293b;color:#fff;'></select>"
"<button onclick='loadFileList()' class='btn-sm' style='padding:7px 9px;' title='Refresh List'>🔄</button>"
"<button onclick='triggerStream()' class='blue' style='padding:7px 11px;'>Print</button>"
"<button onclick='deleteFile()' class='danger' style='padding:7px 11px;'>Del</button>"
"</div>"

"<div class='sec-label'>Filament Slot (IFS)</div>"
"<div class='btn-grid-4'>"
"<button id='btn_slot0' class='ifs-btn' onclick='setActiveSlot(0); selectSlot(0)'>Slot 1</button>"
"<button id='btn_slot1' class='ifs-btn' onclick='setActiveSlot(1); selectSlot(1)'>Slot 2</button>"
"<button id='btn_slot2' class='ifs-btn' onclick='setActiveSlot(2); selectSlot(2)'>Slot 3</button>"
"<button id='btn_slot3' class='ifs-btn' onclick='setActiveSlot(3); selectSlot(3)'>Slot 4</button>"
"</div>"

"<div class='sec-label'>Temperatures & Presets</div>"
"<div class='btn-grid-4'>"
"<button id='btn_cooldown' class='temp-btn' onclick='setActiveTemp(\"cooldown\"); sendAction(\"cooldown\")'>Cool Off</button>"
"<button id='btn_PLA' class='temp-btn' onclick='setActiveTemp(\"PLA\"); applyPreset(\"PLA\")'>PLA</button>"
"<button id='btn_PETG' class='temp-btn' onclick='setActiveTemp(\"PETG\"); applyPreset(\"PETG\")'>PETG</button>"
"<button id='btn_ABS' class='temp-btn' onclick='setActiveTemp(\"ABS\"); applyPreset(\"ABS\")'>ABS</button>"
"</div>"
"<div style='background:#0f172a;border:1px solid var(--border-color);border-radius:6px;padding:8px;margin-bottom:12px;'>"
"<div style='font-size:0.75rem;font-weight:700;color:var(--text-muted);margin-bottom:6px;'>Customize Preset Targets (&deg;C)</div>"
"<div style='display:grid;grid-template-columns:repeat(3,1fr);gap:6px;text-align:center;font-size:0.7rem;'>"
"<div>PLA<br><input id='pla_n' class='temp-input' value='210' onchange='savePresetVal(\"PLA\")'><input id='pla_b' class='temp-input' value='60' style='margin-top:2px;' onchange='savePresetVal(\"PLA\")'></div>"
"<div>PETG<br><input id='petg_n' class='temp-input' value='240' onchange='savePresetVal(\"PETG\")'><input id='petg_b' class='temp-input' value='80' style='margin-top:2px;' onchange='savePresetVal(\"PETG\")'></div>"
"<div>ABS<br><input id='abs_n' class='temp-input' value='260' onchange='savePresetVal(\"ABS\")'><input id='abs_b' class='temp-input' value='100' style='margin-top:2px;' onchange='savePresetVal(\"ABS\")'></div>"
"</div></div>"

"<div class='sec-label'>Core Controls</div>"
"<div class='btn-grid-2'>"
"<button id='btnLed' onclick='sendAction(\"toggle_light\")'>Toggle LEDs</button>"
"<button id='btnPause' class='warn' onclick='togglePauseResume()'>Pause Print</button>"
"<button class='danger' onclick='cancelPrint()'>Cancel Print</button>"
"<button onclick='sendAction(\"beep\")'>🔊 Find Printer</button>"
"</div>"
"<div class='sec-label'>Motion & Fans</div>"
"<div class='btn-grid-4'>"
"<button onclick='sendAction(\"home\")'>Home (G28)</button>"
"<button onclick='sendAction(\"lower_bed\")'>Lower Bed</button>"
"<button onclick='sendAction(\"center_head\")'>Center</button>"
"<button onclick='sendAction(\"motors_off\")'>Unlock</button>"
"</div>"
"<div class='btn-grid-2' style='margin-bottom:10px;'>"
"<button onclick='sendAction(\"fan_off\")'>Kill Fan (0%)</button>"
"<button onclick='sendAction(\"fan_half\")'>Fan 50%</button>"
"</div>"
"<div class='row'><span class='label'>Status:</span><span id='status' class='val'>--</span></div>"
"<div class='row col'><span class='label'>File:</span><div id='file' class='file-box'>--</div></div>"
"<div class='row'><span class='label'>Progress:</span><span id='progress' class='val'>--%</span></div>"
"<div class='row'><span class='label'>Layer:</span><span id='layer' class='val'>-- / --</span></div>"
"<div class='row'><span class='label'>Nozzle:</span><span id='nozzle' class='val'>--&deg;C</span></div>"
"<div class='row'><span class='label'>Bed:</span><span id='bed' class='val'>--&deg;C</span></div>"
"<div class='footer'>Updated:&nbsp;<span id='updated'>--</span><span id='timeFmt' class='pill-btn' onclick='toggleTimeFormat()'>12h</span></div>"
"</div><script>"
"var activeIp='';"
"var use24h=(localStorage.getItem('timeFormat24h')==='true');"
"var camVisible=(localStorage.getItem('camVisible')!=='false');"

"function setActiveTemp(name){"
"  ['cooldown','PLA','PETG','ABS'].forEach(function(k){"
"    var b=document.getElementById('btn_'+k);"
"    if(b) b.classList.remove('active-preset');"
"  });"
"  var activeBtn=document.getElementById('btn_'+name);"
"  if(activeBtn) activeBtn.classList.add('active-preset');"
"  localStorage.setItem('activeTempPreset',name);"
"}"

"function setActiveSlot(slot){"
"  [0,1,2,3].forEach(function(s){"
"    var b=document.getElementById('btn_slot'+s);"
"    if(b) b.classList.remove('active-preset');"
"  });"
"  var activeBtn=document.getElementById('btn_slot'+slot);"
"  if(activeBtn) activeBtn.classList.add('active-preset');"
"  localStorage.setItem('activeIfsSlot',slot);"
"}"

"function loadPresets(){"
"  ['pla','petg','abs'].forEach(function(k){"
"    var n=localStorage.getItem(k+'_n'); var b=localStorage.getItem(k+'_b');"
"    if(n) document.getElementById(k+'_n').value=n;"
"    if(b) document.getElementById(k+'_b').value=b;"
"  });"
"  var savedTemp=localStorage.getItem('activeTempPreset')||'cooldown';"
"  setActiveTemp(savedTemp);"
"  var savedSlot=localStorage.getItem('activeIfsSlot');"
"  if(savedSlot!==null){ setActiveSlot(parseInt(savedSlot)); }"
"}"

"function savePresetVal(k){"
"  var n=document.getElementById(k.toLowerCase()+'_n').value;"
"  var b=document.getElementById(k.toLowerCase()+'_b').value;"
"  localStorage.setItem(k.toLowerCase()+'_n',n);"
"  localStorage.setItem(k.toLowerCase()+'_b',b);"
"}"

"function applyPreset(k){"
"  var n=document.getElementById(k.toLowerCase()+'_n').value;"
"  var b=document.getElementById(k.toLowerCase()+'_b').value;"
"  fetch('/api/cmd?action=custom_temp&n='+n+'&b='+b,{method:'POST'}).then(function(){setTimeout(fetchData,600);});"
"}"

"function selectSlot(slot){"
"  fetch('/api/cmd?action=slot&val='+slot,{method:'POST'}).then(function(){setTimeout(fetchData,600);});"
"}"

"function loadFileList(){"
"  fetch('http://192.168.1.5:8739/').then(function(r){return r.json();}).then(function(d){"
"    var sel=document.getElementById('streamFileSelect');"
"    sel.innerHTML='';"
"    d.forEach(function(f){"
"      if(f.type==='file' && f.name.endsWith('.gcode')){"
"        var opt=document.createElement('option');"
"        opt.value=f.name; opt.text=f.name;"
"        sel.appendChild(opt);"
"      }"
"    });"
"  }).catch(function(){console.log('Failed to fetch file list');});"
"}"

"function deleteFile(){"
"  var fn=document.getElementById('streamFileSelect').value;"
"  if(!fn) return;"
"  if(confirm('Permanently delete '+fn+' from Unraid?')){"
"    fetch('http://192.168.1.5:8739/'+encodeURIComponent(fn), {method:'DELETE'})"
"    .then(function(r){"
"      if(r.ok){ alert('Deleted!'); loadFileList(); }"
"      else alert('Delete failed.');"
"    });"
"  }"
"}"

"function triggerStream(){"
"  var fn = document.getElementById('streamFileSelect').value;"
"  if(!fn){alert('No file selected');return;}"
"  if(confirm('Ready to send '+fn+' to the printer and start printing?')){"
"    fetch('/api/stream_file?file='+encodeURIComponent(fn),{method:'POST'}).then(function(r){"
"      if(r.ok) alert('File stream initiated! Printer will begin shortly.');"
"      else alert('Stream failed. Check if a transfer is already running.');"
"    });"
"  }"
"}"

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
"function loadPrinters(refreshFeed){fetch('/api/printers').then(function(r){return r.json();}).then(function(d){var sel=document.getElementById('printerSelect');var c=document.getElementById('printerListContainer');sel.innerHTML='';c.innerHTML='';if(!d.printers||d.printers.length===0){document.getElementById('mgmtBox').style.display='block';activeIp='';var opt=document.createElement('option');opt.text='-- Add Printer in Manage --';sel.appendChild(opt);c.innerHTML='<div style=\"color:#94a3b8;padding:4px 0;\">No printers saved yet.</div>';var img=document.getElementById('stream');if(img)img.src='';return;}d.printers.forEach(function(p,i){var opt=document.createElement('option');opt.value=i;opt.text=p.name+' ('+p.ip+')';if(i===d.active){opt.selected=true;activeIp=p.ip;}sel.appendChild(opt);var item=document.createElement('div');item.className='p-list-item';item.innerHTML='<span><b>'+p.name+'</b> - '+p.ip+'</span>';var delBtn=document.createElement('button');delBtn.className='btn-sm';delBtn.style.background='#991b1b';delBtn.innerText='Del';delBtn.onclick=function(){deletePrinter(i);};item.appendChild(delBtn);c.appendChild(item);});if(refreshFeed||!document.getElementById('stream').src){reloadStream();}});}"
"function sendAction(act){fetch('/api/cmd?action='+act,{method:'POST'}).then(function(){setTimeout(fetchData,600);});}"
"function togglePauseResume(){sendAction('pause_print');}"
"function cancelPrint(){if(confirm('Stop and cancel current print?')){sendAction('cancel_print');}}"
"function fetchData(){"
"  fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
"    document.getElementById('status').innerText=d.status;"
"    document.getElementById('file').innerText=d.file;"
"    document.getElementById('progress').innerText=d.progress+'%';"
"    document.getElementById('layer').innerText=d.layer+' / '+d.total_layers;"
"    "
"    var nz=document.getElementById('nozzle');"
"    nz.innerText=d.nozzle+'\\u00B0C';"
"    if(d.nozzle>=50){ nz.classList.add('hot-warn'); } else { nz.classList.remove('hot-warn'); }"
"    "
"    var bd=document.getElementById('bed');"
"    bd.innerText=d.bed+'\\u00B0C';"
"    if(d.bed>=50){ bd.classList.add('hot-warn'); } else { bd.classList.remove('hot-warn'); }"
"    "
"    var card=document.getElementById('mainCard');"
"    if(d.nozzle>=50 || d.bed>=50){ card.classList.add('hot-border'); } else { card.classList.remove('hot-border'); }"
"    "
"    var now=new Date();"
"    document.getElementById('updated').innerText=now.toLocaleTimeString([],{hour12:!use24h});"
"  }).catch(function(){document.getElementById('status').innerText='Offline';});"
"}"
"loadPresets();updateTimeFmtLabel();updateCamVisibility();loadPrinters(true);loadFileList();setInterval(fetchData,3000);fetchData();"
"</script></body></html>";

void setup() {
  Serial.begin(115200);
  delay(2000);

  printerMutex = xSemaphoreCreateMutex();
  loadPrintersFromNVS();

  Serial.println("\n[AD5X-VIEW] Starting Station...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  if (USE_STATIC_IP) {
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("[WARN] Static IP configuration failed, falling back to DHCP");
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

  server.on("/", HTTP_GET, [](){
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/printers", HTTP_GET, [](){
    DynamicJsonDocument doc(2048);
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
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
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
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
        if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
          printerList.push_back({n, ip});
          if (activePrinterIndex < 0) {
            activePrinterIndex = 0;
          }
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
      if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
        if (idx >= 0 && idx < (int)printerList.size()) {
          printerList.erase(printerList.begin() + idx);
          if (printerList.empty()) {
            activePrinterIndex = -1;
          } else if (activePrinterIndex >= (int)printerList.size()) {
            activePrinterIndex = printerList.size() - 1;
          }
          savePrintersToNVS();
        }
        xSemaphoreGive(printerMutex);
      }
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/status", HTTP_GET, [](){
    DynamicJsonDocument doc(512);
    if (xSemaphoreTake(printerMutex, pdMS_TO_TICKS(100))) {
      doc["status"] = printer.status;
      doc["file"] = printer.filename;
      doc["progress"] = printer.progress;
      doc["layer"] = printer.current_layer;
      doc["total_layers"] = printer.total_layers;
      doc["nozzle"] = printer.nozzle_temp;
      doc["bed"] = printer.bed_temp;
      xSemaphoreGive(printerMutex);
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  server.on("/api/stream_file", HTTP_POST, [](){
    if (streamTaskHandle != NULL) {
      server.send(400, "application/json", "{\"error\":\"Streaming already in progress\"}");
      return;
    }
    if (server.hasArg("file")) {
      fileToStream = server.arg("file");
      xTaskCreatePinnedToCore(streamPrintTask, "StreamTask", 8192, NULL, 1, &streamTaskHandle, 1);
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"Missing filename\"}");
    }
  });

  server.on("/api/cmd", HTTP_POST, [](){
    if (server.hasArg("action")) {
      String act = server.arg("action");
      bool ok = false;

      if (act == "toggle_light") ok = sendFastTcpCommand("~M146\r\n");
      else if (act == "pause_print") ok = sendFastTcpCommand("~M25\r\n");
      else if (act == "resume_print") ok = sendFastTcpCommand("~M24\r\n");
      else if (act == "cancel_print") ok = sendFastTcpCommand("~M26\r\n");
      else if (act == "cooldown") ok = sendFastTcpCommand("~M104 S0 T0\r\n~M140 S0\r\n");
      else if (act == "slot") {
        if (server.hasArg("val")) {
          int slot = server.arg("val").toInt();
          char slotCmd[20];
          sprintf(slotCmd, "T%d\r\n", slot);
          ok = sendFastTcpCommand(slotCmd);
        }
      }
      else if (act == "custom_temp") {
        if (server.hasArg("n") && server.hasArg("b")) {
          int nTemp = server.arg("n").toInt();
          int bTemp = server.arg("b").toInt();
          char tempCmd[60];
          sprintf(tempCmd, "~M104 S%d T0\r\n~M140 S%d\r\n", nTemp, bTemp);
          ok = sendFastTcpCommand(tempCmd);
        }
      }
      else if (act == "home") ok = sendFastTcpCommand("~G28\r\n");
      else if (act == "lower_bed") ok = sendFastTcpCommand("~G91\r\n~G1 Z50 F1200\r\n~G90\r\n");
      else if (act == "center_head") ok = sendFastTcpCommand("~G90\r\n~G1 X110 Y110 Z50 F3600\r\n");
      else if (act == "motors_off") ok = sendFastTcpCommand("~M84\r\n");
      else if (act == "beep") ok = sendFastTcpCommand("~M300 S1000 P400\r\n");
      else if (act == "fan_off") ok = sendFastTcpCommand("~M107\r\n");
      else if (act == "fan_half") ok = sendFastTcpCommand("~M106 S128\r\n");

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