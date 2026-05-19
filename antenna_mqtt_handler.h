#ifndef ANTENNA_MQTT_HANDLER_H
#define ANTENNA_MQTT_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>                      // NEW

/* ---------- data structures ------------------------------------ */
struct BandState {                  // NEW
    std::vector<String> rx;         // RXANTENNAS lineup
    std::vector<String> tx;         // TXANTENNAS lineup
};

/* ---------- global caches -------------------------------------- */
extern DynamicJsonDocument availableDoc;            // unchanged
extern std::map<String, BandState> g_bandStates;    // NEW  (band ➜ state)

/* ---------- MQTT-callback helpers ------------------------------ */
void handleAvailableJSON(const char* json);         // “…/sta/<station>/available”
void handleStationStateJSON(const char* json);      // “…/sta/<station>”
void handleCurrentBandJSON(const char* json);       // legacy “…/dt/<station>/current”
void handleBandStateJSON(const char* band,
                          const char* json);        // NEW “…/sta/<sta>/b/<Band>”
void serviceAntennaMqttTasks();

/* ---------- antenna helpers ------------------------------------ */
std::vector<String> getCurrentAntList();            // returns {"A1-40", …}
String listAntennasForBand(const char* band);       // "B80" → "A1-80 A3-80 …"
bool publishMatrigsAntennaSetCommand(const char* band,
                                      const char* bank,
                                      const char* oldAnt,
                                      const char* newAnt,
                                      bool* usedFallback,
                                      char* error,
                                      size_t errorLen);
bool publishMatrigsAntennaLineupCommand(const char* band,
                                         const char* bank,
                                         const std::vector<String>& oldLineup,
                                         const std::vector<String>& newLineup,
                                         bool* usedFallback,
                                         char* error,
                                         size_t errorLen);

/* ---------------- shared TX-queue support -------------------- */
struct PendingTxChange {
    String band;
    std::vector<String> oldLineup;
    std::vector<String> newLineup;
    bool valid = false;
};

extern bool            g_txActive;   // true while PTT is active
extern PendingTxChange g_pendingTx;  // one queued TX change
extern char            station_name[];

#endif  // ANTENNA_MQTT_HANDLER_H
