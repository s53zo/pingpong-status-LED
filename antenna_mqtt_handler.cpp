/**
 * antenna_mqtt_handler.cpp  –  MQTT helpers for antenna control
 * ------------------------------------------------------------------
 *  • Caches the latest “…/available” JSON (availableDoc)
 *  • Offers natural alphanumeric sorting:  A1 < A2 < A10 < A-BLAH
 *  • Provides quick look-ups:  listAntennasForBand("40m") → "A1-40 A3-40"
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

/* ------------------------------------------------------------------
 *  Globals declared in the main sketch
 * ---------------------------------------------------------------- */
extern PubSubClient              client;
extern char                      currentRXTX[4];
extern std::vector<String>       currentAntList;
extern String                    currentBand;
extern String                    currentAntennas;
extern std::map<String, String>  bandCache;
extern bool            g_txActive;
extern PendingTxChange g_pendingTx;
extern char            station_name[];


/* Publish-to-MQTT debug helper (implemented in the sketch) */
extern void publishDebugMessage(const char* msg);

/* Live antenna map cache (≈ 1 – 2 kB for your data set) */
DynamicJsonDocument availableDoc(4096);

/* ── global: band ➜ {rx,tx} cache ─────────────────────────────── */
std::map<String, BandState> g_bandStates; 





/* ================================================================
 *  Helper: natural alphanumeric compare ("A1" < "A2" < "A10")
 * ===============================================================*/
static bool naturalLess(const String& a, const String& b)
{
    const char* pa = a.c_str();
    const char* pb = b.c_str();

    while (*pa || *pb) {
        /* skip non-alphanumerics like '-' or '_' */
        while (*pa && !isalnum(*pa)) ++pa;
        while (*pb && !isalnum(*pb)) ++pb;

        if (!*pa || !*pb) break;           // one string ended

        /* numeric chunk? */
        if (isdigit(*pa) && isdigit(*pb)) {
            long na = strtol(pa, (char**)&pa, 10);
            long nb = strtol(pb, (char**)&pb, 10);
            if (na != nb) return na < nb;
        }
        /* alphabetic char */
        else {
            char ca = tolower(*pa++);
            char cb = tolower(*pb++);
            if (ca != cb) return ca < cb;
        }
    }
    /* shorter string (after skipping separators) comes first */
    return *pa == '\0' && *pb != '\0';
}

/* ------------------------------------------------------------------
 *  custom sort order
 *   0 : “A1-xxx”, “A3-xxx”, …  (all normal alphanumeric IDs)
 *   1 : A-HORLOOP
 *   2 : A-BEV…
 *  99 : LOAD-2KA   (always last)
 *  Within each bucket we keep the old naturalLess order.
 * -----------------------------------------------------------------*/
static int antennaRank(const String& s)
{
    if (s.startsWith(F("LOAD")))        return 99;     // last
    if (s.startsWith(F("A-HORLOOP")))   return 1;
    if (s.startsWith(F("A-BEV")))       return 2;
    if (s.startsWith(F("A-INB")))       return 3;
    return 0;                                           // default
}

static bool antennaLess(const String& a, const String& b)
{
    int ra = antennaRank(a);
    int rb = antennaRank(b);
    if (ra != rb) return ra < rb;                       // rank first
    return naturalLess(a, b);                           // then A1<A3<A10…
}

/* ================================================================
 *  Split the global currentAntennas ("A1-40 A3-40 …") into tokens
 *  and return them as std::vector<String>.
 * ===============================================================*/
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
    return list;                          // {"A1-40", "A3-40", …}
}

/* ================================================================
 *  Build a (cached) space-separated antenna list for <band>
 * ===============================================================*/
String listAntennasForBand(const char* band)
{
    auto it = bandCache.find(band);
    if (it != bandCache.end()) return it->second;   // cache hit

    /* collect & sort */
    std::vector<String> names;
    for (JsonPair kv : availableDoc.as<JsonObject>()) {
        JsonObject map = kv.value();
        if (map.containsKey(band)) names.emplace_back(kv.key().c_str());
    }
    std::sort(names.begin(), names.end(), antennaLess);

    /* join into one string */
    String out;
    for (const auto& n : names) { out += n; out += ' '; }
    out.trim();

    bandCache[band] = out;   // store in cache
    return out;
}

/* ── function: handle “…/sta/<sta>/b/<Band>” JSON  -------------- */
void handleBandStateJSON(const char* band, const char* json)       // NEW
{
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json)) return;          // bad JSON → ignore

    BandState& st = g_bandStates[band];              // create or fetch slot

    /* grab first element or reset to empty */
    if (doc["RXANTENNAS"].is<JsonArray>() && doc["RXANTENNAS"].size() > 0)
        st.rx = doc["RXANTENNAS"][0].as<const char*>();
    else
        st.rx = "";

    if (doc["TXANTENNAS"].is<JsonArray>() && doc["TXANTENNAS"].size() > 0)
        st.tx = doc["TXANTENNAS"][0].as<const char*>();
    else
        st.tx = "";

    /* keep the public “currentBand” in sync so the UI stays sane */
    extern String currentBand;
    currentBand = band;
}

/* ================================================================
 *  “…/sta/<station>/available”  →  update cache + pretty log
 * ===============================================================*/
void handleAvailableJSON(const char* json)
{
    availableDoc.clear();
    bandCache.clear();

    if (deserializeJson(availableDoc, json)) {
        publishDebugMessage("[handleAvailableJSON] ❌ JSON parse error");
        return;
    }

    /* pretty print for Serial monitor (optional) */
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

/* ================================================================
 *  “…/dt/<station>/current” handler (band + target RX/TX)
 * ===============================================================*/
void handleCurrentBandJSON(const char* json)
{
    static String   lastBand = "?";
    static uint32_t lastHash = 0;   // djb2-xor of antenna list
    static bool     warnedNoBand = false;

    /* parse the tiny status JSON */
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, json)) return;       // bad JSON → ignore

    extern bool g_txActive;                   // add extern near top
    const char* liveState = doc["RXTX"] | ""; // field sent by MatriGS
    bool newTxActive = (strcmp(liveState, "TX") == 0);

    /* if we just fell back to RX, flush any queued command ---- */
    extern PendingTxChange g_pendingTx;
    if (g_txActive && !newTxActive && g_pendingTx.valid) {

        /* build /p:remove + /p:add exactly like in handleSerialCommands */
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
        g_pendingTx.valid = false;            // clear queue
    }
    g_txActive = newTxActive;                 // remember current PTT state

    /* update currentRXTX if TARGET present */
    if (doc.containsKey("TARGET")) {
        const char* rxtx = doc["TARGET"]["RXTX"] | "";
        strncpy(currentRXTX, rxtx, sizeof(currentRXTX) - 1);
    }

    const char* band = doc["BANDS"] | "?";

    // When MatriGS reports OFFLINE it may send BANDS="-" (no current band).
    // Keep the last valid `currentBand` (often set via retained per-band state)
    // so keypad selection and UI remain usable.
    if (!band || !band[0] || strcmp(band, "?") == 0 || strcmp(band, "-") == 0) {
        if (!warnedNoBand) {
            publishDebugMessage("[BandChange] BANDS not set (\"-\") - keeping last known band");
            warnedNoBand = true;
        }
        return;
    }
    warnedNoBand = false;

    String ants      = listAntennasForBand(band); // sorted antennas

    /* dedup: recompute hash of antenna string */
    uint32_t hash = 5381;
    for (char c : ants) hash = ((hash << 5) + hash) ^ c;  // djb2-xor

    if (lastBand == band && lastHash == hash) return;     // nothing new
    lastBand = band;
    lastHash = hash;

    /* concise debug line */
    char dbg[192];
    snprintf(dbg, sizeof(dbg), "[BandChange] %s | antennas: %s",
             band, ants.c_str());
    publishDebugMessage(dbg);

    /* update globals */
    currentBand     = band;
    currentAntennas = ants;
    currentAntList  = getCurrentAntList();   // << tokenise once
}
