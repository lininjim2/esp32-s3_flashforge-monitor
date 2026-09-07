#pragma once

#include <vector>

// --- Wi-Fi Credentials (2.4GHz Only) ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// --- Network Setup ---
const bool USE_STATIC_IP = false; // Set to true if using a static IP
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 254);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 1, 254);
IPAddress secondaryDNS(1, 1, 1, 1);

// --- Server Configuration ---
const char* UNRAID_SERVER_IP = "192.168.1.X";
const uint16_t NGINX_PORT = 8739;

// --- Default Printers ---
struct ConfigPrinter {
  const char* name;
  const char* ip;
};

const std::vector<ConfigPrinter> DEFAULT_PRINTERS = {
  { "My Flashforge AD5X", "192.168.1.X" }
};

// Set to true ONCE to force-clear old saved printers, then change back to false
const bool FORCE_RESET_PRINTERS = false; 

// --- Default Filament Presets ---
struct FilamentPreset {
  const char* name;
  int nozzle;
  int bed;
};

const std::vector<FilamentPreset> DEFAULT_PRESETS = {
  { "PLA", 210, 60 },
  { "PETG", 240, 80 },
  { "ABS", 260, 100 }
};