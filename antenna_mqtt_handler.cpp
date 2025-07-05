/**
 * antenna_mqtt_handler.cpp
 * -------------------------------------------------------------
 *  • Stores the latest “available” JSON in RAM
 *  • Provides alphabet-AND-number aware sorting (“A1” < “A-H”)
 *  • Supplies helpers for band look-up and pretty printing
 */

#include "antenna_mqtt_handler.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <PubSubClient.h> 
#include <algorithm>
#include <ctype.h>   // isdigit, tolower
#include <map>

extern PubSubClient  client;          
extern char currentRXTX[4];
extern std::vector<String> currentAntList;   
extern String currentBand;                   
extern String currentAntennas;
extern std::map<String,String> bandCache;


// debug publisher from the main sketch
extern void publishDebugMessage(const char* msg);

// live antenna map (≈1 – 2 kB for your data)
DynamicJsonDocument availableDoc(4096);

/* -------------------------------------------------------------
 *  Natural alphanumeric compare:  A1  < A2  < A10 < A-BLAH
 * ----------------------------------------------------------- */
static bool naturalLess(const String& a, const String& b)
{
  const char *pa = a.c_str();
  const char *pb = b.c_str();

  while (*pa || *pb) {

    /* ---- skip separators like '-' or '_' ---- */
    while (*pa && !isalnum(*pa)) ++pa;
    while (*pb && !isalnum(*pb)) ++pb;

    /* both strings ended -> they were equal length */
    if (!*pa || !*pb) break;

    /* ---- numeric chunk ? ---- */
    if (isdigit(*pa) && isdigit(*pb)) {
      long na = strtol(pa, (char**)&pa, 10);
      long nb = strtol(pb, (char**)&pb, 10);
      if (na != nb) return na < nb;
    }

    /* ---- alphabetic char ---- */
    else {
      char ca = tolower(*pa++);
      char cb = tolower(*pb++);
      if (ca != cb) return ca < cb;
    }
  }

  /* shorter (after skipping separators) comes first */
  return *pa == '\0' && *pb != '\0';
}

/* -------------------------------------------------------------
 *  Build a space-separated antenna list for <band>, sorted
 * ----------------------------------------------------------- */
String listAntennasForBand(const char* band)
{
  auto it = bandCache.find(band);
  if (it != bandCache.end()) return it->second;   // cache hit

  /* collect & sort using the naturalLess comparator */
  std::vector<String> names;
  for (JsonPair kv : availableDoc.as<JsonObject>()) {
      JsonObject map = kv.value();
      if (map.containsKey(band)) names.emplace_back(kv.key().c_str());
  }
  std::sort(names.begin(), names.end(), naturalLess);

  /* join into single space-separated string */
  String out;
  for (const auto& n : names) { out += n; out += ' '; }
  out.trim();

  bandCache[band] = out;                          // cache store
  return out;
}


/* -------------------------------------------------------------
 *  “…/sta/<station>/available”  ➜  cache + pretty log
 * ----------------------------------------------------------- */
void handleAvailableJSON(const char* json)
{
  availableDoc.clear();
  bandCache.clear();      // flush cached lists

  if (deserializeJson(availableDoc, json)) {
      publishDebugMessage("[handleAvailableJSON] ❌ Failed to parse JSON");
      return;
  }

  std::vector<String> ants;
  for (JsonPair kv : availableDoc.as<JsonObject>())
      ants.emplace_back(kv.key().c_str());
  std::sort(ants.begin(), ants.end(), naturalLess);

  Serial.println("Parsed antenna availability:");
  for (const String& ant : ants) {
      Serial.printf("Antenna: %s\n", ant.c_str());

      JsonObject bandMap = availableDoc[ant];
      std::vector<String> bands;
      for (JsonPair kv : bandMap) bands.emplace_back(kv.key().c_str());
      std::sort(bands.begin(), bands.end(), naturalLess);

      for (const String& band : bands) {
          JsonArray modes = bandMap[band];
          Serial.printf("  Band: %s → Modes: ", band.c_str());
          for (const char* m : modes) Serial.printf("%s ", m);
          Serial.println();
      }
  }
}

// ────────────────────────────────────────────────────────────────
//  “…/dt/…” current-band handler with de-dup logic
// ────────────────────────────────────────────────────────────────
// ────────────────────────────────────────────────────────────────
//  handleCurrentBandJSON()
//  – Sends antennas to TXANTENNAS / RXANTENNAS topics
//  – Logs one line only when band or antenna list changes
// ────────────────────────────────────────────────────────────────
void handleCurrentBandJSON(const char* json)
{
  /*  ↳  state remembered across calls (for debug de-dup) */
  static String   lastBand = "?";
  static uint32_t lastHash = 0;

  /*  ↳  globals from main sketch */
  extern String currentBand;
  extern String currentAntennas;
  extern std::vector<String> currentAntList;
  extern char   station_name[];
  extern PubSubClient client;

  /* ---------- parse incoming JSON ------------------------------------ */
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, json)) return;              // bad JSON → ignore

  const char* band  = doc["BANDS"] | "?";
  String      ants  = listAntennasForBand(band);       // already sorted

    /* ---------- remember latest RX/TX state --------------------------- */
  if (doc.containsKey("TARGET")) {
      JsonObject t   = doc["TARGET"];
      const char* rxtx = t["RXTX"] | "";
      strncpy(currentRXTX, rxtx, sizeof(currentRXTX) - 1);
  }


  /* ---------- compute hash for debug de-dup -------------------------- */
  uint32_t hash = 5381;
  for (char c : ants) hash = ((hash << 5) + hash) ^ c;   // djb2-xor

  if (lastBand == band && lastHash == hash) return;      // nothing new

  lastBand = band;
  lastHash = hash;

  /* ---------- concise debug line ------------------------------------- */
  char dbg[192];
  snprintf(dbg, sizeof(dbg),
           "[BandChange] %s | antennas: %s",
           band, ants.c_str());
  publishDebugMessage(dbg);

  /* ---------- update globals & serial-select list -------------------- */
  currentBand     = band;
  currentAntennas = ants;

  currentAntList.clear();
  int pos = 0;
  while (pos < ants.length()) {
      int sp = ants.indexOf(' ', pos);
      if (sp == -1) sp = ants.length();
      String token = ants.substring(pos, sp);
      token.trim();
      if (token.length()) currentAntList.push_back(token);
      pos = sp + 1;
  }
}
