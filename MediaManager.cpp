#include "MediaManager.h"
#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "NavigationCache.h"
#include "mode_abstraction.h"
#include <FastLED.h>
#include <esp_heap_caps.h>

bool MediaManager::_taskBusy = false;

void MediaManager::init() { _taskBusy = false; }

void MediaManager::syncFromStorage() {
  Storage.loadIndex(MODE_CD);
  Storage.loadIndex(MODE_BOOK);
  syncLibraryFromStorage();
}

void MediaManager::filter(const char *query, int filterMode, bool ledMasterOn) {
  if (query == nullptr)
    return;

  // Clear and reset results
  search_matches.clear();
  search_display_offset = 0;

  String q = String(query);
  q.toLowerCase();

  FastLED.clear();
  if (!ledMasterOn) {
    FastLED.show();
  }

  if (q.length() == 0) {
    FastLED.show();
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
      if (ledMasterOn) {
        for (int idx : item.ledIndices) {
          if (idx >= 0 && idx < led_count) {
            leds[idx] = item.favorite ? COLOR_FAVORITE : COLOR_FILTERED;
          }
        }
      }
    }
  }
  FastLED.show();
}

#include "ErrorHandler.h" // Moved here as it's used in this section
#include "Utils.h"

// ============================================================================
// INTERNAL API HELPERS
// ============================================================================

MBRelease MediaManager::fetchReleaseByBarcode(const char *barcode) {
  MBRelease result;
  result.success = false;

  if (WiFi.status() != WL_CONNECTED) {
    ErrorHandler::logWarn(ERR_CAT_NETWORK, "WiFi not connected",
                          "fetchReleaseByBarcode");
    return result;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);

  String url =
      "https://musicbrainz.org/ws/2/release/?query=barcode:" + String(barcode) +
      "&fmt=json";

  Serial.printf("MediaManager: MusicBrainz Searching barcode %s\n", barcode);

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    http.end();
    client.stop();

    int releasesIdx = payload.indexOf("\"releases\":[");
    if (releasesIdx < 0) {
      Serial.println("MediaManager: No releases found in MusicBrainz, trying "
                     "Discogs fallback...");
      return fetchReleaseFromDiscogs(barcode);
    }

    int releaseStart = payload.indexOf("{", releasesIdx);
    if (releaseStart < 0) {
      Serial.println(
          "MediaManager: Invalid release format, trying Discogs fallback...");
      return fetchReleaseFromDiscogs(barcode);
    }

    result.releaseMbid = extractJSONString(payload, "id", releaseStart);
    result.title = extractJSONString(payload, "title", releaseStart);
    decodeHTMLEntities(result.title);

    // Artist
    int artistIdx = payload.indexOf("\"artist-credit\"", releaseStart);
    if (artistIdx > 0) {
      result.artist = extractJSONString(payload, "name", artistIdx);
      decodeHTMLEntities(result.artist);
      result.artist = toTitleCase(result.artist); // Capitalize first letters
    }

    result.title = toTitleCase(result.title); // Capitalize Album Title

    // Year
    int dateIdx = payload.indexOf("\"date\":\"", releaseStart);
    if (dateIdx > 0) {
      result.year = payload.substring(dateIdx + 8, dateIdx + 12).toInt();
    }

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
    } else {
      // Even if year is present, try to get genre from Discogs
      // since MusicBrainz barcode search doesn't include genre
      Serial.println("MediaManager: Fetching genre from Discogs to supplement "
                     "MusicBrainz...");
      MBRelease discogsData = fetchReleaseFromDiscogs(barcode);
      if (discogsData.success && discogsData.genre.length() > 0) {
        result.genre = discogsData.genre;
        Serial.printf("MediaManager: Got genre from Discogs: %s\n",
                      result.genre.c_str());
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

  if (WiFi.status() != WL_CONNECTED) {
    ErrorHandler::logWarn(ERR_CAT_NETWORK,
                          "WiFi not connected for Discogs fallback",
                          "fetchReleaseFromDiscogs");
    return result;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);

  // Discogs barcode search endpoint with API token
  String url =
      "https://api.discogs.com/database/search?barcode=" + String(barcode) +
      "&type=release&per_page=1&token=" + String(DISCOGS_TOKEN);

  Serial.printf("MediaManager: Discogs Searching barcode %s\n", barcode);

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    http.end();
    client.stop();

    // Use PSRAM for JSON to save Heap
    BasicJsonDocument<SpiRamAllocator> doc(32768);
    DeserializationError error = deserializeJson(doc, payload);

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

  delay(1000); // MusicBrainz rate limit

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);
  client.setTimeout(30000); // 30s TCP timeout (Increase to fix IncompleteInput)

  // Fetch release with included recordings and genres
  String url = "https://musicbrainz.org/ws/2/release/" + String(releaseMbid) +
               "?inc=recordings+genres+tags+release-groups&fmt=json";

  http.begin(client, url);
  http.addHeader("User-Agent", "DigitalLibrarian/1.0");
  http.setTimeout(30000); // Increased timeout for large JSON

  int httpCode = http.GET();
  int contentLen = http.getSize();
  Serial.printf("fetchTracklist: HTTP %d, Content-Length: %d bytes\n", httpCode,
                contentLen);
  Serial.printf("fetchTracklist: Heap Free: %d, PSRAM Free: %d\n",
                ESP.getFreeHeap(), ESP.getFreePsram());

  if (httpCode == 200) {
    // Robust Download to PSRAM Buffer:
    // 1. Allocate buffer in PSRAM
    // 2. Read entire stream
    // 3. Deserialize from buffer

    // Safety check for size
    if (contentLen <= 0) {
      // Chunked transfer or unknown size - fallback to old method or just try
      // reading For MusicBrainz, we usually get a Content-Length. If 0 or -1,
      // we can't safely malloc. But usually MB returns size. Let's assume size
      // is valid for now or default to a safe max for Stream (e.g. 128KB) if
      // unknown.
      if (contentLen < 0)
        contentLen = 128 * 1024; // Cap at 128KB if unknown
    }

    char *psBuffer = (char *)ps_malloc(contentLen + 1);
    if (!psBuffer) {
      Serial.println("fetchTracklist: PSRAM Allocation Failed!");
      http.end();
      client.stop();
      return tracks;
    }

    // Read loop
    WiFiClient *stream = http.getStreamPtr();
    int totalRead = 0;
    unsigned long startRead = millis();
    while (http.connected() && (totalRead < contentLen || contentLen == -1)) {
      size_t avail = stream->available();
      if (avail) {
        int toRead = avail;
        if (contentLen > 0 && totalRead + toRead > contentLen) {
          toRead = contentLen - totalRead;
        }
        int readBytes = stream->readBytes(psBuffer + totalRead, toRead);
        totalRead += readBytes;
      } else {
        delay(10);
        if (millis() - startRead > 30000) { // 30s Hard Timeout
          Serial.println("fetchTracklist: Download Timeout!");
          break;
        }
      }
      if (contentLen > 0 && totalRead >= contentLen)
        break;
    }
    psBuffer[totalRead] = 0; // Null Check

    Serial.printf("fetchTracklist: Downloaded %d bytes to PSRAM\n", totalRead);

    BasicJsonDocument<SpiRamAllocator> doc(98304);
    DeserializationError error = deserializeJson(doc, psBuffer);

    free(psBuffer); // Important!

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
  if (WiFi.status() != WL_CONNECTED)
    return false;

  Serial.printf("Fetching book metadata for ISBN: %s\n", isbn);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  // client.setBufferSizes(1024, 512); // Optimize Buffers

  String url =
      "https://www.googleapis.com/books/v1/volumes?q=isbn:" + String(isbn);
  http.begin(client, url);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    http.end();

    // Use PSRAM for JSON to save Heap
    BasicJsonDocument<SpiRamAllocator> doc(32768);
    if (deserializeJson(doc, payload))
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
  if (!barcode || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // Preservation logic...
  String itemID = outView.uniqueID;
  String preservedCover = outView.coverFile;

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
  if (!release.success)
    return false;

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

  // MusicBrainz rate limit is 1 req/sec. Adding delay before tracklist fetch.
  delay(1000);

  String gen = "";
  std::vector<Track> tracks;
  // Retry tracklist fetch up to 2 times
  for (int i = 0; i < 2; i++) {
    tracks = fetchTracklist(cd.releaseMbid.c_str(), &gen);
    if (!tracks.empty())
      break;
    Serial.printf("fetchMetadata: Track fetch empty, retrying (%d/2)...\n",
                  i + 1);
    delay(1000);
  }

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
  Storage.saveCD(cd);

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
  if (!isbn || WiFi.status() != WL_CONNECTED)
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

  if (!fetchBookByISBN(isbn, book))
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
  Storage.saveBook(book);

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

// Fetch Album Cover URL from iTunes API
String MediaManager::fetchAlbumCoverUrl(const char *artist, const char *album) {
  String coverUrl = "";

  // Try up to 2 times only (reduced from 3 for faster bulk checks)
  for (int attempt = 1; attempt <= 2; attempt++) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(2000); // Reduced to 2s
    client.setTimeout(5000);          // Read timeout

    String searchQuery = String(artist) + " " + String(album);
    String encodedQuery = urlEncode(searchQuery);

    String url = "https://itunes.apple.com/search?term=" + encodedQuery +
                 "&entity=album&limit=1";

    if (attempt == 1) {
      Serial.printf("Fetching URL for: %s - %s\n", artist, album);
    } else
      Serial.printf("  Retry #%d...\n", attempt);

    http.begin(client, url);

    // Add headers to mimic a browser
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, "
                      "like Gecko) Chrome/112.0.0.0 Safari/537.36");
    http.addHeader("Accept", "*/*");
    http.addHeader("Connection", "close");

    // Reduce timeout
    http.setTimeout(2000);

    int httpCode = http.GET();

    if (httpCode == 200) {
      String payload = http.getString();
      int artworkIndex = payload.indexOf("\"artworkUrl100\":\"");
      if (artworkIndex > 0) {
        int startIndex = artworkIndex + 17;
        int endIndex = payload.indexOf("\"", startIndex);
        coverUrl = payload.substring(startIndex, endIndex);

        // Request 240x240 image
        coverUrl.replace("100x100", "240x240");
        Serial.printf("  ✓ Found: %s\n", coverUrl.c_str());
      }
      http.end();
      break; // Success!
    } else if (httpCode == -1 || httpCode == -11) {
      // Timeout or connection error - don't retry
      Serial.printf("  ✗ Timeout/Connection Error: %d - Skipping\n", httpCode);
      http.end();
      break; // Skip this album, don't retry
    } else {
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("iTunes HTTP Error: ") + String(httpCode) +
                                 " (Query: " + String(artist) + " - " +
                                 String(album) + ")",
                             "fetchAlbumCoverUrl");
      Serial.printf("  ✗ HTTP Error: %d\n", httpCode);
      http.end();
    }
    delay(100);
  }

  return coverUrl;
}

#include <algorithm>

void MediaManager::sortByArtistOrAuthor() {
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
}

// ============================================================================
// LYRICS IMPLEMENTATION
// ============================================================================

// Improved fetchLyricsIfNeeded with better timeouts
LyricsResult fetchLyricsIfNeeded(const char *releaseMbid, int trackIndex,
                                 bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("fetchLyricsIfNeeded: No WiFi");
    return LYRICS_NOT_FOUND;
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
    delete tl;
    return LYRICS_NOT_FOUND;
  }

  Track &track = tl->tracks[trackIndex];

  if (!force) {
    if (track.lyrics.status == "cached") {
      delete tl;
      return LYRICS_ALREADY_CACHED;
    }

    if (track.lyrics.status == "missing") {
      delete tl;
      return LYRICS_NOT_FOUND; // Don't retry automatically
    }
  }

  // 2. Fetch from API
  // 2. Fetch from API
  String finalLyrics = "";
  bool found = false;

  // --- STRATEGY 1: LYRICS.OVH ---
  {
    Serial.println("Using Strategy 1: Lyrics.ovh...");
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    // Schema: https://api.lyrics.ovh/v1/artist/title
    String url = "https://api.lyrics.ovh/v1/" +
                 urlEncode(tl->cdArtist.c_str()) + "/" +
                 urlEncode(track.title.c_str());

    Serial.printf("Query URL: %s\n", url.c_str());

    http.begin(client, url);
    http.addHeader("User-Agent", "DigitalLibrarian/1.0");
    int code = http.GET();

    if (code == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(8192); // Slightly larger buffer for full lyrics
      DeserializationError err = deserializeJson(doc, payload);

      if (!err) {
        finalLyrics = doc["lyrics"].as<String>();
        if (finalLyrics.length() > 0)
          found = true;
      }
    } else {
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("Lyrics.ovh HTTP Error: ") + String(code) +
                                 " (Track: " + String(track.title.c_str()) +
                                 ")",
                             "fetchLyricsIfNeeded");
      Serial.printf("  -> Lyrics.ovh HTTP Error %d\n", code);
    }
    http.end();
  }

  // --- STRATEGY 2: LRCLIB (Fallback) ---
  if (!found) {
    Serial.println("  -> Lyrics.ovh failed, trying LRCLib...");
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10000);
    client.setTimeout(10000);

    String url = "https://lrclib.net/api/get?artist_name=" +
                 urlEncode(tl->cdArtist.c_str()) +
                 "&track_name=" + urlEncode(track.title.c_str()) +
                 "&album_name=" + urlEncode(tl->cdTitle.c_str());

    Serial.printf("Query URL (Fallback): %s\n", url.c_str());

    http.begin(client, url);
    http.addHeader("User-Agent", "DigitalLibrarian/1.0");
    http.setTimeout(10000);

    int code = http.GET();
    if (code == 200) {
      String payload = http.getString();
      DynamicJsonDocument doc(4096);
      DeserializationError err = deserializeJson(doc, payload);

      if (!err) {
        finalLyrics = doc["plainLyrics"].as<String>();

        if (finalLyrics.length() == 0 || finalLyrics == "null")
          finalLyrics = doc["syncedLyrics"].as<String>();

        if (finalLyrics.length() > 0 && finalLyrics != "null")
          found = true;
      }
    } else {
      ErrorHandler::logError(ERR_CAT_NETWORK,
                             String("LRCLib HTTP Error: ") + String(code) +
                                 " (Track: " + String(track.title.c_str()) +
                                 ")",
                             "fetchLyricsIfNeeded");
      Serial.printf("  -> LRCLib HTTP Error %d\n", code);
    }
    http.end();
  }

  if (found) {
    String filename = "/lyrics/" + String(releaseMbid) + "/" +
                      padTrackNumber(track.trackNo) + ".json";
    Serial.printf("Saving lyrics to %s, length: %d\n", filename.c_str(),
                  finalLyrics.length());
    Storage.saveLyrics(filename.c_str(), finalLyrics);

    track.lyrics.status = "cached";
    track.lyrics.path = filename.c_str();
    track.lyrics.offset = 0;
    Storage.saveTracklist(releaseMbid, tl);
    Serial.printf("  -> Found & Saved to %s\n", filename.c_str());
    delete tl;
    return LYRICS_FETCHED_NOW;
  } else {
    track.lyrics.status = "missing";
    Storage.saveTracklist(releaseMbid, tl);
    Serial.println("  -> Not found in any provider");
    delete tl;
    return LYRICS_NOT_FOUND;
  }
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
}
