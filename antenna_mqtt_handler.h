#ifndef ANTENNA_MQTT_HANDLER_H
#define ANTENNA_MQTT_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>

// --- Data Structures ---
// Represents the RX and TX antenna states for a specific band.
struct BandState {
    String rx; // The currently selected RX antenna for this band.
    String tx; // The currently selected TX antenna for this band.
};

// --- Global Caches (declared extern as they are defined in .cpp) ---
// DynamicJsonDocument to store the parsed JSON from the ".../available" MQTT topic.
extern DynamicJsonDocument availableDoc;
// Map to store the BandState for each band (band name -> BandState).
extern std::map<String, BandState> g_bandStates;

// --- MQTT Callback Handlers ---
// Handles messages from the ".../sta/<station>/available" topic.
void handleAvailableJSON(const char* json);
// Handles messages from the legacy ".../dt/<station>/current" topic.
void handleCurrentBandJSON(const char* json);
// Handles messages from the new ".../sta/<sta>/b/<Band>" topic.
void handleBandStateJSON(const char* band,
                          const char* json);

// --- Antenna Helper Functions ---
// Splits the `currentAntennas` string into a vector of individual antenna names.
std::vector<String> getCurrentAntList();
// Returns a space-separated string of sorted antennas available for a given band.
String listAntennasForBand(const char* band);

// --- Shared TX-Queue Support ---
// Structure to hold a pending TX antenna change, used for safe switching.
struct PendingTxChange {
    String band;   // The band for which the change is pending.
    String oldAnt; // The antenna that was active before the pending change.
    String newAnt; // The antenna to switch to once TX is inactive.
    bool   valid;  // Flag indicating if there is a valid pending change.
};

// Flag indicating if the radio is currently transmitting (PTT active).
extern bool g_txActive;
// One queued TX change, used to prevent hot-switching during transmit.
extern PendingTxChange g_pendingTx;
// The station name, used for constructing MQTT topics.
extern char station_name[];

#endif // ANTENNA_MQTT_HANDLER_H
