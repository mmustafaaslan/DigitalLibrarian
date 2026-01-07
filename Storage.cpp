#include "Storage.h"
#include "AppGlobals.h"
#include "ErrorHandler.h"
#include "Utils.h"
#include "waveshare_sd_card.h" // For SD_CS and sdExpander
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

LibrarianStorage Storage;

LibrarianStorage::LibrarianStorage() {
  // Constructor
}

bool LibrarianStorage::begin() {
  // SD Card initialization is handled in setup() for now via
  // waveshare_sd_card.h We assume SD.begin() has already been called.

  // Ensure separate directories exist
  if (!SD.exists("/db"))
    SD.mkdir("/db");
  if (!SD.exists("/db/cds"))
    SD.mkdir("/db/cds");
  if (!SD.exists("/db/books"))
    SD.mkdir("/db/books");
  // Chapters directory creation removed

  return true;
}

// --- Helper: Path Generation ---
String LibrarianStorage::getFilePath(String uniqueID, MediaMode mode) {
  String safeID = sanitizeFilename(uniqueID);
  switch (mode) {
  case MODE_CD:
    return "/db/cds/" + safeID + ".json";
  case MODE_BOOK:
    return "/db/books/" + safeID + ".json";
  default:
    return "/db/unknown/" + safeID + ".json";
  }
}

String LibrarianStorage::getIndexPath(MediaMode mode) {
  switch (mode) {
  case MODE_CD:
    return "/db/cd_index.jsonl";
  case MODE_BOOK:
    return "/db/book_index.jsonl";
  default:
    return "/db/unknown_index.jsonl";
  }
}

IndexVector &LibrarianStorage::getVectorForMode(MediaMode mode) {
  switch (mode) {
  case MODE_BOOK:
    return _bookIndex;
  case MODE_CD:
  default:
    return _cdIndex;
  }
}

IndexVector &LibrarianStorage::getIndex() {
  return getVectorForMode(currentMode);
}

// --- SAVE (Core Function) ---

// --- SAVE (Core Function) ---
bool LibrarianStorage::saveCD(const CD &cd, const char *oldUniqueID,
                              bool skipIndexRewrite) {
  if (oldUniqueID && strlen(oldUniqueID) > 0 && cd.uniqueID != oldUniqueID) {
    String oldPath = getFilePath(oldUniqueID, MODE_CD);
    if (sdExpander && i2cMutex) {
      if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
        sdExpander->digitalWrite(SD_CS, LOW);
        if (SD.exists(oldPath)) {
          SD.remove(oldPath);
          Serial.printf("Storage: Cleaned up old ID file: %s\n",
                        oldPath.c_str());
        }
        sdExpander->digitalWrite(SD_CS, HIGH);
        xSemaphoreGiveRecursive(i2cMutex);
      }
    }
  }

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
      sdExpander->digitalWrite(SD_CS, LOW);
    }
  }

  String path = getFilePath(cd.uniqueID.c_str(), MODE_CD);
  String tmpPath = path + ".tmp";

  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  // 1. Save Detail JSON to TMP
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file) {
    ErrorHandler::logError(
        ERR_CAT_STORAGE, String("Failed to open file for writing: ") + tmpPath,
        "Storage::saveCD");
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    return false;
  }

  DynamicJsonDocument doc(4096); // 4KB is plenty for one item
  doc["title"] = cd.title.c_str();
  doc["artist"] = cd.artist.c_str();
  doc["genre"] = cd.genre.c_str();
  doc["year"] = cd.year;
  doc["uniqueID"] = cd.uniqueID.c_str();
  doc["coverUrl"] = cd.coverUrl.c_str();
  doc["coverFile"] = cd.coverFile.c_str();
  doc["favorite"] = cd.favorite;
  doc["notes"] = cd.notes.c_str();
  doc["barcode"] = cd.barcode.c_str();
  doc["releaseMbid"] = cd.releaseMbid.c_str();
  Serial.printf("Storage: Saving CD %s (MBID: '%s', Tracks: %d, Cover: '%s')\n",
                cd.uniqueID.c_str(), cd.releaseMbid.c_str(), cd.trackCount,
                cd.coverFile.c_str());

  doc["trackCount"] = cd.trackCount;
  doc["totalDurationMs"] = cd.totalDurationMs;

  JsonArray leds = doc.createNestedArray("ledIndices");
  for (int led : cd.ledIndices) {
    leds.add(led);
  }

  serializeJson(doc, file);
  file.close();

  // ATOMIC SWAP: Remove old file, Rename tmp -> actual
  if (SD.exists(path)) {
    SD.remove(path);
  }
  if (!SD.rename(tmpPath, path)) {
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Atomic rename failed: ") + tmpPath + " -> " +
                               path,
                           "Storage::saveCD");
  }

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  // 2. Update Index
  auto &vec = getVectorForMode(MODE_CD);
  bool found = false;
  for (auto &item : vec) {
    if (item.uniqueID == cd.uniqueID.c_str() ||
        (oldUniqueID && strlen(oldUniqueID) > 0 &&
         item.uniqueID == oldUniqueID)) {
      // Update existing (including ID if it changed)
      item.uniqueID = cd.uniqueID.c_str();
      item.title = cd.title.c_str();
      item.artist = cd.artist.c_str();
      item.coverFile = cd.coverFile.c_str();
      item.year = cd.year;
      item.genre = cd.genre.c_str();
      item.favorite = cd.favorite;
      item.ledIndices.assign(cd.ledIndices.begin(), cd.ledIndices.end());
      item.metaInt = cd.trackCount;
      item.metaString = cd.barcode.c_str();
      found = true;
      break;
    }
  }

  if (!found) {
    LibraryIndexItem newItem;
    newItem.uniqueID = cd.uniqueID.c_str();
    newItem.title = cd.title.c_str();
    newItem.artist = cd.artist.c_str();
    newItem.coverFile = cd.coverFile.c_str();
    newItem.year = cd.year;
    newItem.genre = cd.genre.c_str();
    newItem.favorite = cd.favorite;
    newItem.ledIndices.assign(cd.ledIndices.begin(), cd.ledIndices.end());
    newItem.metaInt = cd.trackCount;
    newItem.metaString = cd.barcode.c_str();
    vec.push_back(newItem);
  }

  if (skipIndexRewrite)
    return true;
  return rewriteIndex(MODE_CD);
}

// --- LOAD INDEX ---
bool LibrarianStorage::loadIndex(MediaMode mode) {
  auto &vec = getVectorForMode(mode);
  vec.clear();
  String path = getIndexPath(mode);

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
      sdExpander->digitalWrite(SD_CS, LOW);
    }
  }
  File file = SD.open(path, FILE_READ);

  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false; // No index yet
  }

  // Read Line-By-Line (JSONL)
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;

    StaticJsonDocument<1024> doc; // Increased size for robust loading
    DeserializationError error = deserializeJson(doc, line);

    if (!error) {
      LibraryIndexItem item;
      item.uniqueID = (const char *)(doc["id"] | "");
      item.title = (const char *)(doc["t"] | ""); // Short keys for index
      item.artist = (const char *)(doc["a"] | "");
      item.coverFile = (const char *)(doc["c"] | "");
      item.year = doc["y"] | 0;
      item.genre = (const char *)(doc["g"] | "");
      item.favorite = doc["f"] | false;
      item.metaInt = doc["mi"] | 0;                     // NEW: Meta Int
      item.metaString = (const char *)(doc["ms"] | ""); // NEW: Meta String

      JsonArray leds = doc["l"];
      for (int val : leds)
        item.ledIndices.push_back(val);

      vec.push_back(item);
    }
  }

  file.close();
  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }
  return true;
}

// --- REWRITE INDEX FILE ---
bool LibrarianStorage::rewriteIndex(MediaMode mode) {
  auto &vec = getVectorForMode(mode);
  String path = getIndexPath(mode);
  String tmpPath = path + ".tmp";

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5000)) != pdPASS) {
      return false;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }

  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  // Write to TMP
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    return false;
  }

  for (const auto &item : vec) {
    StaticJsonDocument<1024> doc; // Increased size to prevent truncation
    doc["id"] = item.uniqueID.c_str();
    doc["t"] = item.title.c_str();
    doc["a"] = item.artist.c_str();
    doc["c"] = item.coverFile.c_str();
    doc["y"] = item.year;
    doc["g"] = item.genre.c_str();
    doc["f"] = item.favorite;
    doc["mi"] = item.metaInt;
    doc["ms"] = item.metaString.c_str();

    JsonArray leds = doc.createNestedArray("l");
    for (int val : item.ledIndices)
      leds.add(val);

    serializeJson(doc, file);
    file.println(); // Newline for JSONL
  }

  file.close();

  // Atomic Swap
  if (SD.exists(path))
    SD.remove(path);
  if (!SD.rename(tmpPath, path)) {
    Serial.println("Storage: Index Atomic Rename FAILED!");
  }

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH); // DESELECT
    xSemaphoreGiveRecursive(i2cMutex);
  }
  return true;
}

bool LibrarianStorage::loadCDDetail(String uniqueID, CD &outCD) {
  String path = getFilePath(uniqueID, MODE_CD);
  Serial.printf("Storage: Loading CD Detail: %s\n", path.c_str());

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) != pdPASS) {
      Serial.println("!!! I2C LOCK FAIL: loadCDDetail");
      return false;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }
  File file = SD.open(path, FILE_READ);
  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, file);
  file.close();
  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  outCD.uniqueID = uniqueID.c_str();
  outCD.title = (const char *)(doc["title"] | "");
  outCD.artist = (const char *)(doc["artist"] | "");
  outCD.genre = (const char *)(doc["genre"] | "");
  outCD.year = doc["year"] | 0;
  outCD.coverUrl = (const char *)(doc["coverUrl"] | "");
  outCD.coverFile = (const char *)(doc["coverFile"] | "");
  outCD.favorite = doc["favorite"] | false;
  outCD.notes = (const char *)(doc["notes"] | "");
  outCD.barcode = (const char *)(doc["barcode"] | "");
  outCD.releaseMbid = (const char *)(doc["releaseMbid"] | "");
  outCD.trackCount = doc["trackCount"] | 0;
  outCD.totalDurationMs = doc["totalDurationMs"] | 0;

  outCD.ledIndices.clear();
  JsonArray leds = doc["ledIndices"];
  for (int val : leds)
    outCD.ledIndices.push_back(val);

  Serial.printf("Storage: Loaded CD %s details. ReleaseMbid: '%s', Cover: "
                "'%s', LEDs: %d\n",
                uniqueID.c_str(), outCD.releaseMbid.c_str(),
                outCD.coverFile.c_str(), (int)outCD.ledIndices.size());

  outCD.detailsLoaded = true;

  return true;
}

// Implement Book functions if needed... (Skipped for initial CD migration
// focus) Stub for compilation
// --- SAVE BOOK ---
bool LibrarianStorage::saveBook(const Book &book, const char *oldUniqueID,
                                bool skipIndexRewrite) {
  if (oldUniqueID && strlen(oldUniqueID) > 0 && book.uniqueID != oldUniqueID) {
    String oldPath = getFilePath(oldUniqueID, MODE_BOOK);
    if (sdExpander && i2cMutex) {
      if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
        sdExpander->digitalWrite(SD_CS, LOW);
        if (SD.exists(oldPath)) {
          SD.remove(oldPath);
          Serial.printf("Storage: Cleaned up old ID file: %s\n",
                        oldPath.c_str());
        }
        sdExpander->digitalWrite(SD_CS, HIGH);
        xSemaphoreGiveRecursive(i2cMutex);
      }
    }
  }

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) != pdPASS) {
      Serial.println("!!! I2C LOCK FAIL: saveBook");
      return false;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }

  String path = getFilePath(book.uniqueID.c_str(), MODE_BOOK);
  String tmpPath = path + ".tmp";

  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  // 1. Save Detail JSON to TMP
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  doc["title"] = book.title.c_str();
  doc["artist"] =
      book.author.c_str(); // Store Author in "artist" field for consistency
  doc["author"] = book.author.c_str(); // Explicit
  doc["genre"] = book.genre.c_str();
  doc["year"] = book.year;
  doc["uniqueID"] = book.uniqueID.c_str();
  doc["coverUrl"] = book.coverUrl.c_str();
  doc["coverFile"] = book.coverFile.c_str();
  doc["favorite"] = book.favorite;
  doc["notes"] = book.notes.c_str();
  doc["isbn"] = book.isbn.c_str();
  doc["publisher"] = book.publisher.c_str();
  doc["pageCount"] = book.pageCount;
  doc["currentPage"] = book.currentPage;

  JsonArray leds = doc.createNestedArray("ledIndices");
  for (int led : book.ledIndices) {
    leds.add(led);
  }

  serializeJson(doc, file);
  file.close();

  // ATOMIC SWAP: Remove old file, Rename tmp -> actual
  if (SD.exists(path)) {
    SD.remove(path);
  }
  if (!SD.rename(tmpPath, path)) {
    Serial.println("Storage: Atomic Rename FAILED (Book)!");
  }

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  // 2. Update Index (RAM)
  auto &vec = getVectorForMode(MODE_BOOK);
  bool found = false;
  for (auto &item : vec) {
    if (item.uniqueID == book.uniqueID.c_str() ||
        (oldUniqueID && strlen(oldUniqueID) > 0 &&
         item.uniqueID == oldUniqueID)) {
      item.uniqueID = book.uniqueID.c_str();
      item.title = book.title.c_str();
      item.artist = book.author.c_str(); // Map Author -> Artist for Index
      item.coverFile = book.coverFile.c_str();
      item.year = book.year;
      item.genre = book.genre.c_str();
      item.favorite = book.favorite;
      item.ledIndices.assign(book.ledIndices.begin(), book.ledIndices.end());
      item.metaInt = book.pageCount;       // NEW: Page Count
      item.metaString = book.isbn.c_str(); // NEW: ISBN
      found = true;
      break;
    }
  }

  if (!found) {
    LibraryIndexItem newItem;
    newItem.uniqueID = book.uniqueID.c_str();
    newItem.title = book.title.c_str();
    newItem.artist = book.author.c_str();
    newItem.coverFile = book.coverFile.c_str();
    newItem.year = book.year;
    newItem.genre = book.genre.c_str();
    newItem.favorite = book.favorite;
    newItem.ledIndices.assign(book.ledIndices.begin(), book.ledIndices.end());
    newItem.metaInt = book.pageCount;       // NEW: Page Count
    newItem.metaString = book.isbn.c_str(); // NEW: ISBN
    vec.push_back(newItem);
  }

  if (skipIndexRewrite)
    return true;
  return rewriteIndex(MODE_BOOK);
}

// --- LOAD BOOK DETAIL ---
bool LibrarianStorage::loadBookDetail(String uniqueID, Book &outBook) {
  String path = getFilePath(uniqueID, MODE_BOOK);
  Serial.printf("Storage: Loading Book Detail: %s\n", path.c_str());

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) != pdPASS) {
      Serial.println("!!! I2C LOCK FAIL: loadBookDetail");
      return false;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }
  File file = SD.open(path, FILE_READ);
  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, file);
  file.close();
  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  outBook.uniqueID = uniqueID.c_str();
  outBook.title = (const char *)(doc["title"] | "");
  outBook.author = (const char *)(doc["author"] | doc["artist"] | "");
  outBook.genre = (const char *)(doc["genre"] | "");
  outBook.year = doc["year"] | 0;
  outBook.coverUrl = (const char *)(doc["coverUrl"] | "");
  outBook.coverFile = (const char *)(doc["coverFile"] | "");
  outBook.favorite = doc["favorite"] | false;
  outBook.notes = (const char *)(doc["notes"] | "");
  outBook.isbn = (const char *)(doc["isbn"] | "");
  outBook.publisher = (const char *)(doc["publisher"] | "");
  outBook.pageCount = doc["pageCount"] | 0;
  outBook.currentPage = doc["currentPage"] | 0;

  outBook.ledIndices.clear();
  JsonArray leds = doc["ledIndices"];
  for (int val : leds)
    outBook.ledIndices.push_back(val);

  Serial.printf("Storage: Loaded Book %s details (Publisher: '%s', Cover: "
                "'%s', LEDs: %d)\n",
                uniqueID.c_str(), outBook.publisher.c_str(),
                outBook.coverFile.c_str(), (int)outBook.ledIndices.size());

  outBook.detailsLoaded = true;

  return true;
}

// Stub for delete (can implement later)
// --- DELETE ITEM ---
bool LibrarianStorage::deleteItem(String uniqueID, MediaMode mode) {
  String path = getFilePath(uniqueID, mode);
  Serial.printf("Storage: Deleting %s\n", path.c_str());

  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    if (SD.exists(path)) {
      SD.remove(path);
    } else {
      Serial.println("Storage: File not found (might already be deleted). "
                     "Continuing to remove from index.");
    }

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  // Remove from RAM Index
  auto &vec = getVectorForMode(mode);
  for (auto it = vec.begin(); it != vec.end(); ++it) {
    if (it->uniqueID == uniqueID.c_str()) {
      vec.erase(it);
      break;
    }
  }

  // Persist Index Update
  return rewriteIndex(mode);
}

bool LibrarianStorage::wipeLibrary(MediaMode mode) {
  String indexFile;
  String dataDir;

  if (mode == MODE_CD) {
    indexFile = "/db/cd_index.jsonl";
    dataDir = "/db/cds";
  } else {
    indexFile = "/db/book_index.jsonl";
    dataDir = "/db/books";
  }

  Serial.printf("⚠️ Wiping Library Data: %s\n", dataDir.c_str());

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5000)) != pdPASS) {
      return false;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }

  // 1. Delete Index File
  if (SD.exists(indexFile)) {
    SD.remove(indexFile);
    Serial.println("Deleted index file.");
  }

  // 2. Delete All Data Files
  File dir = SD.open(dataDir);
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile();
    while (file) {
      String fileName = String(file.name());
      String fullPath;
      if (fileName.startsWith("/")) {
        fullPath = fileName;
      } else {
        fullPath = dataDir + "/" + fileName;
      }

      bool isDir = file.isDirectory();
      file.close();

      if (!isDir) {
        SD.remove(fullPath);
        Serial.printf("Deleted: %s\n", fullPath.c_str());
      }

      file = dir.openNextFile();
    }
    dir.close();
  } else {
    if (dir)
      dir.close();
  }

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  // 3. Clear RAM Index
  getVectorForMode(mode).clear();

  return true;
}

// ============================================================================
// TRACKLIST MANAGEMENT
// ============================================================================

TrackList *LibrarianStorage::loadTracklist(const char *releaseMbid) {
  if (!releaseMbid || strlen(releaseMbid) == 0) {
    Serial.println("Storage: Invalid releaseMbid for trackload");
    return nullptr;
  }

  String filename = "/tracks/" + String(releaseMbid) + ".json";

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) != pdPASS) {
      return nullptr;
    }
    sdExpander->digitalWrite(SD_CS, LOW);
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return nullptr;
  }

  // Use PSRAM for the large JSON buffer (64KB)
  // Converting 'file' to 'stream' avoids loading the whole string into Internal
  // Heap
  BasicJsonDocument<SpiRamAllocator> doc(65536);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  if (error) {
    Serial.printf("Storage: Tracklist JSON Error: %s\n", error.c_str());
    return nullptr;
  }

  TrackList *trackList = new TrackList();

  // Safe extraction directly from JSON Document (PSRAM)
  trackList->releaseMbid = releaseMbid;
  trackList->cdTitle = doc["cdTitle"] | "";
  trackList->cdArtist = doc["cdArtist"] | "";
  trackList->fetchedAt = doc["fetchedAt"] | "";

  JsonArray tracks = doc["tracks"];
  for (JsonObject t : tracks) {
    Track track;
    track.trackNo = t["trackNo"];
    track.title = t["title"] | "";
    track.durationMs = t["durationMs"];
    track.recordingMbid = t["recordingMbid"] | "";
    track.isFavoriteTrack = t["isFavoriteTrack"] | false;

    if (t.containsKey("lyrics")) {
      JsonObject lyr = t["lyrics"];
      track.lyrics.status = lyr["status"] | "unchecked";
      track.lyrics.path = lyr["path"] | "";
      String pathStr = track.lyrics.path.c_str();
      pathStr.replace("\\/", "/"); // Legacy cleanup if needed
      track.lyrics.path = pathStr.c_str();
      track.lyrics.fetchedAt = lyr["fetchedAt"] | "";
      track.lyrics.lastTriedAt = lyr["lastTriedAt"] | "";
      track.lyrics.lang = lyr["lang"] | "";
      track.lyrics.error = lyr["error"] | "";
    } else {
      track.lyrics.status = "unchecked";
    }

    trackList->tracks.push_back(track);
  }

  return trackList;
}

bool LibrarianStorage::saveTracklist(const char *releaseMbid,
                                     TrackList *trackList) {
  if (!trackList || !releaseMbid)
    return false;

  bool mutexTaken = false;
  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) == pdPASS) {
      sdExpander->digitalWrite(SD_CS, LOW);
      mutexTaken = true;
    }
  }

  if (!SD.exists("/tracks")) {
    SD.mkdir("/tracks");
  }

  String filename = "/tracks/" + String(releaseMbid) + ".json";
  if (SD.exists(filename)) {
    SD.remove(filename);
  }

  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    if (sdExpander && i2cMutex && mutexTaken) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false;
  }

  // Stream JSON directly to file to save Heap
  file.print("{");
  file.printf("\"releaseMbid\":\"%s\",", releaseMbid);
  file.print("\"cdTitle\":\"" + escapeJSON(trackList->cdTitle.c_str()) + "\",");
  file.print("\"cdArtist\":\"" + escapeJSON(trackList->cdArtist.c_str()) +
             "\",");
  file.printf("\"fetchedAt\":\"%s\",", trackList->fetchedAt.c_str());
  file.print("\"tracks\":[");

  for (size_t i = 0; i < trackList->tracks.size(); i++) {
    Track &track = trackList->tracks[i];
    if (i > 0)
      file.print(",");

    file.print("{");
    file.printf("\"trackNo\":%d,", track.trackNo);
    file.print("\"title\":\"" + escapeJSON(track.title.c_str()) + "\",");
    file.printf("\"durationMs\":%lu,", track.durationMs);
    file.print("\"recordingMbid\":\"" + String(track.recordingMbid.c_str()) +
               "\",");

    file.print("\"lyrics\":{");
    file.print("\"status\":\"" + String(track.lyrics.status.c_str()) + "\"");

    // String comparisons with PsramString work if PsramString is std::string
    if (track.lyrics.status == "cached") {
      file.print(",\"path\":\"" + escapeJSON(track.lyrics.path.c_str()) + "\"");
      file.print(",\"fetchedAt\":\"" + String(track.lyrics.fetchedAt.c_str()) +
                 "\"");
      file.print(",\"lang\":\"" + String(track.lyrics.lang.c_str()) + "\"");
    } else if (track.lyrics.status == "missing") {
      file.print(",\"lastTriedAt\":\"" +
                 String(track.lyrics.lastTriedAt.c_str()) + "\"");
      file.print(",\"error\":\"" + escapeJSON(track.lyrics.error.c_str()) +
                 "\"");
    }
    file.print("},");
    file.printf("\"isFavoriteTrack\":%s",
                track.isFavoriteTrack ? "true" : "false");
    file.print("}");
  }
  file.print("]}");
  file.close();

  if (sdExpander && i2cMutex && mutexTaken) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  return true;
}

void LibrarianStorage::deleteTracklist(TrackList *trackList) {
  if (trackList) {
    trackList->tracks.clear();
    delete trackList;
  }
}

// ============================================================================
// CHAPTER MANAGEMENT
// ============================================================================

// Chapter code removed

// ============================================================================
// LYRICS MANAGEMENT
// ============================================================================

String LibrarianStorage::loadLyrics(const char *lyricsPath) {
  if (!lyricsPath || strlen(lyricsPath) == 0)
    return "";

  String path = String(lyricsPath);
  if (!path.startsWith("/")) {
    path = "/lyrics/" + path;
  }

  String content = "";
  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) == pdPASS) {
      sdExpander->digitalWrite(SD_CS, LOW);
      if (SD.exists(path)) {
        File file = SD.open(path, FILE_READ);
        if (file) {
          content = file.readString();
          file.close();
        }
      } else {
        // Try root fallback
        if (path.startsWith("/lyrics/")) {
          String rootPath = "/" + String(lyricsPath);
          if (SD.exists(rootPath)) {
            File file = SD.open(rootPath, FILE_READ);
            if (file) {
              content = file.readString();
              file.close();
            }
          }
        }
      }
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
  }

  if (content.length() == 0)
    return "";

  // Use DynamicJsonDocument for safe parsing
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, content);

  if (error) {
    Serial.print("Storage: Failed to parse lyrics JSON: ");
    Serial.println(error.c_str());
    return "";
  }

  return doc["text"].as<String>();
}

bool LibrarianStorage::saveLyrics(const char *lyricsPath, String lyricsText,
                                  String lang) {
  if (!lyricsPath)
    return false;

  String path = String(lyricsPath);
  if (!path.startsWith("/")) {
    path = "/lyrics/" + path;
  }

  // Create directory if needed
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String dir = path.substring(0, lastSlash);
    if (sdExpander && i2cMutex) {
      if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) == pdPASS) {
        sdExpander->digitalWrite(SD_CS, LOW);
        if (!SD.exists(dir)) {
          SD.mkdir(dir);
        }
        sdExpander->digitalWrite(SD_CS, HIGH);
        xSemaphoreGiveRecursive(i2cMutex);
      }
    }
  }

  if (sdExpander && i2cMutex) {
    if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) == pdPASS) {
      sdExpander->digitalWrite(SD_CS, LOW);
    }
  }

  // Use O_TRUNC equivalent by using FILE_WRITE which on ESP32 SD usually
  // appends? No, SD lib wrapper usually seeks to end. Best to remove file first
  // to ensure clean write.
  if (SD.exists(path)) {
    SD.remove(path);
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    if (sdExpander && i2cMutex) {
      sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    return false;
  }

  DynamicJsonDocument doc(16384);
  doc["lang"] = lang;
  doc["fetchedAt"] = getCurrentISO8601Timestamp();

  String cleanLyrics = lyricsText;
  decodeHTMLEntities(cleanLyrics);
  doc["text"] = sanitizeText(cleanLyrics);

  serializeJson(doc, file);
  file.close();

  if (sdExpander && i2cMutex) {
    sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  return true;
}
