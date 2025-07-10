/**
 * antenna_mqtt_handler.cpp – MQTT helpers for antenna control
 * ------------------------------------------------------------------
 *  • Caches the latest “…/available” JSON (availableDoc)
 *  • Offers natural alphanumeric sorting: A1 < A2 < A10 < A-BLAH
 *  • Provides quick look-ups: listAntennasForBand("40m") → "A1-40 A3-40"
 *  • Updates globals + publishes concise debug lines on band change
 */

#include "antenna_mqtt_handler.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <algorithm>
#include <ctype.h>
#include <map>
#include <vector>

// --- External Global Variables (from main sketch) ---
// MQTT client instance.
extern PubSubClient client;
// Current radio state ("RX" or "TX").
extern char currentRXTX[4];
// List of antennas for the current band.
extern std::vector<String> currentAntList;
// Currently selected radio band.
extern String currentBand;
// Space-separated list of antennas for the current band.
extern String currentAntennas;
// Cache for band name -> sorted antenna list string.
extern std::map<String, String> bandCache;
// Flag indicating if the radio is currently transmitting (PTT active).
extern bool g_txActive;
// Structure to hold a pending TX antenna change.
extern PendingTxChange g_pendingTx;
// Unique name for this station.
extern char station_name[];

// --- External Debug Helper ---
// Function to publish debug messages (defined in main sketch).
extern void publishDebugMessage(const char* msg);

// --- Global Caches (defined here) ---
// DynamicJsonDocument to store the parsed JSON from the ".../available" MQTT topic.
DynamicJsonDocument availableDoc(4096);
// Map to store the BandState for each band (band name -> BandState).
std::map<String, BandState> g_bandStates;

// -----------------------------------------------------------------
//  naturalLess()
//  Helper function for natural alphanumeric comparison of strings.
//  (e.g., "A1" < "A2" < "A10").
// -----------------------------------------------------------------
static bool naturalLess(const String& a, const String& b)
{
    const char* pa = a.c_str();
    const char* pb = b.c_str();

    while (*pa || *pb) {
        // Skip non-alphanumeric characters like '-' or '_'.
        while (*pa && !isalnum(*pa)) ++pa;
        while (*pb && !isalnum(*pb)) ++pb;

        if (!*pa || !*pb) break; // One string ended.

        // If both are numeric chunks, compare as numbers.
        if (isdigit(*pa) && isdigit(*pb)) {
            long na = strtol(pa, (char**)&pa, 10);
            long nb = strtol(pb, (char**)&pb, 10);
            if (na != nb) return na < nb;
        }
        // Otherwise, compare as alphabetic characters (case-insensitive).
        else {
            char ca = tolower(*pa++);
            char cb = tolower(*pb++);
            if (ca != cb) return ca < cb;
        }
    }
    // Shorter string (after skipping separators) comes first.
    return *pa == '\0' && *pb != '\0';
}

// -----------------------------------------------------------------
//  antennaRank()
//  Assigns a rank to antenna names for custom sorting order.
//  Specific antennas (LOAD, A-HORLOOP, A-BEV, A-INB) have higher ranks.
// -----------------------------------------------------------------
static int antennaRank(const String& s)
{
    if (s.startsWith(F("LOAD")))      return 99; // Always last.
    if (s.startsWith(F("A-HORLOOP"))) return 1;
    if (s.startsWith(F("A-BEV")))     return 2;
    if (s.startsWith(F("A-INB")))     return 3;
    return 0;                                     // Default rank for others.
}

// -----------------------------------------------------------------
//  antennaLess()
//  Custom comparison function for sorting antenna names.
//  Sorts by rank first, then by natural alphanumeric order.
// -----------------------------------------------------------------
static bool antennaLess(const String& a, const String& b)
{
    int ra = antennaRank(a);
    int rb = antennaRank(b);
    if (ra != rb) return ra < rb; // Compare by rank first.
    return naturalLess(a, b);     // Then by natural alphanumeric order.
}

// -----------------------------------------------------------------
//  getCurrentAntList()
//  Splits the global `currentAntennas` string (e.g., "A1-40 A3-40 …")
//  into individual tokens and returns them as a `std::vector<String>`.
// -----------------------------------------------------------------
std::vector<String> getCurrentAntList()
{
    std::vector<String> list;
    int pos = 0;
    while (pos < currentAntennas.length()) {
        int sp = currentAntennas.indexOf(' ', pos);
        if (sp == -1) sp = currentAntennas.length();
        String tok = currentAntennas.substring(pos, sp);
        tok.trim();
        if (tok.length()) list.push_back(tok);
        pos = sp + 1;
    }
    return list; // Returns example: {"A1-40", "A3-40", …}
}

// -----------------------------------------------------------------
//  listAntennasForBand()
//  Builds a (cached) space-separated antenna list for a given band.
//  Collects antennas from `availableDoc`, sorts them, and caches the result.
// -----------------------------------------------------------------
String listAntennasForBand(const char* band)
{
    // Check if the list is already in cache.
    auto it = bandCache.find(band);
    if (it != bandCache.end()) return it->second; // Cache hit.

    // Collect and sort antenna names for the given band.
    std::vector<String> names;
    for (JsonPair kv : availableDoc.as<JsonObject>()) {
        JsonObject map = kv.value();
        if (map.containsKey(band)) names.emplace_back(kv.key().c_str());
    }
    std::sort(names.begin(), names.end(), antennaLess);

    // Join sorted names into a single space-separated string.
    String out;
    for (const auto& n : names) { out += n; out += ' '; }
    out.trim();

    bandCache[band] = out; // Store in cache.
    return out;
}

// -----------------------------------------------------------------
//  handleBandStateJSON()
//  Handles JSON messages from the "…/sta/<sta>/b/<Band>" topic.
//  Updates the `g_bandStates` map with the current RX/TX antennas for the band.
// -----------------------------------------------------------------
void handleBandStateJSON(const char* band, const char* json)
{
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json)) return; // Bad JSON, ignore.

    // Get or create the BandState entry for this band.
    BandState& st = g_bandStates[band];

    // Extract the first RXANTENNAS item or set to empty.
    if (doc["RXANTENNAS"].is<JsonArray>() && doc["RXANTENNAS"].size() > 0)
        st.rx = doc["RXANTENNAS"][0].as<const char*>();
    else
        st.rx = "";

    // Extract the first TXANTENNAS item or set to empty.
    if (doc["TXANTENNAS"].is<JsonArray>() && doc["TXANTENNAS"].size() > 0)
        st.tx = doc["TXANTENNAS"][0].as<const char*>();
    else
        st.tx = "";

    // Keep the global `currentBand` in sync for UI consistency.
    extern String currentBand;
    currentBand = band;
}

// -----------------------------------------------------------------
//  handleAvailableJSON()
//  Handles JSON messages from the "…/sta/<station>/available" topic.
//  Updates the `availableDoc` cache and prints a pretty log to Serial.
// -----------------------------------------------------------------
void handleAvailableJSON(const char* json)
{
    availableDoc.clear(); // Clear previous availability data.
    bandCache.clear();    // Clear band cache as availability has changed.

    if (deserializeJson(availableDoc, json)) {
        publishDebugMessage("[handleAvailableJSON] ❌ JSON parse error");
        return;
    }

    // Pretty print for Serial monitor (optional).
    std::vector<String> ants;
    for (JsonPair kv : availableDoc.as<JsonObject>())
        ants.emplace_back(kv.key().c_str());
    std::sort(ants.begin(), ants.end(), naturalLess);

    Serial.println(F("Parsed antenna availability:"));
    for (const String& ant : ants) {
        Serial.printf("  Antenna %s\n", ant.c_str());

        JsonObject bandMap = availableDoc[ant];
        std::vector<String> bands;
        for (JsonPair kv : bandMap) bands.emplace_back(kv.key().c_str());
        std::sort(bands.begin(), bands.end(), naturalLess);

        for (const String& band : bands) {
            Serial.printf("    Band %s → modes: ", band.c_str());
            for (const char* m : bandMap[band].as<JsonArray>())
                Serial.printf("%s ", m);
            Serial.println();
        }
    }
}

// -----------------------------------------------------------------
//  handleCurrentBandJSON()
//  Handles JSON messages from the legacy "…/dt/<station>/current" topic.
//  Updates current band, antenna list, and handles queued TX changes.
// -----------------------------------------------------------------
void handleCurrentBandJSON(const char* json)
{
    static String   lastBand = "?";
    static uint32_t lastHash = 0; // djb2-xor hash of antenna list for deduplication.

    // Parse the status JSON.
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, json)) return; // Bad JSON, ignore.

    extern bool g_txActive;
    const char* liveState = doc["RXTX"] | ""; // Field sent by MatriGS.
    bool newTxActive = (strcmp(liveState, "TX") == 0);

    // If we just fell back to RX from TX, flush any queued command.
    extern PendingTxChange g_pendingTx;
    if (g_txActive && !newTxActive && g_pendingTx.valid) {

        // Build /p:remove + /p:add topics exactly like in handleSerialCommands.
        char topicR[128], topicA[128];
        snprintf(topicR, sizeof(topicR),
                 "matrigs/0/sta/%s/b/%s/p:remove/TXANTENNAS",
                 station_name, g_pendingTx.band.c_str());
        snprintf(topicA, sizeof(topicA),
                 "matrigs/0/sta/%s/b/%s/p:add/TXANTENNAS",
                 station_name, g_pendingTx.band.c_str());

        client.publish(topicR, g_pendingTx.oldAnt.c_str());
        client.publish(topicA, g_pendingTx.newAnt.c_str());

        publishDebugMessage("[TX-Queue] ▶ executed queued TX change");
        g_pendingTx.valid = false; // Clear queue.
    }
    g_txActive = newTxActive; // Remember current PTT state.

    const char* band = doc["BANDS"] | "?";
    String ants      = listAntennasForBand(band); // Get sorted antennas for the band.

    // Update `currentRXTX` if `TARGET` field is present.
    if (doc.containsKey("TARGET")) {
        const char* rxtx = doc["TARGET"]["RXTX"] | "";
        strncpy(currentRXTX, rxtx, sizeof(currentRXTX) - 1);
    }

    // Deduplicate: recompute hash of antenna string.
    uint32_t hash = 5381;
    for (char c : ants) hash = ((hash << 5) + hash) ^ c; // djb2-xor hash.

    if (lastBand == band && lastHash == hash) return; // Nothing new, return.
    lastBand = band;
    lastHash = hash;

    // Publish concise debug line.
    char dbg[192];
    snprintf(dbg, sizeof(dbg), "[BandChange] %s | antennas: %s",
             band, ants.c_str());
    publishDebugMessage(dbg);

    // Update global state variables.
    currentBand     = band;
    currentAntennas = ants;
    currentAntList  = getCurrentAntList(); // Tokenize once.
}
