#include "MediaManager.h"
#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "ErrorHandler.h"
#include "NavigationCache.h"
#include "RuntimeDiagnostics.h"
#include "TlsTrust.h"
#include "mode_abstraction.h"
#include <FastLED.h>
#include <esp_heap_caps.h>

bool MediaManager::_taskBusy = false;

namespace {
static constexpr size_t MAX_LYRICS_RESPONSE_BYTES = 128 * 1024;
static constexpr size_t MAX_METADATA_RESPONSE_BYTES = 256 * 1024;
static constexpr uint32_t HTTP_EOF_SETTLE_MS = 750;
static char lastLyricsResponseError[128] = {0};
// MbedTLS needs a reasonably large contiguous internal block even though the
// response body and JSON document live in PSRAM. Starting another handshake
// below this point is much more likely to reset the board than to succeed.
static constexpr size_t MIN_LYRICS_TLS_INTERNAL_BLOCK = 24 * 1024;

bool networkAbortRequested() {
  return BackgroundWorker::isCancellationRequested() ||
         BackgroundWorker::isSkipRequested();
}

bool waitForLyricsNetworkCooldown(uint32_t normalMs, uint32_t weakLinkMs) {
  const uint32_t waitMs =
      WiFi.RSSI() <= -75 ? weakLinkMs : normalMs;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < waitMs) {
    if (networkAbortRequested() || WiFi.status() != WL_CONNECTED)
      return false;
    delay(50);
  }
  return true;
}

uint32_t lyricsRequestTimeoutMs() {
  // HTTPClient has a separate connection timeout whose default is only five
  // seconds. Weak links on this board regularly need longer just to establish
  // the TCP/TLS connection, even though the response timeout is already 10 s.
  return WiFi.RSSI() <= -75 ? 15000UL : 10000UL;
}

bool canStartLyricsTlsRequest() {
  RuntimeDiagnostics::markPhase(
      "lyrics/heap-check", BackgroundWorker::getStackHighWaterMark());
  if (WiFi.status() != WL_CONNECTED || networkAbortRequested())
    return false;

  if (!heap_caps_check_integrity_all(true)) {
    ErrorHandler::logError(ERR_CAT_SYSTEM,
                           "Heap integrity check failed before lyrics request",
                           "fetchLyricsIfNeeded");
    return false;
  }

  const size_t largestInternal = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largestInternal < MIN_LYRICS_TLS_INTERNAL_BLOCK) {
    Serial.printf("Lyrics request stopped safely: largest internal block is "
                  "%u bytes\n",
                  (unsigned)largestInternal);
    ErrorHandler::logError(ERR_CAT_SYSTEM,
                           "Not enough contiguous memory for another lyrics "
                           "connection",
                           "fetchLyricsIfNeeded");
    return false;
  }
  return true;
}

void closeLyricsRequest(HTTPClient &http, WiFiClientSecure &client) {
  RuntimeDiagnostics::markPhase(
      "lyrics/socket-close", BackgroundWorker::getStackHighWaterMark());
  http.end();
  client.stop();
  // Let the TCP/IP task release its buffers before another TLS connection.
  delay(50);
}


// HTTPClient::getString() stores the complete response in scarce internal
// heap. API providers can return unexpectedly large JSON (or an HTML error
// page), which can overlap with TLS and ArduinoJson allocations and reboot the
// ESP32-S3. Stream into PSRAM with a hard bound instead.
bool readHttpResponseToPsram(HTTPClient &http, PsramString &body,
                             size_t maximumBytes,
                             uint32_t idleTimeoutMs = 10000) {
  body.clear();
  if (networkAbortRequested())
    return false;
  const int expectedLength = http.getSize();
  if (expectedLength > (int)maximumBytes)
    return false;

  const size_t initialCapacity =
      expectedLength > 0 ? (size_t)expectedLength
                         : std::min(maximumBytes, (size_t)16 * 1024);
  body.reserve(initialCapacity);
  WiFiClient *stream = http.getStreamPtr();
  if (!stream)
    return false;

  uint32_t lastDataAt = millis();
  char chunk[1024];
  while (body.size() < maximumBytes) {
    if (networkAbortRequested()) {
      body.clear();
      return false;
    }
    const int available = stream->available();
    if (available <= 0) {
      // Unknown-length HTTP/1.0 responses end by closing the connection. On
      // ESP32, connected() can turn false just before the final decrypted
      // bytes become visible in available(). Give that tail a short settling
      // window instead of returning a truncated JSON document.
      if (!http.connected() && !body.empty() &&
          millis() - lastDataAt >= HTTP_EOF_SETTLE_MS)
        break;
      if (millis() - lastDataAt > idleTimeoutMs)
        break;
      delay(1);
      continue;
    }

    const size_t remaining = maximumBytes - body.size();
    const size_t toRead =
        std::min(remaining, std::min((size_t)available, sizeof(chunk)));
    const int bytesRead = stream->read((uint8_t *)chunk, toRead);
    if (bytesRead <= 0) {
      delay(1);
      continue;
    }
    body.append(chunk, (size_t)bytesRead);
    lastDataAt = millis();
    if (expectedLength >= 0 && body.size() >= (size_t)expectedLength)
      break;
    delay(1); // Keep WiFi/system tasks responsive during a continuous stream.
  }

  if (body.empty())
    return false;
  if (expectedLength >= 0 && body.size() != (size_t)expectedLength)
    return false;
  // An unknown-length response that fills the entire bound is treated as
  // truncated; never attempt to parse or retain it.
  if (expectedLength < 0 && body.size() == maximumBytes)
    return false;
  return true;
}

bool extractLyricsFromResponse(HTTPClient &http, const char *primaryField,
                               const char *fallbackField,
                               PsramString &lyrics) {
  lastLyricsResponseError[0] = '\0';
  RuntimeDiagnostics::markPhase(
      "lyrics/response-read", BackgroundWorker::getStackHighWaterMark());
  PsramString response;
  if (!readHttpResponseToPsram(http, response, MAX_LYRICS_RESPONSE_BYTES)) {
    snprintf(lastLyricsResponseError, sizeof(lastLyricsResponseError),
             "response read failed (%u bytes)", (unsigned)response.size());
    return false;
  }

  RuntimeDiagnostics::markPhase(
      "lyrics/json-parse", BackgroundWorker::getStackHighWaterMark());
  const size_t jsonCapacity = response.size() + 4096;
  BasicJsonDocument<SpiRamAllocator> doc(jsonCapacity);
  const DeserializationError error =
      deserializeJson(doc, response.data(), response.size());
  if (error) {
    snprintf(lastLyricsResponseError, sizeof(lastLyricsResponseError),
             "JSON %s at %u bytes", error.c_str(),
             (unsigned)response.size());
    return false;
  }

  const char *value = doc[primaryField] | "";
  if ((!value || value[0] == '\0' || strcmp(value, "null") == 0) &&
      fallbackField)
    value = doc[fallbackField] | "";
  if (!value || value[0] == '\0' || strcmp(value, "null") == 0) {
    snprintf(lastLyricsResponseError, sizeof(lastLyricsResponseError),
             "lyrics fields were empty");
    return false;
  }
  lyrics.assign(value);
  return !lyrics.empty();
}
} // namespace

void MediaManager::init() { _taskBusy = false; }

void MediaManager::syncFromStorage() {
  Storage.loadIndex(MODE_CD);
  Storage.loadIndex(MODE_BOOK);
  syncLibraryFromStorage();
}

void MediaManager::filter(const char *query, int filterMode, bool ledMasterOn) {
  if (query == nullptr)
    return;

  const uint32_t startedAt = millis();

  // Clear and reset results
  search_matches.clear();
  search_display_offset = 0;

  String q = String(query);
  q.toLowerCase();

  if (q.length() == 0) {
    // UI should trigger initial batch render
    return;
  }

  int total = getItemCount();
  for (int i = 0; i < total; i++) {
    // RAM-only access for lightning fast search
    ItemView item = getItemAtRAM(i);
    if (!item.isValid)
      break;

    String matchTitle = item.title;
    String matchArtist = item.artistOrAuthor;
    String matchGenre = item.genre;

    matchTitle.toLowerCase();
    matchArtist.toLowerCase();
    matchGenre.toLowerCase();

    bool match = false;
    if (filterMode == 0) { // All
      if (matchTitle.indexOf(q) >= 0 || matchArtist.indexOf(q) >= 0 ||
          matchGenre.indexOf(q) >= 0)
        match = true;
    } else if (filterMode == 1) { // Title
      if (matchTitle.indexOf(q) >= 0)
        match = true;
    } else if (filterMode == 2) { // Artist
      if (matchArtist.indexOf(q) >= 0)
        match = true;
    } else if (filterMode == 3) { // Genre
      if (matchGenre.indexOf(q) >= 0)
        match = true;
    }

    if (match) {
      search_matches.push_back(i);
    }
  }

  Serial.printf("[SEARCH] '%s': %d matches in %lu ms\n", query,
                (int)search_matches.size(),
                (unsigned long)(millis() - startedAt));
}

#include "ErrorHandler.h" // Moved here as it's used in this section
#include "Utils.h"

// ============================================================================
// INTERNAL API HELPERS
// ============================================================================

MBRelease MediaManager::fetchReleaseByBarcode(const char *barcode) {
  MBRelease result;
  result.success = false;

  if (WiFi.status() != WL_CONNECTED ||
      BackgroundWorker::isCancellationRequested()) {
    ErrorHandler::logWarn(ERR_CAT_NETWORK, "WiFi not connected",
                          "fetchReleaseByBarcode");
    return result;
  }

  BackgroundWorker::reportProgress(0.22f);

  HTTPClient http;
  WiFiClientSecure client;
  configureTrustedTlsClient(client);
  // NetworkClientSecure expects seconds here, unlike HTTPClient::setTimeout.
  client.setHandshakeTimeout(10);

  String url =
      "https://musicbrainz.org/ws/2/release/?query=barcode:" + String(barcode) +
      "&fmt=json&limit=1";

  Serial.printf("MediaManager: MusicBrainz Searching barcode %s\n", barcode);

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (BackgroundWorker::isCancellationRequested()) {
    http.end();
    client.stop();
    return result;
  }

  if (httpCode == 200) {
    PsramString payload;
    const bool bodyRead = readHttpResponseToPsram(
        http, payload, MAX_METADATA_RESPONSE_BYTES);
    http.end();
    client.stop();

    if (!bodyRead) {
      ErrorHandler::logWarn(ERR_CAT_NETWORK,
                            "MusicBrainz response was empty or too large",
                            "fetchReleaseByBarcode");
      return fetchReleaseFromDiscogs(barcode);
    }

    // Parse directly from the mutable PSRAM response so JSON strings can be
    // referenced in place instead of copied into internal RAM.
    BasicJsonDocument<SpiRamAllocator> doc(16384);
    const DeserializationError parseError =
        deserializeJson(doc, payload.data(), payload.size());
    JsonArray releases = doc["releases"].as<JsonArray>();
    if (parseError || releases.isNull() || releases.size() == 0) {
      Serial.println("MediaManager: No releases found in MusicBrainz, trying "
                     "Discogs fallback...");
      return fetchReleaseFromDiscogs(barcode);
    }

    JsonObject release = releases[0];
    result.releaseMbid = release["id"] | "";
    result.title = release["title"] | "";
    decodeHTMLEntities(result.title);

    JsonArray artistCredit = release["artist-credit"].as<JsonArray>();
    if (!artistCredit.isNull() && artistCredit.size() > 0) {
      result.artist = artistCredit[0]["name"] | "";
      decodeHTMLEntities(result.artist);
      result.artist = toTitleCase(result.artist); // Capitalize first letters
    }

    result.title = toTitleCase(result.title); // Capitalize Album Title

    // Year
    const char *releaseDate = release["date"] | "";
    if (releaseDate && strlen(releaseDate) >= 4)
      result.year = String(releaseDate).substring(0, 4).toInt();

    result.success = (result.releaseMbid.length() > 0);

    if (!result.success) {
      Serial.println("MediaManager: MusicBrainz returned incomplete data, "
                     "trying Discogs fallback...");
      return fetchReleaseFromDiscogs(barcode);
    }

    // Supplement with Discogs data for missing year/genre
    // MusicBrainz often lacks genre info, so we proactively fetch from Discogs
    bool needsDiscogs = (result.year == 0);

    if (needsDiscogs) {
      Serial.println(
          "MediaManager: MusicBrainz missing year, fetching from Discogs...");
      MBRelease discogsData = fetchReleaseFromDiscogs(barcode);
      if (discogsData.success) {
        if (discogsData.year > 0) {
          result.year = discogsData.year;
          Serial.printf("MediaManager: Supplemented year from Discogs: %d\n",
                        result.year);
        }
        if (discogsData.genre.length() > 0) {
          result.genre = discogsData.genre;
          Serial.printf("MediaManager: Got genre from Discogs: %s\n",
                        result.genre.c_str());
        }
      }
    }
  } else {
    http.end();
    client.stop();
    ErrorHandler::logError(
        ERR_CAT_NETWORK, String("MusicBrainz HTTP Error: ") + String(httpCode),
        "fetchReleaseByBarcode");
    Serial.printf(
        "MediaManager: MusicBrainz HTTP Error %d, trying Discogs fallback...\n",
        httpCode);
    return fetchReleaseFromDiscogs(barcode);
  }
  return result;
}

// Discogs API Fallback
MBRelease MediaManager::fetchReleaseFromDiscogs(const char *barcode) {
  MBRelease result;
  result.success = false;

  if (WiFi.status() != WL_CONNECTED ||
      BackgroundWorker::isCancellationRequested()) {
    ErrorHandler::logWarn(ERR_CAT_NETWORK,
                          "WiFi not connected for Discogs fallback",
                          "fetchReleaseFromDiscogs");
    return result;
  }

  BackgroundWorker::reportProgress(0.42f);

  HTTPClient http;
  WiFiClientSecure client;
  configureTrustedTlsClient(client);
  client.setHandshakeTimeout(10);

  // Discogs barcode search endpoint with API token
  String url =
      "https://api.discogs.com/database/search?barcode=" + String(barcode) +
      "&type=release&per_page=1&token=" + String(DISCOGS_TOKEN);

  Serial.printf("MediaManager: Discogs Searching barcode %s\n", barcode);

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (BackgroundWorker::isCancellationRequested()) {
    http.end();
    client.stop();
    return result;
  }

  if (httpCode == 200) {
    PsramString payload;
    const bool bodyRead = readHttpResponseToPsram(
        http, payload, MAX_METADATA_RESPONSE_BYTES);
    http.end();
    client.stop();

    if (!bodyRead) {
      ErrorHandler::logWarn(ERR_CAT_NETWORK,
                            "Discogs response was empty or too large",
                            "fetchReleaseFromDiscogs");
      return result;
    }

    // Use PSRAM for JSON to save Heap
    BasicJsonDocument<SpiRamAllocator> doc(32768);
    DeserializationError error =
        deserializeJson(doc, payload.data(), payload.size());

    if (!error) {
      JsonArray results = doc["results"];
      if (results.size() > 0) {
        JsonObject release = results[0];

        // Extract title - Discogs returns combined "Artist - Title" format
        String fullTitle = release["title"].as<String>();
        result.year = release["year"] | 0;

        // Discogs doesn't provide MusicBrainz IDs, use Discogs ID as
        // placeholder
        result.releaseMbid = "discogs_" + release["id"].as<String>();

        // Parse artist and title from combined string
        // Discogs search results typically return "Artist - Album Title"
        int dashPos = fullTitle.indexOf(" - ");
        if (dashPos > 0) {
          result.artist = fullTitle.substring(0, dashPos);
          result.title = fullTitle.substring(dashPos + 3);
        } else {
          // Fallback: use full title and check for artist field
          result.title = fullTitle;
          if (release.containsKey("artist")) {
            result.artist = release["artist"].as<String>();
          } else {
            result.artist = "Various Artists";
          }
        }

        // Extract genre/style information
        // Discogs provides both "genre" (broad) and "style" (specific) arrays
        if (release.containsKey("genre") && release["genre"].size() > 0) {
          result.genre = release["genre"][0].as<String>();
        } else if (release.containsKey("style") &&
                   release["style"].size() > 0) {
          result.genre = release["style"][0].as<String>();
        }

        // Clean up titles
        decodeHTMLEntities(result.title);
        decodeHTMLEntities(result.artist);
        result.title = toTitleCase(result.title);
        result.artist = toTitleCase(result.artist);
        if (result.genre.length() > 0) {
          result.genre = toTitleCase(result.genre);
        }

        result.success = true;
        Serial.println("MediaManager: Successfully fetched from Discogs!");
      } else {
        ErrorHandler::logWarn(ERR_CAT_API, "No results from Discogs",
                              "fetchReleaseFromDiscogs");
      }
    } else {
      Serial.printf("MediaManager: Discogs JSON Parse Error: %s\n",
                    error.c_str());
    }
  } else {
    http.end();
    ErrorHandler::logError(ERR_CAT_NETWORK,
                           String("Discogs HTTP Error: ") + String(httpCode),
                           "fetchReleaseFromDiscogs");
  }
  return result;
}

std::vector<Track> MediaManager::fetchTracklist(const char *releaseMbid,
                                                String *outGenre) {
  std::vector<Track> tracks;
  if (WiFi.status() != WL_CONNECTED || !releaseMbid)
    return tracks;
  if (BackgroundWorker::isCancellationRequested())
    return tracks;

  BackgroundWorker::reportProgress(0.68f);

  delay(1000); // MusicBrainz rate limit

  HTTPClient http;
  WiFiClientSecure client;
  configureTrustedTlsClient(client);
  client.setHandshakeTimeout(10);
  client.setTimeout(15000);

  // Fetch release with included recordings and genres
  String url = "https://musicbrainz.org/ws/2/release/" + String(releaseMbid) +
               "?inc=recordings+genres+tags+release-groups&fmt=json";

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setConnectTimeout(15000);
  http.setTimeout(15000);

  int httpCode = http.GET();
  int contentLen = http.getSize();
  Serial.printf("fetchTracklist: HTTP %d, Content-Length: %d bytes\n", httpCode,
                contentLen);
  Serial.printf("fetchTracklist: Heap Free: %d, PSRAM Free: %d\n",
                ESP.getFreeHeap(), ESP.getFreePsram());

  if (httpCode == 200) {
    PsramString response;
    const bool bodyRead = readHttpResponseToPsram(
        http, response, MAX_METADATA_RESPONSE_BYTES, 15000);
    // Release MbedTLS internal buffers before constructing/parsing the JSON
    // tree. The response itself remains safely resident in PSRAM.
    http.end();
    client.stop();
    if (!bodyRead) {
      Serial.println("fetchTracklist: response was incomplete or too large");
      return tracks;
    }

    Serial.printf("fetchTracklist: Downloaded %u bytes to PSRAM\n",
                  (unsigned)response.size());

    BasicJsonDocument<SpiRamAllocator> doc(98304);
    // This overload is zero-copy. Keep `response` alive until every field has
    // been extracted below; freeing it here used to leave ArduinoJson holding
    // dangling pointers and caused intermittent freezes/reboots.
    DeserializationError error =
        deserializeJson(doc, response.data(), response.size());

    Serial.printf("fetchTracklist: deserializeJson result: %s\n",
                  error.c_str());

    if (!error) {
      // 1. Extract Genre
      if (outGenre) {
        *outGenre = "";
        // Helper to find first valid genre using a simple blacklist
        auto findValidGenre = [](JsonArray arr) -> String {
          for (JsonObject obj : arr) {
            String name = obj["name"].as<String>();
            String lower = name;
            lower.toLowerCase();
            // Blacklist: skip non-genre tags
            if (lower == "hidden track" || lower.indexOf("bonus") >= 0 ||
                lower.indexOf("edition") >= 0 || lower == "remastered" ||
                lower == "cc-by-nc-sa" || lower.indexOf("copy protest") >= 0) {
              continue;
            }
            return name; // Found a good one
          }
          return "";
        };

        if (doc.containsKey("genres") && doc["genres"].size() > 0) {
          *outGenre = findValidGenre(doc["genres"]);
        }

        if (outGenre->length() == 0 && doc.containsKey("tags") &&
            doc["tags"].size() > 0) {
          *outGenre = findValidGenre(doc["tags"]);
        }

        // Check Release Group as fallback
        if (outGenre->length() == 0 && doc.containsKey("release-group")) {
          JsonObject rg = doc["release-group"];
          if (rg.containsKey("genres") && rg["genres"].size() > 0) {
            *outGenre = findValidGenre(rg["genres"]);
          }
          if (outGenre->length() == 0 && rg.containsKey("tags") &&
              rg["tags"].size() > 0) {
            *outGenre = findValidGenre(rg["tags"]);
          }
        }

        if (outGenre->length() == 0)
          *outGenre = "Unknown";

        *outGenre = toTitleCase(*outGenre); // Capitalize Genre
      }

      // 2. Extract Tracks
      // Try "media" array first (Standard structure)
      if (doc.containsKey("media")) {
        JsonArray media = doc["media"];
        for (JsonObject medium : media) {
          int positionOffset =
              tracks.size(); // Continues numbering for multi-disc
          if (medium.containsKey("tracks")) {
            JsonArray trkArray = medium["tracks"];
            for (JsonObject t : trkArray) {
              Track track;
              track.trackNo =
                  t["position"] | (tracks.size() + 1); // Or logical ordering
              track.title = t["title"] | "Unknown Track";
              // Length is usually in "recording" object or "length" field
              if (t.containsKey("recording")) {
                track.durationMs = t["recording"]["length"] | 0;
                track.recordingMbid = t["recording"]["id"] | "";
              } else {
                track.durationMs = t["length"] | 0;
                track.recordingMbid = t["id"] | "";
              }

              // Sanitize
              // Sanitize
              String tempTitle = track.title.c_str();
              decodeHTMLEntities(tempTitle);
              tempTitle = sanitizeText(tempTitle);
              tempTitle = toTitleCase(tempTitle); // Capitalize Track Title
              track.title = tempTitle.c_str();

              track.lyrics.status = "unchecked";
              tracks.push_back(track);
            }
          }
        }
      }
      // Fallback: Check if "recordings" exists (unlikely for this endpoint but
      // safe to check)
      else if (doc.containsKey("recordings")) {
        JsonArray recs = doc["recordings"];
        for (JsonObject r : recs) {
          Track track;
          track.title = r["title"] | "Unknown";
          track.durationMs = r["length"] | 0;
          track.recordingMbid = r["id"] | "";
          track.trackNo = tracks.size() + 1;
          track.lyrics.status = "unchecked";
          String tempTitle = track.title.c_str();
          track.title = toTitleCase(tempTitle)
                            .c_str(); // Capitalize Track Title (Fallback)
          tracks.push_back(track);
        }
      }

    } else {
      Serial.print("fetchTracklist: JSON Parse Error: ");
      Serial.println(error.c_str());
    }
  } else {
    ErrorHandler::logError(ERR_CAT_NETWORK,
                           String("MusicBrainz Tracklist HTTP Error: ") +
                               String(httpCode) +
                               " (MBID: " + String(releaseMbid) + ")",
                           "fetchTracklist");
    Serial.printf("fetchTracklist: HTTP Error %d\n", httpCode);
  }
  http.end();
  client.stop();
  delay(50); // Allow socket cleanup
  return tracks;
}

bool MediaManager::fetchBookByISBN(const char *isbn, Book &book) {
  if (WiFi.status() != WL_CONNECTED || networkAbortRequested())
    return false;

  Serial.printf("Fetching book metadata for ISBN: %s\n", isbn);
  BackgroundWorker::reportProgress(0.25f);

  HTTPClient http;
  WiFiClientSecure client;
  configureTrustedTlsClient(client);
  if (networkAbortRequested())
    return false;
  client.setHandshakeTimeout(10);
  // client.setBufferSizes(1024, 512); // Optimize Buffers

  String url =
      "https://www.googleapis.com/books/v1/volumes?q=isbn:" + String(isbn) +
      "&maxResults=1";
  http.begin(client, url);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (networkAbortRequested()) {
    http.end();
    client.stop();
    return false;
  }
  if (httpCode == 200) {
    PsramString payload;
    const bool bodyRead = readHttpResponseToPsram(
        http, payload, MAX_METADATA_RESPONSE_BYTES);
    http.end();
    client.stop();
    if (!bodyRead)
      return false;

    BackgroundWorker::reportProgress(0.85f);

    // Use PSRAM for JSON to save Heap
    BasicJsonDocument<SpiRamAllocator> doc(32768);
    if (deserializeJson(doc, payload.data(), payload.size()))
      return false;

    if (doc["totalItems"] == 0) {
      Serial.printf("No book found for ISBN: %s\n", isbn);
      return false;
    }

    JsonObject info = doc["items"][0]["volumeInfo"];
    book.title = (const char *)(info["title"] | "Unknown");
    book.author = (info["authors"].size() > 0)
                      ? info["authors"][0].as<String>().c_str()
                      : "Unknown";
    book.genre = (info["categories"].size() > 0)
                     ? info["categories"][0].as<String>().c_str()
                     : "Unknown";

    // Apply Title Case
    book.title = toTitleCase(book.title.c_str()).c_str();
    book.author = toTitleCase(book.author.c_str()).c_str();
    book.genre = toTitleCase(book.genre.c_str()).c_str();

    String date = info["publishedDate"] | "";
    if (date.length() >= 4)
      book.year = date.substring(0, 4).toInt();

    book.isbn = (const char *)isbn;
    book.publisher = (const char *)(info["publisher"] | "");
    book.pageCount = info["pageCount"] | 0;

    // Extract cover URL from Google Books API (prefer their thumbnail)
    if (info["imageLinks"]["thumbnail"]) {
      book.coverUrl = info["imageLinks"]["thumbnail"].as<String>().c_str();
      Serial.printf("Found cover URL: %s\n", book.coverUrl.c_str());
    } else {
      // Fallback to Open Library if Google doesn't have a cover
      book.coverUrl =
          ("https://covers.openlibrary.org/b/isbn/" + String(isbn) + "-M.jpg")
              .c_str();
      Serial.printf("No Google Books cover, using Open Library fallback\n");
    }

    // Small delay to prevent heap fragmentation during bulk sync
    delay(500);
    return true;
  } else {
    ErrorHandler::logError(
        ERR_CAT_NETWORK, String("GoogleBooks HTTP Error: ") + String(httpCode),
        "fetchBookByISBN");
  }
  http.end();
  return false;
}

// Metadata Fetching (Online)
bool MediaManager::fetchMetadataForBarcode(const char *barcode,
                                           ItemView &outView) {
  if (!barcode || WiFi.status() != WL_CONNECTED ||
      BackgroundWorker::isCancellationRequested()) {
    return false;
  }

  // Preservation logic...
  String itemID = outView.uniqueID;
  String preservedCover = outView.coverFile;
  String preservedReleaseMbid = outView.releaseMbid;

  if (itemID.length() == 0) {
    // Find existing by barcode
    int total = getItemCount();
    for (int i = 0; i < total; i++) {
      ItemView item = getItemAt(i);
      if (item.codecOrIsbn == String(barcode)) {
        itemID = item.uniqueID;
        break;
      }
    }
  }
  if (itemID.length() == 0) {
    if (String(barcode).length() > 0)
      itemID = String(barcode);
    else
      itemID = String(millis()) + "_" + String(random(9999));
  }

  MBRelease release = fetchReleaseByBarcode(barcode);
  if (!release.success || BackgroundWorker::isCancellationRequested())
    return false;

  BackgroundWorker::reportProgress(0.60f);

  CD cd;
  cd.uniqueID = itemID.c_str();
  cd.title = release.title.c_str();
  cd.artist = release.artist.c_str();
  cd.year = release.year;
  cd.releaseMbid = release.releaseMbid.c_str();
  cd.barcode = String(barcode).c_str();

  // Copy genre from Discogs if available
  if (release.genre.length() > 0) {
    cd.genre = release.genre.c_str();
    Serial.printf("fetchMetadata: Initial genre from Discogs: %s\n",
                  cd.genre.c_str());
  }

  // A Discogs search result has no MusicBrainz release ID. When editing an
  // existing CD, retain its known MBID rather than replacing it with a
  // placeholder that cannot be used by the track-list endpoint.
  if (String(cd.releaseMbid.c_str()).startsWith("discogs_") &&
      preservedReleaseMbid.length() > 0 &&
      !preservedReleaseMbid.startsWith("discogs_"))
    cd.releaseMbid = preservedReleaseMbid.c_str();

  String gen = "";
  std::vector<Track> tracks;
  // Discogs placeholders are not valid MusicBrainz IDs. Previously they were
  // sent to MusicBrainz twice, making FETCH look frozen for up to a minute.
  // A track-list miss is non-fatal metadata, so make at most one request.
  if (cd.releaseMbid.length() > 0 &&
      !String(cd.releaseMbid.c_str()).startsWith("discogs_")) {
    tracks = fetchTracklist(cd.releaseMbid.c_str(), &gen);
  }
  if (BackgroundWorker::isCancellationRequested())
    return false;
  BackgroundWorker::reportProgress(0.88f);

  Serial.printf("fetchMetadata: Before tracklist - cd.genre = '%s'\n",
                cd.genre.c_str());

  // Prioritize meaningful genre data:
  // 1. Use tracklist genre if it's valid and not "Unknown"
  // 2. Otherwise keep Discogs genre (already set earlier)
  // 3. Fall back to "Unknown" only if nothing else is available

  if (gen.length() > 0 && gen != "Unknown") {
    Serial.printf("fetchMetadata: Tracklist provided genre: '%s' (using it)\n",
                  gen.c_str());
    cd.genre = gen.c_str();
  } else if (cd.genre.length() > 0 && cd.genre != "Unknown") {
    // Keep the Discogs genre we already set
    Serial.printf(
        "fetchMetadata: Keeping Discogs genre: '%s' (tracklist had '%s')\n",
        cd.genre.c_str(), gen.c_str());
  } else if (release.genre.length() > 0 && release.genre != "Unknown") {
    // Fallback: try to get genre from release object
    cd.genre = release.genre.c_str();
    Serial.printf("fetchMetadata: Using genre from release object: %s\n",
                  cd.genre.c_str());
  } else {
    Serial.printf("fetchMetadata: No valid genre available, keeping: '%s'\n",
                  cd.genre.c_str());
  }

  Serial.printf("fetchMetadata: After genre logic - cd.genre = '%s'\n",
                cd.genre.c_str());

  cd.trackCount = (int)tracks.size();
  unsigned long duration = 0;
  for (auto &t : tracks)
    duration += t.durationMs;
  cd.totalDurationMs = duration;

  // FALLBACK: If API returned 0 tracks, check if we have a cached tracklist
  if (cd.trackCount == 0) {
    TrackList *cached = Storage.loadTracklist(cd.releaseMbid.c_str());
    if (cached) {
      Serial.printf("fetchMetadata: API returned 0 tracks, but found %d cached "
                    "tracks. Using cache.\n",
                    (int)cached->tracks.size());
      cd.trackCount = cached->tracks.size();
      cd.totalDurationMs = 0;
      for (auto &t : cached->tracks)
        cd.totalDurationMs += t.durationMs;
      delete cached; // Clean up the object
    }
  }

  if (tracks.size() > 0) {
    TrackList tl;
    tl.releaseMbid = cd.releaseMbid.c_str();
    tl.cdTitle = cd.title.c_str();
    tl.cdArtist = cd.artist.c_str();
    tl.fetchedAt = getCurrentISO8601Timestamp().c_str();
    // Vector copy (different allocators requires explicit range assignment)
    tl.tracks.assign(tracks.begin(), tracks.end());
    Storage.saveTracklist(cd.releaseMbid.c_str(), &tl);
  }
  BackgroundWorker::reportProgress(0.94f);

  // Merge with existing
  CD existing;
  bool exists = Storage.loadCDDetail(itemID, existing);

  if (exists) {
    Serial.printf("fetchMetadata: Found existing CD metadata (ID: %s). "
                  "Cover: '%s', Favorite: %d\n",
                  itemID.c_str(), existing.coverFile.c_str(),
                  existing.favorite);

    if (existing.coverFile.length() > 2) {
      cd.coverFile = existing.coverFile;
      Serial.printf("fetchMetadata: Preserving cover from storage: %s\n",
                    cd.coverFile.c_str());
    } else if (preservedCover.length() > 2) {
      cd.coverFile = preservedCover.c_str();
      Serial.printf("fetchMetadata: Preserving cover from input view: %s\n",
                    cd.coverFile.c_str());
    } else {
      Serial.println("fetchMetadata: No existing cover file to preserve.");
    }
    if (cd.notes.length() == 0)
      cd.notes = existing.notes;
    cd.favorite = existing.favorite;
    // CRITICAL: Preserve LED assignments!
    cd.ledIndices = existing.ledIndices;

    // Smart Merge: Don't let a failed track fetch overwrite existing tracks
    if (cd.trackCount == 0 && existing.trackCount > 0) {
      cd.trackCount = existing.trackCount;
      cd.totalDurationMs = existing.totalDurationMs;
    }
    if ((cd.genre == "Unknown" || cd.genre.length() == 0) &&
        existing.genre != "Unknown") {
      cd.genre = existing.genre;
    }
  }

  // Ensure LED assignment
  if (cd.ledIndices.empty()) {
    cd.ledIndices.push_back(getNextLedIndex());
  }

  // Only mark as fully loaded if we actually got some meat
  cd.detailsLoaded = (cd.trackCount > 0 || cd.year > 0);

  // FETCH is a preview/staging action. Do not rewrite the CD detail or library
  // index until the user explicitly presses SAVE in the Add/Edit page.

  // Update outView
  outView.title = cd.title.c_str();
  outView.artistOrAuthor = cd.artist.c_str();
  outView.genre = cd.genre.c_str();
  outView.year = cd.year;
  outView.uniqueID = cd.uniqueID.c_str();
  outView.codecOrIsbn = cd.barcode.c_str();
  outView.trackCount = cd.trackCount;
  outView.totalDurationMs = cd.totalDurationMs;
  outView.releaseMbid = cd.releaseMbid.c_str();
  outView.coverFile = cd.coverFile.c_str();
  outView.ledIndices = cd.ledIndices; // Pass preserved LEDs back to UI
  outView.favorite = cd.favorite;     // Pass favorite back to UI
  outView.notes = cd.notes.c_str();   // Pass notes back to UI
  outView.detailsLoaded = true;
  outView.isValid = true;

  return true;
}

bool MediaManager::fetchMetadataForISBN(const char *isbn, ItemView &outView) {
  if (!isbn || WiFi.status() != WL_CONNECTED ||
      BackgroundWorker::isCancellationRequested())
    return false;

  String preservedCover = outView.coverFile;
  Book book;
  book.uniqueID = outView.uniqueID.c_str();
  if (book.uniqueID.length() == 0) {
    // Find existing by ISBN
    int total = getItemCount();
    for (int i = 0; i < total; i++) {
      ItemView item = getItemAt(i);
      if (item.codecOrIsbn == String(isbn)) {
        book.uniqueID = item.uniqueID.c_str();
        break;
      }
    }
  }
  if (book.uniqueID.length() == 0) {
    if (String(isbn).length() > 0)
      book.uniqueID = String(isbn).c_str();
    else
      book.uniqueID = (String(millis()) + "_" + String(random(9999))).c_str();
  }

  if (!fetchBookByISBN(isbn, book) ||
      BackgroundWorker::isCancellationRequested())
    return false;

  // Merge with existing
  Book existing;
  if (Storage.loadBookDetail(book.uniqueID.c_str(), existing)) {
    if (book.notes.length() == 0)
      book.notes = existing.notes;
    book.favorite = existing.favorite;
    if (book.coverFile.length() == 0) {
      if (existing.coverFile.length() > 2) {
        book.coverFile = existing.coverFile;
        Serial.printf(
            "fetchMetadataForISBN: Preserving cover from storage: %s\n",
            book.coverFile.c_str());
      } else if (preservedCover.length() > 2) {
        book.coverFile = preservedCover.c_str();
        Serial.printf(
            "fetchMetadataForISBN: Preserving cover from input view: %s\n",
            book.coverFile.c_str());
      } else {
        Serial.println(
            "fetchMetadataForISBN: No existing cover file to preserve.");
      }
    }
    // CRITICAL: Preserve LED assignments!
    book.ledIndices = existing.ledIndices;
  }

  // Ensure LED assignment
  if (book.ledIndices.empty()) {
    book.ledIndices.push_back(getNextLedIndex());
  }

  book.detailsLoaded = true;

  // As with CD lookup, leave persistence to the Add/Edit SAVE action.

  // Update outView
  outView.title = book.title.c_str();
  outView.artistOrAuthor = book.author.c_str();
  outView.genre = book.genre.c_str();
  outView.year = book.year;
  outView.uniqueID = book.uniqueID.c_str();
  outView.codecOrIsbn = book.isbn.c_str();
  outView.pageCount = book.pageCount;
  outView.publisher = book.publisher.c_str();
  outView.coverFile = book.coverFile.c_str();
  outView.ledIndices = book.ledIndices; // Pass preserved LEDs back to UI
  outView.favorite = book.favorite;     // Pass favorite back to UI
  outView.notes = book.notes.c_str();   // Pass notes back to UI
  outView.detailsLoaded = true;
  outView.isValid = true;

  return true;
}

namespace {

bool isValidMbid(const String &value) {
  if (value.length() != 36)
    return false;
  for (size_t i = 0; i < value.length(); i++) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if ((separator && value[i] != '-') ||
        (!separator && !isHexadecimalDigit(value[i])))
      return false;
  }
  return true;
}

String escapeMusicBrainzSearchValue(String value) {
  // Quoted Lucene values still need embedded slashes and quotes escaped.
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  return value;
}

String fetchMusicBrainzCoverGroup(const String &artist, const String &album,
                                  bool quickMode, bool &providerResponded,
                                  String &connectionError) {
  if (networkAbortRequested() || WiFi.status() != WL_CONNECTED)
    return "";

  HTTPClient http;
  WiFiClientSecure client;
  if (!configureTrustedTlsClient(client)) {
    connectionError = "MusicBrainz: TLS clock unavailable";
    return "";
  }

  const unsigned long handshakeTimeoutSeconds = quickMode ? 2UL : 20UL;
  const uint32_t requestTimeoutMs = quickMode ? 3000UL : 20000UL;
  client.setHandshakeTimeout(handshakeTimeoutSeconds);
  client.setTimeout(requestTimeoutMs);

  const String query =
      "releasegroup:\"" + escapeMusicBrainzSearchValue(album) +
      "\" AND artist:\"" + escapeMusicBrainzSearchValue(artist) + "\"";
  const String url = "https://musicbrainz.org/ws/2/release-group/?query=" +
                     urlEncode(query) + "&fmt=json&limit=3";

  Serial.printf("  MusicBrainz cover search: '%s' - '%s'\n", artist.c_str(),
                album.c_str());
  if (!http.begin(client, url)) {
    connectionError = "MusicBrainz: could not start request";
    client.stop();
    return "";
  }
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  http.setConnectTimeout(requestTimeoutMs);
  http.setTimeout(requestTimeoutMs);

  const int httpCode = http.GET();
  if (networkAbortRequested()) {
    http.end();
    client.stop();
    return "";
  }

  if (httpCode == HTTP_CODE_OK) {
    providerResponded = true;
    PsramString payload;
    const bool bodyRead = readHttpResponseToPsram(
        http, payload, MAX_METADATA_RESPONSE_BYTES, quickMode ? 2500 : 10000);
    http.end();
    client.stop();
    if (!bodyRead) {
      connectionError = "MusicBrainz: response was empty or too large";
      return "";
    }

    BasicJsonDocument<SpiRamAllocator> doc(24576);
    const DeserializationError parseError =
        deserializeJson(doc, payload.data(), payload.size());
    if (parseError) {
      Serial.printf("  MusicBrainz cover response parse error: %s\n",
                    parseError.c_str());
      return "";
    }

    JsonArray groups = doc["release-groups"].as<JsonArray>();
    for (JsonObject group : groups) {
      const int score = group["score"] | 0;
      String groupMbid = group["id"] | "";
      groupMbid.trim();
      // A quoted artist/title match should normally score 100. Reject weaker
      // matches so a similarly named album does not receive the wrong artwork.
      if (score >= 90 && isValidMbid(groupMbid)) {
        const String coverUrl =
            "https://coverartarchive.org/release-group/" + groupMbid +
            "/front-250";
        Serial.printf("  Found MusicBrainz release-group cover: %s\n",
                      coverUrl.c_str());
        return coverUrl;
      }
    }
    Serial.println("  MusicBrainz returned no strong release-group match");
    return "";
  }

  if (httpCode < 0) {
    char tlsErrorText[96] = {0};
    const int tlsError = client.lastError(tlsErrorText, sizeof(tlsErrorText));
    connectionError =
        "MusicBrainz: " + String(HTTPClient::errorToString(httpCode));
    if (tlsError != 0)
      connectionError +=
          " / TLS " + String(tlsError) + " " + String(tlsErrorText);
  } else {
    connectionError = "MusicBrainz: HTTP " + String(httpCode);
  }
  Serial.printf("  %s\n", connectionError.c_str());
  http.end();
  client.stop();
  return "";
}

} // namespace

// Fetch an album cover URL. Prefer MusicBrainz/Cover Art Archive because it
// matches the metadata provider and offers release-group artwork, then retain
// iTunes as an independent fallback.
String MediaManager::fetchAlbumCoverUrl(const char *artist, const char *album,
                                        bool quickMode,
                                        bool *requestFailed,
                                        String *errorDetail) {
  String coverUrl = "";
  if (requestFailed)
    *requestFailed = false;
  if (errorDetail)
    *errorDetail = "";
  String cleanArtist = artist ? String(artist) : String();
  String cleanAlbum = album ? String(album) : String();
  cleanArtist.trim();
  cleanAlbum.trim();
  if (cleanArtist.length() == 0 || cleanAlbum.length() == 0)
    return coverUrl;

  bool providerResponded = false;
  String providerError;
  coverUrl = fetchMusicBrainzCoverGroup(cleanArtist, cleanAlbum, quickMode,
                                        providerResponded, providerError);
  if (coverUrl.length() > 0 || networkAbortRequested())
    return coverUrl;

  // Bulk sync must abandon weak records quickly. Manual search instead gets
  // one realistic handshake window for the board's high-latency WiFi, without
  // multiplying that delay through retries.
  const int maxAttempts = 1;
  const unsigned long handshakeTimeoutSeconds = quickMode ? 2UL : 20UL;
  const uint32_t requestTimeoutMs = quickMode ? 3000UL : 20000UL;

  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    if (networkAbortRequested() || WiFi.status() != WL_CONNECTED)
      break;
    HTTPClient http;
    WiFiClientSecure client;
    if (!configureTrustedTlsClient(client)) {
      providerError = "iTunes: TLS clock unavailable";
      break;
    }
    if (networkAbortRequested())
      break;
    client.setHandshakeTimeout(handshakeTimeoutSeconds);
    client.setTimeout(requestTimeoutMs);

    String searchQuery = cleanArtist + " " + cleanAlbum;
    String encodedQuery = urlEncode(searchQuery);

    String url = "https://itunes.apple.com/search?term=" + encodedQuery +
                 "&entity=album&limit=1";

    if (attempt == 1) {
      Serial.printf("  Trying iTunes cover search for: %s - %s\n",
                    cleanArtist.c_str(), cleanAlbum.c_str());
    } else
      Serial.printf("  Retry #%d...\n", attempt);

    if (!http.begin(client, url)) {
      providerError = "iTunes: could not start request";
      client.stop();
      break;
    }

    // Add headers to mimic a browser
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, "
                      "like Gecko) Chrome/112.0.0.0 Safari/537.36");
    http.addHeader("Accept", "*/*");
    http.addHeader("Connection", "close");

    http.setConnectTimeout(requestTimeoutMs);
    http.setTimeout(requestTimeoutMs);

    int httpCode = http.GET();
    if (networkAbortRequested()) {
      http.end();
      break;
    }

    if (httpCode == 200) {
      providerResponded = true;
      PsramString payload;
      const bool bodyRead = readHttpResponseToPsram(
          http, payload, MAX_METADATA_RESPONSE_BYTES,
          quickMode ? 2500 : 10000);
      if (networkAbortRequested()) {
        http.end();
        client.stop();
        break;
      }
      static constexpr char ARTWORK_FIELD[] = "\"artworkUrl100\":\"";
      const size_t artworkIndex =
          bodyRead ? payload.find(ARTWORK_FIELD) : PsramString::npos;
      if (artworkIndex != PsramString::npos) {
        const size_t startIndex = artworkIndex + strlen(ARTWORK_FIELD);
        const size_t endIndex = payload.find('\"', startIndex);
        if (endIndex != PsramString::npos) {
          coverUrl = String(payload.c_str() + startIndex)
                         .substring(0, endIndex - startIndex);

          // Request 240x240 image
          coverUrl.replace("100x100", "240x240");
          Serial.printf("  Found iTunes cover: %s\n", coverUrl.c_str());
        }
      }
      http.end();
      break; // Success!
    } else if (httpCode < 0) {
      char tlsErrorText[96] = {0};
      const int tlsError = client.lastError(tlsErrorText,
                                            sizeof(tlsErrorText));
      String detail = HTTPClient::errorToString(httpCode);
      if (tlsError != 0) {
        detail += " / TLS " + String(tlsError) + " " + String(tlsErrorText);
      }
      providerError = "iTunes: " + detail;
      Serial.printf("  Cover provider connection error: %d (%s)\n", httpCode,
                    detail.c_str());
      http.end();
      if (attempt >= maxAttempts)
        break;
      delay(250);
      continue;
    } else {
      providerError = "iTunes: HTTP " + String(httpCode);
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("iTunes HTTP Error: ") + String(httpCode) +
                                 " (Query: " + cleanArtist + " - " +
                                  cleanAlbum + ")",
                             "fetchAlbumCoverUrl");
      Serial.printf("  ✗ HTTP Error: %d\n", httpCode);
      http.end();
    }
    delay(quickMode ? 20 : 150);
  }

  // Only describe this as a connection failure if neither provider completed a
  // valid response. A responsive provider with no match is simply "not found".
  if (coverUrl.length() == 0 && !providerResponded &&
      providerError.length() > 0) {
    if (requestFailed)
      *requestFailed = true;
    if (errorDetail)
      *errorDetail = providerError;
  }
  return coverUrl;
}

#include <algorithm>

void MediaManager::sortByArtistOrAuthor() {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_CD:
    std::sort(cdLibrary.begin(), cdLibrary.end(), [](const CD &a, const CD &b) {
      String aStr = a.artist.c_str();
      String bStr = b.artist.c_str();
      aStr.toLowerCase();
      bStr.toLowerCase();
      if (aStr != bStr)
        return aStr < bStr;
      return String(a.title.c_str()) < String(b.title.c_str());
    });
    break;
  case MODE_BOOK:
    std::sort(bookLibrary.begin(), bookLibrary.end(),
              [](const Book &a, const Book &b) {
                String aStr = a.author.c_str();
                String bStr = b.author.c_str();
                aStr.toLowerCase();
                bStr.toLowerCase();
                if (aStr != bStr)
                  return aStr < bStr;
                return String(a.title.c_str()) < String(b.title.c_str());
              });
    break;
  default:
    break;
  }

  // REBUILD CACHE after sorting to avoid indexing mismatches
  rebuildNavigationCache(getCurrentItemIndex());
  saveLibrary();
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

// ============================================================================
// LYRICS IMPLEMENTATION
// ============================================================================

// Improved fetchLyricsIfNeeded with better timeouts
LyricsResult fetchLyricsIfNeeded(const char *releaseMbid, int trackIndex,
                                 bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("fetchLyricsIfNeeded: No WiFi");
    return LYRICS_ERROR;
  }

  // 1. Check current status in cache first
  TrackList *tl = Storage.loadTracklist(releaseMbid);
  if (!tl) {
    Serial.printf("fetchLyricsIfNeeded: Could not load TrackList for %s\n",
                  releaseMbid);
    return LYRICS_NOT_FOUND;
  }

  if (trackIndex < 0 || trackIndex >= tl->tracks.size()) {
    Serial.printf(
        "fetchLyricsIfNeeded: TrackIndex %d out of bounds (Size: %d)\n",
        trackIndex, (int)tl->tracks.size());
    Storage.deleteTracklist(tl);
    return LYRICS_NOT_FOUND;
  }

  Track &track = tl->tracks[trackIndex];

  if (!force) {
    if (track.lyrics.status == "cached") {
      Storage.deleteTracklist(tl);
      return LYRICS_ALREADY_CACHED;
    }

    if (track.lyrics.status == "missing") {
      Storage.deleteTracklist(tl);
      return LYRICS_NOT_FOUND; // Don't retry automatically
    }
  }

  // Retain only the small request fields while TLS is active. Keeping a full
  // track list alive during the provider response needlessly raised the peak
  // allocation and made large releases more vulnerable to fragmentation.
  const String albumArtist = tl->cdArtist.c_str();
  const String albumTitle = tl->cdTitle.c_str();
  const String trackTitle = track.title.c_str();
  const int trackNumber = track.trackNo;
  Storage.deleteTracklist(tl);
  tl = nullptr;

  // 2. Fetch from API. The body, JSON document, and extracted lyrics all use
  // PSRAM and are bounded independently from the TLS/internal heap.
  PsramString finalLyrics;
  bool found = false;
  bool providerReached = false;
  bool requestFailed = false;

  // --- STRATEGY 1: LYRICS.OVH ---
  {
    Serial.println("Using Strategy 1: Lyrics.ovh...");
    HTTPClient http;
    WiFiClientSecure client;
    RuntimeDiagnostics::markPhase(
        "lyrics/ovh-config", BackgroundWorker::getStackHighWaterMark());
    if (!canStartLyricsTlsRequest() || !configureTrustedTlsClient(client)) {
      Serial.println("  -> Lyrics.ovh request could not start safely");
      return LYRICS_ERROR;
    }
    const uint32_t requestTimeoutMs = lyricsRequestTimeoutMs();
    client.setHandshakeTimeout((requestTimeoutMs + 999UL) / 1000UL);
    client.setTimeout(requestTimeoutMs);

    // Schema: https://api.lyrics.ovh/v1/artist/title
    String url = "https://api.lyrics.ovh/v1/" +
                 urlEncode(albumArtist) + "/" + urlEncode(trackTitle);

    Serial.printf("Query URL: %s\n", url.c_str());

    if (!http.begin(client, url)) {
      client.stop();
      return LYRICS_ERROR;
    }
    http.useHTTP10(true);
    http.setReuse(false);
    http.addHeader("User-Agent", "DigitalLibrarian/1.0");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(requestTimeoutMs);
    http.setTimeout(requestTimeoutMs);
    RuntimeDiagnostics::markPhase(
        "lyrics/ovh-get", BackgroundWorker::getStackHighWaterMark());
    int code = http.GET();

    providerReached = code > 0;
    if (code == 200) {
      found = extractLyricsFromResponse(http, "lyrics", nullptr, finalLyrics);
    } else {
      requestFailed = code <= 0;
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("Lyrics.ovh HTTP Error: ") + String(code) +
                                 " (Track: " + trackTitle + ")",
                             "fetchLyricsIfNeeded");
      Serial.printf("  -> Lyrics.ovh HTTP Error %d\n", code);
    }
    closeLyricsRequest(http, client);
  }

  // --- STRATEGY 2: LRCLIB (Fallback) ---
  if (!found && !networkAbortRequested() && WiFi.status() == WL_CONNECTED) {
    if (!waitForLyricsNetworkCooldown(300, 1500))
      return LYRICS_ERROR;
    Serial.println("  -> Lyrics.ovh failed, trying LRCLib...");
    HTTPClient http;
    WiFiClientSecure client;
    RuntimeDiagnostics::markPhase(
        "lyrics/lrclib-config", BackgroundWorker::getStackHighWaterMark());
    if (!canStartLyricsTlsRequest() || !configureTrustedTlsClient(client)) {
      Serial.println("  -> LRCLib request could not start safely");
      return LYRICS_ERROR;
    }
    const uint32_t requestTimeoutMs = lyricsRequestTimeoutMs();
    client.setHandshakeTimeout((requestTimeoutMs + 999UL) / 1000UL);
    client.setTimeout(requestTimeoutMs);

    String url = "https://lrclib.net/api/get?artist_name=" +
                 urlEncode(albumArtist) + "&track_name=" +
                 urlEncode(trackTitle) + "&album_name=" +
                 urlEncode(albumTitle);

    Serial.printf("Query URL (Fallback): %s\n", url.c_str());

    if (!http.begin(client, url)) {
      client.stop();
      return LYRICS_ERROR;
    }
    http.useHTTP10(true);
    http.setReuse(false);
    http.addHeader("User-Agent", "DigitalLibrarian/1.0");
    http.addHeader("Connection", "close");
    http.setConnectTimeout(requestTimeoutMs);
    http.setTimeout(requestTimeoutMs);

    RuntimeDiagnostics::markPhase(
        "lyrics/lrclib-get", BackgroundWorker::getStackHighWaterMark());
    int code = http.GET();
    providerReached = providerReached || code > 0;
    if (code == 200) {
      found = extractLyricsFromResponse(http, "plainLyrics", "syncedLyrics",
                                        finalLyrics);
    } else {
      requestFailed = requestFailed || code <= 0;
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("LRCLib HTTP Error: ") + String(code) +
                                 " (Track: " + trackTitle + ")",
                             "fetchLyricsIfNeeded");
      Serial.printf("  -> LRCLib HTTP Error %d\n", code);
    }
    closeLyricsRequest(http, client);
  }

  if (networkAbortRequested() || WiFi.status() != WL_CONNECTED)
    return LYRICS_ERROR;

  if (found) {
    RuntimeDiagnostics::markPhase(
        "lyrics/file-save", BackgroundWorker::getStackHighWaterMark());
    String filename = "/lyrics/" + String(releaseMbid) + "/" +
                      padTrackNumber(trackNumber) + ".json";
    Serial.printf("Saving lyrics to %s, length: %d\n", filename.c_str(),
                  (int)finalLyrics.length());
    if (!Storage.saveLyrics(filename.c_str(), finalLyrics)) {
      ErrorHandler::logError(ERR_CAT_STORAGE, "Could not save downloaded lyrics",
                             "fetchLyricsIfNeeded");
      return LYRICS_ERROR;
    }

    tl = Storage.loadTracklist(releaseMbid);
    if (!tl || trackIndex < 0 || trackIndex >= (int)tl->tracks.size()) {
      if (tl)
        Storage.deleteTracklist(tl);
      return LYRICS_ERROR;
    }
    tl->tracks[trackIndex].lyrics.status = "cached";
    tl->tracks[trackIndex].lyrics.path = filename.c_str();
    tl->tracks[trackIndex].lyrics.offset = 0;
    RuntimeDiagnostics::markPhase(
        "lyrics/index-save", BackgroundWorker::getStackHighWaterMark());
    const bool tracklistSaved = Storage.saveTracklist(releaseMbid, tl);
    Serial.printf("  -> Found & Saved to %s\n", filename.c_str());
    Storage.deleteTracklist(tl);
    return tracklistSaved ? LYRICS_FETCHED_NOW : LYRICS_ERROR;
  } else {
    // A connection/TLS failure is not evidence that lyrics do not exist. Do
    // not persist "missing" and do not let a bulk job immediately open yet
    // another TLS connection after both providers were unreachable.
    if (!providerReached || requestFailed) {
      Serial.println("  -> Lyrics providers were not reachable; stopping");
      return LYRICS_ERROR;
    }
    tl = Storage.loadTracklist(releaseMbid);
    if (tl && trackIndex >= 0 && trackIndex < (int)tl->tracks.size()) {
      tl->tracks[trackIndex].lyrics.status = "missing";
      Storage.saveTracklist(releaseMbid, tl);
    }
    if (tl)
      Storage.deleteTracklist(tl);
    Serial.println("  -> Not found in any provider");
    return LYRICS_NOT_FOUND;
  }
}

namespace {

struct LyricsProbeOutcome {
  int httpCode = 0;
  int tlsError = 0;
  uint32_t elapsedMs = 0;
  String address;
  String detail;
};

bool runLyricsDiagnosticProbe(const char *host, const char *url,
                              const char *primaryField,
                              const char *fallbackField,
                              const char *phasePrefix,
                              LyricsProbeOutcome &outcome) {
  outcome = LyricsProbeOutcome{};
  if (networkAbortRequested()) {
    outcome.detail = "cancelled";
    return false;
  }

  IPAddress resolved;
  RuntimeDiagnostics::markPhase(
      phasePrefix, BackgroundWorker::getStackHighWaterMark());
  if (WiFi.hostByName(host, resolved) != 1) {
    outcome.detail = "DNS failed";
    return false;
  }
  outcome.address = resolved.toString();

  HTTPClient http;
  WiFiClientSecure client;
  if (!canStartLyricsTlsRequest()) {
    outcome.detail = WiFi.status() == WL_CONNECTED
                         ? "TLS memory guard stopped request"
                         : "WiFi disconnected";
    return false;
  }
  if (!configureTrustedTlsClient(client)) {
    outcome.detail = "TLS clock unavailable";
    return false;
  }

  client.setHandshakeTimeout(15);
  client.setTimeout(15000);
  if (!http.begin(client, url)) {
    client.stop();
    outcome.detail = "request setup failed";
    return false;
  }
  http.useHTTP10(true);
  http.setReuse(false);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0 diagnostics");
  http.addHeader("Connection", "close");
  http.setConnectTimeout(15000);
  http.setTimeout(15000);

  const uint32_t startedAt = millis();
  outcome.httpCode = http.GET();
  outcome.elapsedMs = millis() - startedAt;

  char tlsErrorText[96] = {0};
  if (outcome.httpCode < 0)
    outcome.tlsError = client.lastError(tlsErrorText, sizeof(tlsErrorText));

  PsramString lyrics;
  const bool parsed =
      outcome.httpCode == HTTP_CODE_OK &&
      extractLyricsFromResponse(http, primaryField, fallbackField, lyrics);

  if (parsed) {
    outcome.detail = "OK";
  } else if (outcome.httpCode == HTTP_CODE_OK) {
    outcome.detail = lastLyricsResponseError[0]
                         ? String(lastLyricsResponseError)
                         : String("response parse failed");
  } else if (outcome.httpCode < 0) {
    outcome.detail = HTTPClient::errorToString(outcome.httpCode);
    if (outcome.tlsError != 0) {
      outcome.detail += " / TLS " + String(outcome.tlsError);
      if (tlsErrorText[0])
        outcome.detail += " " + String(tlsErrorText);
    }
  } else {
    outcome.detail = "HTTP " + String(outcome.httpCode);
  }

  closeLyricsRequest(http, client);
  return parsed;
}

String formatProbeFailure(const char *provider,
                          const LyricsProbeOutcome &outcome) {
  String text = String(provider) + ": " + outcome.detail;
  if (outcome.address.length() > 0)
    text += " @" + outcome.address;
  if (outcome.elapsedMs > 0)
    text += " " + String(outcome.elapsedMs) + "ms";
  return text;
}

} // namespace

bool MediaManager::stressTestLyricsTransport(int cycles,
                                             String &resultMessage) {
  cycles = constrain(cycles, 1, 20);
  if (WiFi.status() != WL_CONNECTED) {
    resultMessage = "No WiFi connection";
    return false;
  }

  const char *lrclibProbeUrl =
      "https://lrclib.net/api/get?artist_name=Coldplay&track_name=Yellow&album_name=Parachutes";
  const char *lyricsOvhProbeUrl =
      "https://api.lyrics.ovh/v1/Coldplay/Yellow";
  uint32_t lowestLargestBlock = UINT32_MAX;
  int recoveredRetries = 0;
  int providerFallbacks = 0;

  for (int cycle = 0; cycle < cycles; ++cycle) {
    if (networkAbortRequested()) {
      resultMessage = "Stress test cancelled";
      RuntimeDiagnostics::clearOperation();
      return false;
    }

    RuntimeDiagnostics::beginOperation(JOB_DIAGNOSTIC_LYRICS_STRESS,
                                       cycle + 1, cycles, "LRCLIB probe");
    BackgroundWorker::reportStatus("TLS stress: cycle " +
                                   String(cycle + 1) + " of " +
                                   String(cycles));
    BackgroundWorker::reportProgress((float)cycle / (float)cycles);

    LyricsProbeOutcome lrclibOutcome;
    RuntimeDiagnostics::markPhase(
        "stress/lrclib", BackgroundWorker::getStackHighWaterMark());
    bool parsed = runLyricsDiagnosticProbe(
        "lrclib.net", lrclibProbeUrl, "plainLyrics", "syncedLyrics",
        "stress/lrclib-dns", lrclibOutcome);

    if (!parsed && !networkAbortRequested() &&
        WiFi.status() == WL_CONNECTED) {
      BackgroundWorker::reportStatus("Cycle " + String(cycle + 1) +
                                     ": retrying LRCLIB...");
      if (!waitForLyricsNetworkCooldown(1000, 3000)) {
        RuntimeDiagnostics::clearOperation();
        resultMessage = "Stress test cancelled or WiFi disconnected";
        return false;
      }
      parsed = runLyricsDiagnosticProbe(
          "lrclib.net", lrclibProbeUrl, "plainLyrics", "syncedLyrics",
          "stress/lrclib-retry", lrclibOutcome);
      if (parsed)
        recoveredRetries++;
    }

    LyricsProbeOutcome ovhOutcome;
    if (!parsed && !networkAbortRequested() &&
        WiFi.status() == WL_CONNECTED) {
      BackgroundWorker::reportStatus("Cycle " + String(cycle + 1) +
                                     ": trying Lyrics.ovh...");
      if (!waitForLyricsNetworkCooldown(1000, 3000)) {
        RuntimeDiagnostics::clearOperation();
        resultMessage = "Stress test cancelled or WiFi disconnected";
        return false;
      }
      parsed = runLyricsDiagnosticProbe(
          "api.lyrics.ovh", lyricsOvhProbeUrl, "lyrics", nullptr,
          "stress/ovh-dns", ovhOutcome);
      if (parsed)
        providerFallbacks++;
    }

    if (!parsed) {
      resultMessage = "Cycle " + String(cycle + 1) + " failed. " +
                      formatProbeFailure("LRCLIB", lrclibOutcome);
      if (ovhOutcome.detail.length() > 0)
        resultMessage += " | " + formatProbeFailure("OVH", ovhOutcome);
      const int32_t rssi = WiFi.RSSI();
      resultMessage += " | " + WiFi.SSID() + " " + String(rssi) + "dBm";
      if (rssi <= -75)
        resultMessage += "; weak signal - select a stronger saved network";
      RuntimeDiagnostics::clearOperation();
      return false;
    }
    if (!heap_caps_check_integrity_all(true)) {
      resultMessage = "Heap corruption detected at cycle " +
                      String(cycle + 1);
      RuntimeDiagnostics::clearOperation();
      return false;
    }

    const uint32_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lowestLargestBlock = std::min(lowestLargestBlock, largest);
    RuntimeDiagnostics::markPhase(
        "stress/cycle-done", BackgroundWorker::getStackHighWaterMark());
    RuntimeDiagnostics::clearOperation();
    if (cycle + 1 < cycles &&
        !waitForLyricsNetworkCooldown(1000, 3000)) {
      resultMessage = "Stress test cancelled or WiFi disconnected";
      return false;
    }
  }

  BackgroundWorker::reportProgress(1.0f);
  resultMessage = "Passed " + String(cycles) +
                  " TLS cycles; lowest block " +
                  String(lowestLargestBlock / 1024) + "K";
  if (recoveredRetries > 0)
    resultMessage += "; retries " + String(recoveredRetries);
  if (providerFallbacks > 0)
    resultMessage += "; OVH fallback " + String(providerFallbacks);
  resultMessage += "; WiFi " + String(WiFi.RSSI()) + "dBm";
  return true;
}

void fetchAllLyrics(const char *releaseMbid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("fetchAllLyrics: No WiFi");
    return;
  }

  // Enqueue background job instead of blocking UI
  BackgroundJob job;
  job.type = JOB_LYRICS_FETCH_ALL;
  job.id = releaseMbid;
  BackgroundWorker::addJob(job);
  Serial.printf("Enqueued lyrics fetch for: %s\n", releaseMbid);
}

void MediaManager::sortByLedIndex() {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_CD:
    std::sort(cdLibrary.begin(), cdLibrary.end(), [](const CD &a, const CD &b) {
      int aLed = a.ledIndices.empty() ? 9999 : a.ledIndices[0];
      int bLed = b.ledIndices.empty() ? 9999 : b.ledIndices[0];
      return aLed < bLed;
    });
    break;
  case MODE_BOOK:
    std::sort(bookLibrary.begin(), bookLibrary.end(),
              [](const Book &a, const Book &b) {
                int aLed = a.ledIndices.empty() ? 9999 : a.ledIndices[0];
                int bLed = b.ledIndices.empty() ? 9999 : b.ledIndices[0];
                return aLed < bLed;
              });
    break;
  default:
    break;
  }

  // REBUILD CACHE after sorting to avoid indexing mismatches
  rebuildNavigationCache(getCurrentItemIndex());
  saveLibrary();
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}
