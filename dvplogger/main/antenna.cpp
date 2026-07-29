/*
 * DVPlogger antenna allocation and OTRSP/TCP client
 *
 * DVPlogger is the controller.  It allocates one exclusive antenna to each
 * radio from per-band preference rows, waits until every radio is in RX, and
 * then sends AUX1/AUX2 commands to an external OTRSP peripheral over TCP.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "decl.h"
#include "variables.h"
#include "antenna.h"
#include "log.h"

int antenna_control_enable = 0;
char antenna_host[64] = "192.168.8.120";
int antenna_port = 12001;
char antenna_pref[ANTENNA_PREF_ROWS][N_BAND + 1] = {
  "111222344456",
  "000777000000",
  ""
};
char antenna_name[ANTENNA_MAX_ID][24] = {
  "Triband dipole", "Tribander", "50MHz 2el",
  "144/430/1200 GP", "2.4GHz antenna", "5.6GHz antenna",
  "Second triband DP", "Antenna 8", "Antenna 9"
};

namespace {
WiFiClient otrsp_client;

enum AntState {
  ANT_DISABLED,
  ANT_DISCONNECTED,
  ANT_READY,
  ANT_WAIT_RX,
  ANT_RX_SETTLE,
  ANT_SENDING,
  ANT_ERROR
};

AntState state = ANT_DISABLED;
int current_ant[ANTENNA_RADIOS] = {-1, -1};
int requested_ant[ANTENNA_RADIOS] = {0, 0};
int requested_pref[ANTENNA_RADIOS] = {0, 0};
int blocked_by[ANTENNA_RADIOS] = {-1, -1};
int last_band[ANTENNA_RADIOS] = {-1, -1};
uint32_t next_service_at = 0;
uint32_t next_connect_at = 0;
constexpr int OTRSP_CONNECT_TIMEOUT_MS = 100;
constexpr uint32_t OTRSP_RETRY_INTERVAL_MS = 5000;
uint32_t rx_since = 0;
bool force_resend = true;
char reason_text[80] = "Disabled";

int decode_ant(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  return 0;
}

const char *state_text(AntState s) {
  switch (s) {
  case ANT_DISABLED: return "DISABLED";
  case ANT_DISCONNECTED: return "DISCONNECTED";
  case ANT_READY: return "READY";
  case ANT_WAIT_RX: return "WAIT_RX";
  case ANT_RX_SETTLE: return "RX_SETTLE";
  case ANT_SENDING: return "SENDING";
  case ANT_ERROR: return "ERROR";
  }
  return "UNKNOWN";
}

bool any_radio_tx(int *tx_radio) {
  for (int i = 0; i < N_RADIO; ++i) {
    if (!radio_list[i].enabled) continue;
    if (radio_list[i].ptt != 0 || radio_list[i].ptt_stat != 0) {
      if (tx_radio) *tx_radio = i;
      return true;
    }
  }
  return false;
}

int preference_for_band(int row, int bandid) {
  if (row < 0 || row >= ANTENNA_PREF_ROWS) return 0;
  if (bandid < 1 || bandid >= N_BAND) return 0;
  const size_t idx = (size_t)(bandid - 1);
  const size_t len = strlen(antenna_pref[row]);
  if (idx >= len) return 0;
  const int ant = decode_ant(antenna_pref[row][idx]);
  return (ant >= 1 && ant <= ANTENNA_MAX_ID) ? ant : 0;
}

void calculate_requested() {
  int used_by[ANTENNA_MAX_ID + 1];
  for (int i = 0; i <= ANTENNA_MAX_ID; ++i) used_by[i] = -1;

  for (int radio = 0; radio < ANTENNA_RADIOS; ++radio) {
    requested_ant[radio] = 0;
    requested_pref[radio] = 0;
    blocked_by[radio] = -1;
    last_band[radio] = radio_list[radio].bandid;

    if (!radio_list[radio].enabled) continue;
    const int bandid = radio_list[radio].bandid;
    if (bandid < 1 || bandid >= N_BAND) continue;

    int first_blocker = -1;
    for (int row = 0; row < ANTENNA_PREF_ROWS; ++row) {
      const int ant = preference_for_band(row, bandid);
      if (ant == 0) continue;
      if (used_by[ant] < 0) {
        requested_ant[radio] = ant;
        requested_pref[radio] = row + 1;
        blocked_by[radio] = first_blocker;
        used_by[ant] = radio;
        break;
      }
      if (first_blocker < 0) first_blocker = used_by[ant];
    }
    if (requested_ant[radio] == 0) blocked_by[radio] = first_blocker;
  }
}

bool allocation_changed() {
  for (int i = 0; i < ANTENNA_RADIOS; ++i) {
    if (current_ant[i] != requested_ant[i]) return true;
  }
  return false;
}

bool connect_otrsp() {
  if (otrsp_client.connected()) return true;
  otrsp_client.stop();
  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(reason_text, "Wi-Fi disconnected", sizeof(reason_text));
    return false;
  }
  if (!antenna_host[0] || antenna_port <= 0 || antenna_port > 65535) {
    strlcpy(reason_text, "Invalid OTRSP host/port", sizeof(reason_text));
    return false;
  }
  const uint32_t connect_started = millis();
  if (!otrsp_client.connect(antenna_host, (uint16_t)antenna_port, OTRSP_CONNECT_TIMEOUT_MS)) {
    const uint32_t elapsed = millis() - connect_started;
    snprintf(reason_text, sizeof(reason_text),
             "OTRSP connect failed (%lu ms)", (unsigned long)elapsed);
    if (elapsed > (uint32_t)(OTRSP_CONNECT_TIMEOUT_MS + 50)) {
      Serial.printf("ANTENNA connect slow host=%s port=%d elapsed=%lu ms timeout=%d ms\n",
                    antenna_host, antenna_port, (unsigned long)elapsed,
                    OTRSP_CONNECT_TIMEOUT_MS);
    }
    return false;
  }
  otrsp_client.setNoDelay(true);
  force_resend = true;
  strlcpy(reason_text, "Connected; synchronizing", sizeof(reason_text));
  return true;
}

bool send_allocation() {
  char cmd[24];
  for (int i = 0; i < ANTENNA_RADIOS; ++i) {
    const int n = snprintf(cmd, sizeof(cmd), "AUX%d %d\r", i + 1, requested_ant[i]);
    if (n <= 0 || n >= (int)sizeof(cmd)) return false;
    const size_t written = otrsp_client.write((const uint8_t *)cmd, (size_t)n);
    if (written != (size_t)n) return false;
  }
  otrsp_client.flush();
  for (int i = 0; i < ANTENNA_RADIOS; ++i) {
    current_ant[i] = requested_ant[i];
    radio_list[i].antenna = current_ant[i];
  }
  force_resend = false;
  return true;
}

String json_escape(const char *src) {
  String out;
  if (!src) return out;
  while (*src) {
    const char c = *src++;
    if (c == '\\' || c == '"') out += '\\';
    if ((unsigned char)c >= 0x20) out += c;
  }
  return out;
}

const char *ant_name(int ant) {
  if (ant < 1 || ant > ANTENNA_MAX_ID) return "None";
  return antenna_name[ant - 1][0] ? antenna_name[ant - 1] : "Unnamed";
}

const char *radio_status(int radio) {
  if (!radio_list[radio].enabled) return "Disabled";
  if (radio_list[radio].ptt != 0 || radio_list[radio].ptt_stat != 0) return "TX";
  if (state == ANT_DISCONNECTED || state == ANT_ERROR) return "Disconnected";
  if (current_ant[radio] != requested_ant[radio]) return "Waiting";
  if (requested_ant[radio] == 0) return "Disabled";
  return "Ready";
}
} // namespace

void antenna_settings_changed() {
  force_resend = true;
  next_connect_at = 0;
  otrsp_client.stop();
}

void antenna_force_resend() {
  force_resend = true;
}

const char *antenna_controller_state() {
  return state_text(state);
}

void antenna_process() {
  const uint32_t now = millis();
  if ((int32_t)(now - next_service_at) < 0) return;
  next_service_at = now + 100;

  calculate_requested();

  if (!antenna_control_enable) {
    if (otrsp_client.connected()) otrsp_client.stop();
    state = ANT_DISABLED;
    strlcpy(reason_text, "Antenna control disabled", sizeof(reason_text));
    return;
  }

  if (!otrsp_client.connected()) {
    state = ANT_DISCONNECTED;
    if ((int32_t)(now - next_connect_at) >= 0) {
      connect_otrsp();
      next_connect_at = millis() + OTRSP_RETRY_INTERVAL_MS;
    }
    if (!otrsp_client.connected()) return;
  }

  if (!allocation_changed() && !force_resend) {
    state = ANT_READY;
    strlcpy(reason_text, "Current allocation active", sizeof(reason_text));
    rx_since = 0;
    return;
  }

  int tx_radio = -1;
  if (any_radio_tx(&tx_radio)) {
    state = ANT_WAIT_RX;
    rx_since = 0;
    snprintf(reason_text, sizeof(reason_text), "Radio %d transmitting", tx_radio);
    return;
  }

  if (rx_since == 0) {
    rx_since = now;
    state = ANT_RX_SETTLE;
    strlcpy(reason_text, "Waiting 150 ms after all radios RX", sizeof(reason_text));
    return;
  }
  if ((uint32_t)(now - rx_since) < 150) {
    state = ANT_RX_SETTLE;
    return;
  }

  state = ANT_SENDING;
  strlcpy(reason_text, "Sending OTRSP antenna allocation", sizeof(reason_text));
  if (!send_allocation()) {
    state = ANT_ERROR;
    strlcpy(reason_text, "OTRSP write failed", sizeof(reason_text));
    otrsp_client.stop();
    next_connect_at = millis() + OTRSP_RETRY_INTERVAL_MS;
    rx_since = 0;
    return;
  }
  state = ANT_READY;
  strlcpy(reason_text, "Current allocation active", sizeof(reason_text));
  rx_since = 0;
}

String antenna_status_json() {
  String s;
  s.reserve(1200);
  s += F("{\"controller\":\"OTRSP/TCP\",\"connection\":\"");
  s += otrsp_client.connected() ? F("Connected") : F("Disconnected");
  s += F("\",\"host\":\"");
  s += json_escape(antenna_host);
  s += ':';
  s += String(antenna_port);
  s += F("\",\"state\":\"");
  s += state_text(state);
  s += F("\",\"reason\":\"");
  s += json_escape(reason_text);
  s += F("\",\"pending\":");
  s += (allocation_changed() || force_resend) ? F("true") : F("false");
  s += F(",\"radios\":[");
  for (int i = 0; i < ANTENNA_RADIOS; ++i) {
    if (i) s += ',';
    s += F("{\"radio\":"); s += String(i);
    s += F(",\"band\":\"");
    if (radio_list[i].bandid >= 1 && radio_list[i].bandid < N_BAND)
      s += band_str[radio_list[i].bandid - 1];
    else
      s += '-';
    s += F(" MHz\",\"current\":"); s += String(current_ant[i]);
    s += F(",\"currentName\":\""); s += json_escape(ant_name(current_ant[i]));
    s += F("\",\"requested\":"); s += String(requested_ant[i]);
    s += F(",\"requestedName\":\""); s += json_escape(ant_name(requested_ant[i]));
    s += F("\",\"pref\":"); s += String(requested_pref[i]);
    s += F(",\"blockedBy\":"); s += String(blocked_by[i]);
    s += F(",\"status\":\""); s += radio_status(i); s += F("\"}");
  }
  s += F("]}");
  return s;
}
