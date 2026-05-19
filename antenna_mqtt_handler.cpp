/**
 * antenna_mqtt_handler.cpp  –  MQTT helpers for antenna control
 * ------------------------------------------------------------------
 *  • Caches the latest “…/available” JSON (availableDoc)
 *  • Offers natural alphanumeric sorting:  A1 < A2 < A10 < A-BLAH
 *  • Provides quick look-ups:  listAntennasForBand("40m") → "A1-40 A3-40"
 *  • Updates globals + publishes concise debug lines on band change
 */

#include "antenna_mqtt_handler.h"
#include "mqtt5_json_publisher.h"

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
extern PubSubClient              clientMatrigs;
extern char                      currentRXTX[4];
extern std::vector<String>       currentAntList;
extern String                    currentBand;
extern String                    currentAntennas;
extern std::map<String, String>  bandCache;
extern bool            g_txActive;
extern PendingTxChange g_pendingTx;
extern char            station_name[];
extern char            matrigs_server[];
extern int             matrigs_port;
extern char            macAddress[];


/* Publish-to-MQTT debug helper (implemented in the sketch) */
extern void publishDebugMessage(const char* msg);

/* Live antenna map cache (≈ 1 – 2 kB for your data set) */
DynamicJsonDocument availableDoc(4096);

/* ── global: band ➜ {rx,tx} cache ─────────────────────────────── */
std::map<String, BandState> g_bandStates; 

static PendingTxChange s_deferredTxChange = { "", "", "", false };
static uint32_t s_lastDeferredTxAttemptMs = 0;
static uint32_t s_lastMqtt5FailureMs = 0;
static constexpr uint32_t DEFERRED_TX_RETRY_MS = 2000;
static constexpr uint32_t MQTT5_RETRY_DELAY_MS = 10000;

static String makeSingleAntennaJsonArray(const char* antenna)
{
    String payload;
    payload.reserve(strlen(antenna) + 6);
    payload += F("[\"");
    for (const char* p = antenna; *p; ++p) {
        if (*p == '"' || *p == '\\') payload += '\\';
        payload += *p;
    }
    payload += F("\"]");
    return payload;
}

static bool publishRemoveAddFallback(const char* band,
                                     const char* bank,
                                     const char* oldAnt,
                                     const char* newAnt)
{
    if (!clientMatrigs.connected()) return false;

    bool ok = true;
    if (oldAnt && oldAnt[0]) {
        char topicR[160];
        snprintf(topicR, sizeof(topicR),
                 "matrigs/0/sta/%s/b/%s/p:remove/%sANTENNAS",
                 station_name, band, bank);
        ok = clientMatrigs.publish(topicR, oldAnt) && ok;
    }

    char topicA[160];
    snprintf(topicA, sizeof(topicA),
             "matrigs/0/sta/%s/b/%s/p:add/%sANTENNAS",
             station_name, band, bank);
    ok = clientMatrigs.publish(topicA, newAnt) && ok;

    return ok;
}

bool publishMatrigsAntennaSetCommand(const char* band,
                                     const char* bank,
                                     const char* oldAnt,
                                     const char* newAnt,
                                     bool* usedFallback,
                                     char* error,
                                     size_t errorLen)
{
    if (usedFallback) *usedFallback = false;
    if (error && errorLen) error[0] = '\0';

    if (!band || !band[0] || !bank || !bank[0] || !newAnt || !newAnt[0]) {
        if (error && errorLen)
            snprintf(error, errorLen, "missing antenna set argument");
        return false;
    }

    char topic[160];
    snprintf(topic, sizeof(topic),
             "matrigs/0/sta/%s/b/%s/p:set/%sANTENNAS",
             station_name, band, bank);

    char clientId[64];
    snprintf(clientId, sizeof(clientId), "PingPong-MGS5-%s", macAddress);

    String payload = makeSingleAntennaJsonArray(newAnt);
    char mqtt5Error[128] = "";
    bool shouldTryMqtt5 =
        s_lastMqtt5FailureMs == 0 ||
        static_cast<uint32_t>(millis() - s_lastMqtt5FailureMs) >= MQTT5_RETRY_DELAY_MS ||
        !clientMatrigs.connected();

    if (shouldTryMqtt5) {
        if (mqtt5PublishJson(matrigs_server, static_cast<uint16_t>(matrigs_port),
                             clientId, topic, payload.c_str(),
                             mqtt5Error, sizeof(mqtt5Error))) {
            s_lastMqtt5FailureMs = 0;
            return true;
        }
        s_lastMqtt5FailureMs = millis();
    } else {
        snprintf(mqtt5Error, sizeof(mqtt5Error),
                 "suppressed after recent MQTT5 failure");
    }

    if (usedFallback) *usedFallback = true;
    if (publishRemoveAddFallback(band, bank, oldAnt, newAnt)) {
        if (error && errorLen)
            snprintf(error, errorLen, "MQTT5 failed: %s; used remove/add fallback",
                     mqtt5Error);
        return true;
    }

    if (error && errorLen)
        snprintf(error, errorLen, "MQTT5 failed: %s; remove/add fallback failed",
                 mqtt5Error);
    return false;
}

void serviceAntennaMqttTasks()
{
    if (!s_deferredTxChange.valid) return;

    uint32_t now = millis();
    if (s_lastDeferredTxAttemptMs &&
        static_cast<uint32_t>(now - s_lastDeferredTxAttemptMs) < DEFERRED_TX_RETRY_MS) {
        return;
    }
    s_lastDeferredTxAttemptMs = now;

    bool usedFallback = false;
    char publishError[160] = "";
    bool sent = publishMatrigsAntennaSetCommand(
        s_deferredTxChange.band.c_str(), "TX",
        s_deferredTxChange.oldAnt.c_str(), s_deferredTxChange.newAnt.c_str(),
        &usedFallback, publishError, sizeof(publishError));

    if (sent && usedFallback) {
        publishDebugMessage("[TX-Queue] executed queued TX change via remove/add fallback");
        g_bandStates[s_deferredTxChange.band].tx = s_deferredTxChange.newAnt;
        s_deferredTxChange.valid = false;
        s_lastDeferredTxAttemptMs = 0;
    } else if (sent) {
        publishDebugMessage("[TX-Queue] executed queued TX change via MQTT5 p:set");
        g_bandStates[s_deferredTxChange.band].tx = s_deferredTxChange.newAnt;
        s_deferredTxChange.valid = false;
        s_lastDeferredTxAttemptMs = 0;
    } else {
        publishDebugMessage(publishError[0] ? publishError
                                            : "[TX-Queue] queued TX change publish failed");
    }
}

static void updateCurrentRxTx(const char* value, const char* source)
{
    if (!value || !value[0]) return;
    if (strcmp(value, "RX") != 0 && strcmp(value, "TX") != 0) {
        char dbg[96];
        snprintf(dbg, sizeof(dbg), "[%s] ignored invalid RXTX=%s", source, value);
        publishDebugMessage(dbg);
        return;
    }

    strncpy(currentRXTX, value, sizeof(currentRXTX) - 1);
    currentRXTX[sizeof(currentRXTX) - 1] = '\0';
}

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

    // Per-band retained state is not proof that this is the active band.
    // Only the DT feed owns currentBand; /b/<band> refreshes UI/keypad data
    // when it belongs to the already-active band.
    if (currentBand == band) {
        String ants = listAntennasForBand(band);
        if (ants.length()) {
            currentAntennas = ants;
            currentAntList  = getCurrentAntList();
        } else {
            currentAntennas = "?";
            currentAntList.clear();
        }
    }
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

    // If we already know the current band (eg from retained DT state),
    // refresh the current antenna list so the web UI stays useful.
    if (currentBand.length() && currentBand != "?" && currentBand != "-") {
        String ants = listAntennasForBand(currentBand.c_str());
        if (ants.length()) {
            currentAntennas = ants;
            currentAntList  = getCurrentAntList();
        }
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
 *  “…/sta/<station>” handler (selected RX/TX antenna bank)
 * ===============================================================*/
void handleStationStateJSON(const char* json)
{
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json)) return;

    updateCurrentRxTx(doc["RXTX"] | "", "StationState");
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

    const char* liveState = doc["RXTX"] | ""; // optional live PTT state
    const bool hasLiveState =
        (strcmp(liveState, "RX") == 0 || strcmp(liveState, "TX") == 0);

    /* if we just fell back to RX, flush any queued command ---- */
    if (hasLiveState && g_txActive && strcmp(liveState, "RX") == 0 && g_pendingTx.valid) {

        s_deferredTxChange = g_pendingTx;
        s_deferredTxChange.valid = true;
        g_pendingTx.valid = false;
        publishDebugMessage("[TX-Queue] deferred queued TX change until MQTT callback returns");
    }

    if (hasLiveState)
        g_txActive = (strcmp(liveState, "TX") == 0);  // remember current PTT state

    const char* targetBand = doc["TARGET"]["BAND"] | "";
    const char* bandsField = doc["BANDS"] | "?";
    const char* band = targetBand;
    if (!band[0] || strcmp(band, "?") == 0 || strcmp(band, "-") == 0)
        band = bandsField;

    // When MatriGS reports OFFLINE it may send BANDS="-" (no current band).
    // Keep the last valid `currentBand` so keypad selection and UI remain usable.
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
