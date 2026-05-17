#ifndef ANTENNA_MQTT_HANDLER_H
#define ANTENNA_MQTT_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>                      // NEW

/* ---------- data structures ------------------------------------ */
struct BandState {                  // NEW
    String rx;                      // first (or only) RXANTENNAS item
    String tx;                      // first (or only) TXANTENNAS item
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

/* ---------- antenna helpers ------------------------------------ */
std::vector<String> getCurrentAntList();            // returns {"A1-40", …}
String listAntennasForBand(const char* band);       // "B80" → "A1-80 A3-80 …"

/* ---------------- shared TX-queue support -------------------- */
struct PendingTxChange {
    String band;
    String oldAnt;
    String newAnt;
    bool   valid;
};

extern bool            g_txActive;   // true while PTT is active
extern PendingTxChange g_pendingTx;  // one queued TX change
extern char            station_name[];

#endif  // ANTENNA_MQTT_HANDLER_H
