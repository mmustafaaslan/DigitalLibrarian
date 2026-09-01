#include "Storage.h"
#include "AppGlobals.h"
#include "ErrorHandler.h"
#include "Utils.h"
#include "waveshare_sd_card.h" // For SD_CS and sdExpander
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

LibrarianStorage Storage;

namespace {
class ScopedLibraryAccess {
public:
  explicit ScopedLibraryAccess(uint32_t timeoutMs = 5000) : locked(false) {
    if (!libraryMutex) {
      locked = true;
      return;
    }
    locked = xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(timeoutMs)) ==
             pdPASS;
  }

  ~ScopedLibraryAccess() {
    if (locked && libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
  }

  explicit operator bool() const { return locked; }

private:
  bool locked;
};

class ScopedSdAccess {
public:
  explicit ScopedSdAccess(uint32_t timeoutMs = 2000) : locked(false) {
    if (i2cMutex) {
      locked = xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(timeoutMs)) ==
               pdPASS;
      if (!locked)
        return;
    } else {
      locked = true;
    }

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);
  }

  ~ScopedSdAccess() {
    if (!locked)
      return;
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    if (i2cMutex)
      xSemaphoreGiveRecursive(i2cMutex);
  }

  explicit operator bool() const { return locked; }

private:
  bool locked;
};

bool replaceFileWithRollback(const String &tmpPath, const String &path,
                             bool keepBackup = false) {
  const String backupPath = path + ".bak";
  if (SD.exists(backupPath) && !SD.remove(backupPath))
    return false;

  const bool hadOriginal = SD.exists(path);
  if (hadOriginal && !SD.rename(path, backupPath))
    return false;

  if (!SD.rename(tmpPath, path)) {
    if (hadOriginal)
      SD.rename(backupPath, path);
    return false;
  }

  if (!keepBackup && hadOriginal && SD.exists(backupPath))
    SD.remove(backupPath);
  return true;
}

void rollbackReplacedFile(const String &path, bool hadOriginal) {
  const String backupPath = path + ".bak";
  if (SD.exists(path))
    SD.remove(path);
  if (hadOriginal && SD.exists(backupPath))
    SD.rename(backupPath, path);
  else if (SD.exists(backupPath))
    SD.remove(backupPath);
}

void finalizeReplacedFile(const String &path) {
  const String backupPath = path + ".bak";
  if (SD.exists(backupPath))
    SD.remove(backupPath);
}

bool finishJsonWrite(File &file, size_t bytesWritten) {
  file.flush();
  const bool ok = bytesWritten > 0 && file.getWriteError() == 0;
  file.close();
  return ok;
}

bool writeIndexTemp(const IndexVector &items, const String &tmpPath) {
  if (SD.exists(tmpPath) && !SD.remove(tmpPath))
    return false;

  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file)
    return false;

  bool writeOk = true;
  for (const auto &item : items) {
    StaticJsonDocument<1024> doc;
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

    writeOk = serializeJson(doc, file) > 0 && file.println() > 0 && writeOk;
  }

  file.flush();
  writeOk = writeOk && file.getWriteError() == 0;
  file.close();
  if (!writeOk)
    SD.remove(tmpPath);
  return writeOk;
}
} // namespace

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
  ScopedLibraryAccess library(5000);
  if (!library) {
    ErrorHandler::logError(ERR_CAT_STORAGE, "Library busy", "Storage::saveCD");
    return false;
  }

  String path = getFilePath(cd.uniqueID.c_str(), MODE_CD);
  String tmpPath = path + ".tmp";
  ScopedSdAccess sd(2000);
  if (!sd) {
    ErrorHandler::logError(ERR_CAT_STORAGE, "SD bus busy", "Storage::saveCD");
    return false;
  }

  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  // 1. Save Detail JSON to TMP
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file) {
    ErrorHandler::logError(
        ERR_CAT_STORAGE, String("Failed to open file for writing: ") + tmpPath,
        "Storage::saveCD");
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

  if (!finishJsonWrite(file, serializeJson(doc, file))) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Failed while writing: ") + tmpPath,
                           "Storage::saveCD");
    return false;
  }

  // 2. Build the proposed index without exposing a partially committed state.
  auto &vec = getVectorForMode(MODE_CD);
  IndexVector updated = vec;
  bool found = false;
  for (auto &item : updated) {
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
    updated.push_back(newItem);
  }

  // 3. Keep the previous detail file until the matching index is durable.
  const bool hadDetail = SD.exists(path);
  if (!replaceFileWithRollback(tmpPath, path, true)) {
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Atomic rename failed: ") + tmpPath + " -> " +
                               path,
                           "Storage::saveCD");
    SD.remove(tmpPath);
    return false;
  }

  vec.swap(updated); // updated now contains the previous in-memory index.
  if (!skipIndexRewrite && !rewriteIndex(MODE_CD)) {
    vec.swap(updated);
    rollbackReplacedFile(path, hadDetail);
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           "Rolled back CD because index save failed",
                           "Storage::saveCD");
    return false;
  }

  finalizeReplacedFile(path);
  if (oldUniqueID && strlen(oldUniqueID) > 0 && cd.uniqueID != oldUniqueID) {
    const String oldPath = getFilePath(oldUniqueID, MODE_CD);
    if (oldPath != path && SD.exists(oldPath) && !SD.remove(oldPath)) {
      ErrorHandler::logError(ERR_CAT_STORAGE,
                             String("Could not remove renamed CD file: ") + oldPath,
                             "Storage::saveCD");
    }
  }

  if (skipIndexRewrite)
    return true;
  return true;
}

bool LibrarianStorage::updateFavorite(String uniqueID, MediaMode mode,
                                      bool favorite) {
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

  String path = getFilePath(uniqueID, mode);
  String tmpPath = path + ".tmp";
  ScopedSdAccess sd(5000);
  if (!sd)
    return false;

  File source = SD.open(path, FILE_READ);
  if (!source)
    return false;

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, source);
  source.close();
  if (error)
    return false;

  doc["favorite"] = favorite;
  if (SD.exists(tmpPath))
    SD.remove(tmpPath);
  File target = SD.open(tmpPath, FILE_WRITE);
  if (!target)
    return false;
  if (!finishJsonWrite(target, serializeJson(doc, target))) {
    SD.remove(tmpPath);
    return false;
  }

  IndexVector &index = getVectorForMode(mode);
  IndexVector updated = index;
  bool indexFound = false;
  for (LibraryIndexItem &item : updated) {
    if (item.uniqueID == uniqueID.c_str()) {
      item.favorite = favorite;
      indexFound = true;
      break;
    }
  }

  if (!indexFound) {
    SD.remove(tmpPath);
    return false;
  }

  const bool hadDetail = SD.exists(path);
  if (!replaceFileWithRollback(tmpPath, path, true)) {
    SD.remove(tmpPath);
    return false;
  }

  index.swap(updated);
  if (!rewriteIndex(mode)) {
    index.swap(updated);
    rollbackReplacedFile(path, hadDetail);
    return false;
  }

  finalizeReplacedFile(path);
  return true;
}

// --- LOAD INDEX ---
bool LibrarianStorage::loadIndex(MediaMode mode) {
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

  auto &vec = getVectorForMode(mode);
  IndexVector loaded;
  String path = getIndexPath(mode);
  ScopedSdAccess sd(2000);
  if (!sd)
    return false;
  File file = SD.open(path, FILE_READ);

  if (!file)
    return false; // No index yet

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

      loaded.push_back(item);
    } else {
      ErrorHandler::logError(ERR_CAT_STORAGE,
                             String("Skipping invalid index row: ") + error.c_str(),
                             "Storage::loadIndex");
    }
  }

  file.close();
  vec.swap(loaded);
  return true;
}

// --- REWRITE INDEX FILE ---
bool LibrarianStorage::rewriteIndex(MediaMode mode) {
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

  auto &vec = getVectorForMode(mode);
  String path = getIndexPath(mode);
  String tmpPath = path + ".tmp";

  ScopedSdAccess sd(5000);
  if (!sd)
    return false;

  if (!writeIndexTemp(vec, tmpPath) ||
      !replaceFileWithRollback(tmpPath, path)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Index transaction failed",
                           "Storage::rewriteIndex");
    return false;
  }
  return true;
}

bool LibrarianStorage::replaceIndex(MediaMode mode, const IndexVector &items) {
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

  ScopedSdAccess sd(5000);
  if (!sd)
    return false;

  const String path = getIndexPath(mode);
  const String tmpPath = path + ".tmp";
  if (!writeIndexTemp(items, tmpPath) ||
      !replaceFileWithRollback(tmpPath, path)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Index replacement failed",
                           "Storage::replaceIndex");
    return false;
  }

  getVectorForMode(mode) = items;
  return true;
}

bool LibrarianStorage::loadCDDetail(String uniqueID, CD &outCD) {
  String path = getFilePath(uniqueID, MODE_CD);
  Serial.printf("Storage: Loading CD Detail: %s\n", path.c_str());

  ScopedSdAccess sd(2000);
  if (!sd)
    return false;
  File file = SD.open(path, FILE_READ);
  if (!file)
    return false;

  DynamicJsonDocument doc(4096);
  DeserializationError parseError = deserializeJson(doc, file);
  file.close();
  if (parseError || !doc["title"].is<const char *>()) {
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Invalid CD detail JSON: ") + parseError.c_str(),
                           "Storage::loadCDDetail");
    return false;
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
  ScopedLibraryAccess library(5000);
  if (!library) {
    ErrorHandler::logError(ERR_CAT_STORAGE, "Library busy", "Storage::saveBook");
    return false;
  }

  String path = getFilePath(book.uniqueID.c_str(), MODE_BOOK);
  String tmpPath = path + ".tmp";
  ScopedSdAccess sd(2000);
  if (!sd)
    return false;

  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  // 1. Save Detail JSON to TMP
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file)
    return false;

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

  if (!finishJsonWrite(file, serializeJson(doc, file))) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Book transaction failed",
                           "Storage::saveBook");
    return false;
  }

  // 2. Build the proposed index without exposing a partial update.
  auto &vec = getVectorForMode(MODE_BOOK);
  IndexVector updated = vec;
  bool found = false;
  for (auto &item : updated) {
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
    updated.push_back(newItem);
  }

  const bool hadDetail = SD.exists(path);
  if (!replaceFileWithRollback(tmpPath, path, true)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Book detail commit failed",
                           "Storage::saveBook");
    return false;
  }

  vec.swap(updated);
  if (!skipIndexRewrite && !rewriteIndex(MODE_BOOK)) {
    vec.swap(updated);
    rollbackReplacedFile(path, hadDetail);
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           "Rolled back book because index save failed",
                           "Storage::saveBook");
    return false;
  }

  finalizeReplacedFile(path);
  if (oldUniqueID && strlen(oldUniqueID) > 0 && book.uniqueID != oldUniqueID) {
    const String oldPath = getFilePath(oldUniqueID, MODE_BOOK);
    if (oldPath != path && SD.exists(oldPath) && !SD.remove(oldPath)) {
      ErrorHandler::logError(ERR_CAT_STORAGE,
                             String("Could not remove renamed book file: ") + oldPath,
                             "Storage::saveBook");
    }
  }

  if (skipIndexRewrite)
    return true;
  return true;
}

// --- LOAD BOOK DETAIL ---
bool LibrarianStorage::loadBookDetail(String uniqueID, Book &outBook) {
  String path = getFilePath(uniqueID, MODE_BOOK);
  Serial.printf("Storage: Loading Book Detail: %s\n", path.c_str());

  ScopedSdAccess sd(2000);
  if (!sd)
    return false;
  File file = SD.open(path, FILE_READ);
  if (!file)
    return false;

  DynamicJsonDocument doc(4096);
  DeserializationError parseError = deserializeJson(doc, file);
  file.close();
  if (parseError || !doc["title"].is<const char *>()) {
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Invalid book detail JSON: ") + parseError.c_str(),
                           "Storage::loadBookDetail");
    return false;
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
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

  String path = getFilePath(uniqueID, mode);
  String deleteBackup = path + ".delete.bak";
  Serial.printf("Storage: Deleting %s\n", path.c_str());

  ScopedSdAccess sd(2000);
  if (!sd)
    return false;
  auto &vec = getVectorForMode(mode);
  IndexVector updated = vec;
  bool found = false;
  for (auto it = updated.begin(); it != updated.end(); ++it) {
    if (it->uniqueID == uniqueID.c_str()) {
      updated.erase(it);
      found = true;
      break;
    }
  }
  if (!found)
    return false;

  if (SD.exists(deleteBackup) && !SD.remove(deleteBackup))
    return false;
  const bool hadDetail = SD.exists(path);
  if (hadDetail && !SD.rename(path, deleteBackup))
    return false;

  vec.swap(updated);
  if (!rewriteIndex(mode)) {
    vec.swap(updated);
    if (hadDetail && SD.exists(deleteBackup))
      SD.rename(deleteBackup, path);
    return false;
  }

  if (SD.exists(deleteBackup) && !SD.remove(deleteBackup)) {
    ErrorHandler::logError(ERR_CAT_STORAGE,
                           String("Could not remove deleted item backup: ") +
                               deleteBackup,
                           "Storage::deleteItem");
  }
  return true;
}

bool LibrarianStorage::wipeLibrary(MediaMode mode) {
  ScopedLibraryAccess library(5000);
  if (!library)
    return false;

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

  ScopedSdAccess sd(5000);
  if (!sd)
    return false;

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

  String filename = "/tracks/" + sanitizeFilename(String(releaseMbid)) + ".json";
  static constexpr size_t MAX_TRACKLIST_BYTES = 512 * 1024;
  char *jsonBuffer = nullptr;
  size_t jsonSize = 0;
  {
    // SD chip-select and touch share the I2C expander. Hold that bus only for
    // the physical read; JSON parsing can be comparatively expensive and must
    // not prevent touch-release events from being sampled.
    ScopedSdAccess sd(2000);
    if (!sd)
      return nullptr;

    File file = SD.open(filename, FILE_READ);
    if (!file)
      return nullptr;

    jsonSize = file.size();
    if (jsonSize == 0 || jsonSize > MAX_TRACKLIST_BYTES) {
      file.close();
      return nullptr;
    }
    jsonBuffer = static_cast<char *>(heap_caps_malloc(
        jsonSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jsonBuffer) {
      file.close();
      return nullptr;
    }
    const size_t bytesRead = file.readBytes(jsonBuffer, jsonSize);
    file.close();
    if (bytesRead != jsonSize) {
      heap_caps_free(jsonBuffer);
      return nullptr;
    }
    jsonBuffer[jsonSize] = '\0';
  }

  // Parse after ScopedSdAccess has released the shared I2C guard.
  BasicJsonDocument<SpiRamAllocator> doc(65536);
  DeserializationError error = deserializeJson(doc, jsonBuffer, jsonSize);
  heap_caps_free(jsonBuffer);

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

  ScopedSdAccess sd(3000);
  if (!sd)
    return false;

  if (!SD.exists("/tracks")) {
    SD.mkdir("/tracks");
  }

  String filename = "/tracks/" + sanitizeFilename(String(releaseMbid)) + ".json";
  String tmpPath = filename + ".tmp";
  if (SD.exists(tmpPath))
    SD.remove(tmpPath);

  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file)
    return false;

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
  file.flush();
  const bool writeOk = file.getWriteError() == 0 && file.size() > 0;
  file.close();

  if (!writeOk || !replaceFileWithRollback(tmpPath, filename)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Tracklist transaction failed",
                           "Storage::saveTracklist");
    return false;
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

bool LibrarianStorage::loadLyrics(const char *lyricsPath,
                                  PsramString &lyricsOut) {
  lyricsOut.clear();
  if (!lyricsPath || strlen(lyricsPath) == 0)
    return false;

  String path = String(lyricsPath);
  if (!path.startsWith("/")) {
    path = "/lyrics/" + path;
  }

  static constexpr size_t MAX_LYRICS_FILE_BYTES = 512 * 1024;
  char *jsonBuffer = nullptr;
  size_t jsonSize = 0;
  {
    // Keep both the file buffer and parsed lyrics out of scarce internal RAM.
    // The shared SD/touch I2C guard is held only for the physical file read.
    ScopedSdAccess sd(2000);
    if (!sd)
      return false;

    String resolvedPath = path;
    if (!SD.exists(resolvedPath) && path.startsWith("/lyrics/")) {
      String rootPath = "/" + String(lyricsPath);
      if (SD.exists(rootPath))
        resolvedPath = rootPath;
    }
    if (!SD.exists(resolvedPath))
      return false;

    File file = SD.open(resolvedPath, FILE_READ);
    if (!file)
      return false;
    jsonSize = file.size();
    if (jsonSize == 0 || jsonSize > MAX_LYRICS_FILE_BYTES) {
      file.close();
      return false;
    }
    jsonBuffer = static_cast<char *>(heap_caps_malloc(
        jsonSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jsonBuffer) {
      file.close();
      return false;
    }
    const size_t bytesRead = file.readBytes(jsonBuffer, jsonSize);
    file.close();
    if (bytesRead != jsonSize) {
      heap_caps_free(jsonBuffer);
      return false;
    }
    jsonBuffer[jsonSize] = '\0';
  }

  BasicJsonDocument<SpiRamAllocator> doc(16384);
  DeserializationError error = deserializeJson(doc, jsonBuffer, jsonSize);

  if (error) {
    Serial.print("Storage: Failed to parse lyrics JSON: ");
    Serial.println(error.c_str());
    heap_caps_free(jsonBuffer);
    return false;
  }

  const char *text = doc["text"] | "";
  if (text && text[0] != '\0')
    lyricsOut.assign(text);
  heap_caps_free(jsonBuffer);
  return !lyricsOut.empty();
}

bool LibrarianStorage::saveLyrics(const char *lyricsPath,
                                  const PsramString &lyricsText,
                                  String lang) {
  if (!lyricsPath)
    return false;

  String path = String(lyricsPath);
  if (!path.startsWith("/")) {
    path = "/lyrics/" + path;
  }

  ScopedSdAccess sd(3000);
  if (!sd)
    return false;

  // Create directory if needed
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String dir = path.substring(0, lastSlash);
    if (!SD.exists(dir) && !SD.mkdir(dir))
      return false;
  }

  String tmpPath = path + ".tmp";
  if (SD.exists(tmpPath))
    SD.remove(tmpPath);
  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file)
    return false;

  // The lyrics body can be much larger than the ESP32's free internal heap.
  // Keep the JSON document and its duplicated string in PSRAM and size it from
  // the already bounded provider response.
  const size_t jsonCapacity = lyricsText.size() + 4096;
  BasicJsonDocument<SpiRamAllocator> doc(jsonCapacity);
  doc["lang"] = lang;
  doc["fetchedAt"] = getCurrentISO8601Timestamp();
  doc["text"] = lyricsText.c_str();

  if (doc.overflowed()) {
    file.close();
    SD.remove(tmpPath);
    return false;
  }

  if (!finishJsonWrite(file, serializeJson(doc, file)) ||
      !replaceFileWithRollback(tmpPath, path)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Lyrics transaction failed",
                           "Storage::saveLyrics");
    return false;
  }
  return true;
}
