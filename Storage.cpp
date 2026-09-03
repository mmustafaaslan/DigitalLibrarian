#include "Storage.h"
#include "AppGlobals.h"
#include "ErrorHandler.h"
#include "Utils.h"
#include "waveshare_sd_card.h" // For SD_CS and sdExpander
#include <SD.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <utility>

LibrarianStorage Storage;

namespace {
constexpr size_t ITEM_JSON_CAPACITY = 64 * 1024;
constexpr size_t MAX_INDEX_LINE_BYTES = 64 * 1024;

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

// A reset can occur between renaming the old file to .bak and promoting the
// completed .tmp file. Prefer the known-good backup when the canonical file is
// absent. A .delete.bak is also safe to restore: if deletion had committed, its
// index row would no longer cause this detail file to be loaded.
bool recoverInterruptedReplace(const String &path) {
  if (SD.exists(path))
    return true;

  const String backupPath = path + ".bak";
  const String deleteBackupPath = path + ".delete.bak";
  const String tmpPath = path + ".tmp";
  String recoveryPath;
  if (SD.exists(backupPath))
    recoveryPath = backupPath;
  else if (SD.exists(deleteBackupPath))
    recoveryPath = deleteBackupPath;
  else if (SD.exists(tmpPath)) {
    File candidate = SD.open(tmpPath, FILE_READ);
    const bool usable = candidate && candidate.size() > 0;
    if (candidate)
      candidate.close();
    if (usable)
      recoveryPath = tmpPath;
  }

  if (recoveryPath.isEmpty() || !SD.rename(recoveryPath, path))
    return false;

  if (recoveryPath != tmpPath && SD.exists(tmpPath))
    SD.remove(tmpPath);
  ErrorHandler::logWarn(ERR_CAT_STORAGE,
                        String("Recovered interrupted file transaction: ") +
                            path,
                        "Storage::recovery");
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

// ArduinoJson and escaped-string writers may otherwise call File::write() one
// byte at a time. A large lyrics body or full track-list rewrite can then keep
// core 0 inside SD/SPI code long enough for the idle-task watchdog to fire.
// Buffer writes and yield after each physical SD chunk.
class YieldingBufferedFileWriter : public Print {
public:
  explicit YieldingBufferedFileWriter(File &file)
      : _file(file), _used(0), _ok(true), _bytesAccepted(0) {}

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *data, size_t length) override {
    if (!_ok || !data)
      return 0;
    size_t accepted = 0;
    while (accepted < length) {
      const size_t available = sizeof(_buffer) - _used;
      const size_t amount = std::min(available, length - accepted);
      memcpy(_buffer + _used, data + accepted, amount);
      _used += amount;
      accepted += amount;
      _bytesAccepted += amount;
      if (_used == sizeof(_buffer) && !flushBuffer())
        break;
    }
    return accepted;
  }

  bool finish() { return flushBuffer() && _ok; }
  size_t bytesAccepted() const { return _bytesAccepted; }

private:
  bool flushBuffer() {
    if (!_ok || _used == 0)
      return _ok;
    const size_t written = _file.write(_buffer, _used);
    _ok = written == _used && _file.getWriteError() == 0;
    _used = 0;
    delay(1);
    return _ok;
  }

  File &_file;
  uint8_t _buffer[512];
  size_t _used;
  bool _ok;
  size_t _bytesAccepted;
};

size_t readFileYielding(File &file, uint8_t *destination, size_t length) {
  if (!destination)
    return 0;
  size_t total = 0;
  static constexpr size_t SD_READ_CHUNK_BYTES = 4096;
  while (total < length) {
    const size_t amount =
        std::min(SD_READ_CHUNK_BYTES, length - total);
    const int bytesRead = file.read(destination + total, amount);
    if (bytesRead <= 0)
      break;
    total += static_cast<size_t>(bytesRead);
    delay(1);
  }
  return total;
}

// Stream JSON strings directly to the SD file. Repeatedly assembling escaped
// Arduino Strings here fragments the small internal heap that the next TLS
// handshake needs during a bulk lyrics download.
bool writeEscapedJsonString(Print &file, const char *value) {
  if (!value)
    value = "";
  static const char hex[] = "0123456789abcdef";
  bool ok = file.write((uint8_t)'"') == 1;
  for (const uint8_t *p = reinterpret_cast<const uint8_t *>(value); ok && *p;
       ++p) {
    switch (*p) {
    case '"':
      ok = file.print("\\\"") == 2;
      break;
    case '\\':
      ok = file.print("\\\\") == 2;
      break;
    case '\b':
      ok = file.print("\\b") == 2;
      break;
    case '\f':
      ok = file.print("\\f") == 2;
      break;
    case '\n':
      ok = file.print("\\n") == 2;
      break;
    case '\r':
      ok = file.print("\\r") == 2;
      break;
    case '\t':
      ok = file.print("\\t") == 2;
      break;
    default:
      if (*p < 0x20) {
        char escaped[] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 0x0f]};
        ok = file.write(reinterpret_cast<const uint8_t *>(escaped),
                        sizeof(escaped)) == sizeof(escaped);
      } else {
        ok = file.write(*p) == 1;
      }
      break;
    }
  }
  return ok && file.write((uint8_t)'"') == 1;
}

bool writeIndexTemp(const IndexVector &items, const String &tmpPath) {
  if (SD.exists(tmpPath) && !SD.remove(tmpPath))
    return false;

  File file = SD.open(tmpPath, FILE_WRITE);
  if (!file)
    return false;
  YieldingBufferedFileWriter writer(file);

  bool writeOk = true;
  for (const auto &item : items) {
    // Stream index rows directly instead of placing the whole record in a
    // fixed JsonDocument. A record with a long title or many shelf LEDs must
    // never be silently truncated while the write still reports success.
    writeOk = writer.print(F("{\"id\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.uniqueID.c_str()) && writeOk;
    writeOk = writer.print(F(",\"t\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.title.c_str()) && writeOk;
    writeOk = writer.print(F(",\"a\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.artist.c_str()) && writeOk;
    writeOk = writer.print(F(",\"c\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.coverFile.c_str()) && writeOk;
    writeOk = writer.print(F(",\"y\":")) > 0 && writeOk;
    writeOk = writer.print(item.year) > 0 && writeOk;
    writeOk = writer.print(F(",\"g\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.genre.c_str()) && writeOk;
    writeOk = writer.print(F(",\"f\":")) > 0 && writeOk;
    writeOk = writer.print(item.favorite ? F("true") : F("false")) > 0 &&
              writeOk;
    writeOk = writer.print(F(",\"mi\":")) > 0 && writeOk;
    writeOk = writer.print(item.metaInt) > 0 && writeOk;
    writeOk = writer.print(F(",\"ms\":")) > 0 && writeOk;
    writeOk = writeEscapedJsonString(writer, item.metaString.c_str()) && writeOk;
    writeOk = writer.print(F(",\"l\":[")) > 0 && writeOk;
    for (size_t i = 0; i < item.ledIndices.size(); ++i) {
      if (i > 0)
        writeOk = writer.write((uint8_t)',') == 1 && writeOk;
      writeOk = writer.print(item.ledIndices[i]) > 0 && writeOk;
    }
    writeOk = writer.print(F("]}\n")) > 0 && writeOk;
  }

  writeOk = writer.finish() && writeOk;
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
                              bool skipIndexRewrite,
                              bool keepDetailBackup) {
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

  BasicJsonDocument<SpiRamAllocator> doc(ITEM_JSON_CAPACITY);
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

  if (doc.overflowed()) {
    file.close();
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "CD detail is too large to save",
                           "Storage::saveCD");
    return false;
  }

  YieldingBufferedFileWriter writer(file);
  const size_t detailBytes = serializeJson(doc, writer);
  if (!writer.finish() || !finishJsonWrite(file, detailBytes)) {
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

  if (!keepDetailBackup)
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

  recoverInterruptedReplace(path);
  File source = SD.open(path, FILE_READ);
  if (!source)
    return false;

  BasicJsonDocument<SpiRamAllocator> doc(ITEM_JSON_CAPACITY);
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
  YieldingBufferedFileWriter writer(target);
  const size_t detailBytes = serializeJson(doc, writer);
  if (!writer.finish() || !finishJsonWrite(target, detailBytes)) {
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
  recoverInterruptedReplace(path);
  File file = SD.open(path, FILE_READ);

  if (!file) {
    // A missing index is a valid empty library (first boot or after a wipe).
    // Do not leave stale in-memory rows behind after a successful SD mount.
    vec.clear();
    return true;
  }

  // Read Line-By-Line (JSONL)
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;

    if (line.length() > MAX_INDEX_LINE_BYTES) {
      file.close();
      ErrorHandler::logError(ERR_CAT_STORAGE, "Index row exceeds 64 KB",
                             "Storage::loadIndex");
      return false;
    }

    const size_t capacity =
        std::min(MAX_INDEX_LINE_BYTES,
                 std::max<size_t>(4096, line.length() * 4 + 2048));
    BasicJsonDocument<SpiRamAllocator> doc(capacity);
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
                             String("Invalid index row: ") + error.c_str(),
                             "Storage::loadIndex");
      file.close();
      return false;
    }
    delay(1);
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
  recoverInterruptedReplace(path);
  File file = SD.open(path, FILE_READ);
  if (!file)
    return false;

  BasicJsonDocument<SpiRamAllocator> doc(ITEM_JSON_CAPACITY);
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
                                bool skipIndexRewrite,
                                bool keepDetailBackup) {
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

  BasicJsonDocument<SpiRamAllocator> doc(ITEM_JSON_CAPACITY);
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

  if (doc.overflowed()) {
    file.close();
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Book detail is too large to save",
                           "Storage::saveBook");
    return false;
  }

  YieldingBufferedFileWriter writer(file);
  const size_t detailBytes = serializeJson(doc, writer);
  if (!writer.finish() || !finishJsonWrite(file, detailBytes)) {
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

  if (!keepDetailBackup)
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
  recoverInterruptedReplace(path);
  File file = SD.open(path, FILE_READ);
  if (!file)
    return false;

  BasicJsonDocument<SpiRamAllocator> doc(ITEM_JSON_CAPACITY);
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

    recoverInterruptedReplace(filename);
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
    const size_t bytesRead = readFileYielding(
        file, reinterpret_cast<uint8_t *>(jsonBuffer), jsonSize);
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
                                     TrackList *trackList,
                                     bool keepBackup) {
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

  YieldingBufferedFileWriter writer(file);

  // Stream JSON directly to file without allocating temporary Arduino Strings.
  bool streamOk = writer.print("{\"releaseMbid\":") > 0;
  streamOk = writeEscapedJsonString(writer, releaseMbid) && streamOk;
  streamOk = writer.print(",\"cdTitle\":") > 0 && streamOk;
  streamOk = writeEscapedJsonString(writer, trackList->cdTitle.c_str()) && streamOk;
  streamOk = writer.print(",\"cdArtist\":") > 0 && streamOk;
  streamOk = writeEscapedJsonString(writer, trackList->cdArtist.c_str()) && streamOk;
  streamOk = writer.print(",\"fetchedAt\":") > 0 && streamOk;
  streamOk = writeEscapedJsonString(writer, trackList->fetchedAt.c_str()) && streamOk;
  streamOk = writer.print(",\"tracks\":[") > 0 && streamOk;

  for (size_t i = 0; i < trackList->tracks.size(); i++) {
    Track &track = trackList->tracks[i];
    if (i > 0)
      streamOk = writer.print(",") > 0 && streamOk;

    streamOk = writer.print("{") > 0 && streamOk;
    streamOk = writer.printf("\"trackNo\":%d,", track.trackNo) > 0 && streamOk;
    streamOk = writer.print("\"title\":") > 0 && streamOk;
    streamOk = writeEscapedJsonString(writer, track.title.c_str()) && streamOk;
    streamOk = writer.printf(",\"durationMs\":%lu,", track.durationMs) > 0 && streamOk;
    streamOk = writer.print("\"recordingMbid\":") > 0 && streamOk;
    streamOk = writeEscapedJsonString(writer, track.recordingMbid.c_str()) && streamOk;
    streamOk = writer.print(",") > 0 && streamOk;

    streamOk = writer.print("\"lyrics\":{\"status\":") > 0 && streamOk;
    streamOk = writeEscapedJsonString(writer, track.lyrics.status.c_str()) && streamOk;

    // String comparisons with PsramString work if PsramString is std::string
    if (track.lyrics.status == "cached") {
      streamOk = writer.print(",\"path\":") > 0 && streamOk;
      streamOk = writeEscapedJsonString(writer, track.lyrics.path.c_str()) && streamOk;
      streamOk = writer.print(",\"fetchedAt\":") > 0 && streamOk;
      streamOk = writeEscapedJsonString(writer, track.lyrics.fetchedAt.c_str()) && streamOk;
      streamOk = writer.print(",\"lang\":") > 0 && streamOk;
      streamOk = writeEscapedJsonString(writer, track.lyrics.lang.c_str()) && streamOk;
    } else if (track.lyrics.status == "missing") {
      streamOk = writer.print(",\"lastTriedAt\":") > 0 && streamOk;
      streamOk = writeEscapedJsonString(writer, track.lyrics.lastTriedAt.c_str()) && streamOk;
      streamOk = writer.print(",\"error\":") > 0 && streamOk;
      streamOk = writeEscapedJsonString(writer, track.lyrics.error.c_str()) && streamOk;
    }
    streamOk = writer.print("},") > 0 && streamOk;
    streamOk = writer.printf("\"isFavoriteTrack\":%s",
                             track.isFavoriteTrack ? "true" : "false") > 0 && streamOk;
    streamOk = writer.print("}") > 0 && streamOk;
  }
  streamOk = writer.print("]}") > 0 && streamOk;
  streamOk = writer.finish() && streamOk;
  file.flush();
  const bool writeOk = streamOk && file.getWriteError() == 0 && file.size() > 0;
  file.close();

  if (!writeOk || !replaceFileWithRollback(tmpPath, filename, keepBackup)) {
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
    recoverInterruptedReplace(resolvedPath);
    if (!SD.exists(resolvedPath) && path.startsWith("/lyrics/")) {
      String rootPath = "/" + String(lyricsPath);
      recoverInterruptedReplace(rootPath);
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
    const size_t bytesRead = readFileYielding(
        file, reinterpret_cast<uint8_t *>(jsonBuffer), jsonSize);
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

  YieldingBufferedFileWriter writer(file);
  const size_t bytesWritten = serializeJson(doc, writer);
  const bool bufferedWriteOk = writer.finish();
  if (!bufferedWriteOk || !finishJsonWrite(file, bytesWritten) ||
      !replaceFileWithRollback(tmpPath, path)) {
    SD.remove(tmpPath);
    ErrorHandler::logError(ERR_CAT_STORAGE, "Lyrics transaction failed",
                           "Storage::saveLyrics");
    return false;
  }
  return true;
}

bool LibrarianStorage::lyricsFileExists(const char *lyricsPath) {
  if (!lyricsPath || lyricsPath[0] == '\0')
    return false;

  String path(lyricsPath);
  if (!path.startsWith("/"))
    path = "/lyrics/" + path;

  ScopedSdAccess sd(2000);
  if (!sd)
    return false;
  recoverInterruptedReplace(path);
  File file = SD.open(path, FILE_READ);
  if (!file)
    return false;
  const size_t size = file.size();
  file.close();
  return size > 0 && size <= 512 * 1024;
}

static bool backupStringValid(JsonVariantConst value, size_t maxLength,
                              bool required = false) {
  if (value.isNull())
    return !required;
  if (!value.is<const char *>())
    return false;
  const char *text = value.as<const char *>();
  if (!text)
    return !required;
  const size_t length = strlen(text);
  return length <= maxLength && (!required || length > 0);
}

static bool backupLedArrayValid(JsonVariantConst value) {
  if (!value.is<JsonArrayConst>())
    return false;
  JsonArrayConst values = value.as<JsonArrayConst>();
  if (values.size() > 128)
    return false;
  for (JsonVariantConst entry : values) {
    if (!entry.is<int>())
      return false;
    const int led = entry.as<int>();
    if (led < 0 || led >= led_count)
      return false;
  }
  return true;
}

static bool backupLyricsPathValid(JsonVariantConst value) {
  if (!backupStringValid(value, 255))
    return false;
  const String path = value.isNull() ? "" : String(value.as<const char *>());
  if (path.isEmpty())
    return true;
  return path.indexOf("..") < 0 && path.indexOf('\\') < 0 &&
         (!path.startsWith("/") || path.startsWith("/lyrics/"));
}

static bool backupDocumentValid(JsonDocument &doc) {
  if (!doc["type"].is<const char *>() || !doc["data"].is<JsonObject>())
    return false;

  const char *type = doc["type"].as<const char *>();
  JsonObjectConst data = doc["data"].as<JsonObjectConst>();
  if (strcmp(type, "cd") == 0 || strcmp(type, "book") == 0) {
    if (!backupStringValid(data["uniqueID"], 128, true) ||
        !backupStringValid(data["title"], 512, true) ||
        !backupStringValid(data["genre"], 128) ||
        !backupStringValid(data["coverUrl"], 2048) ||
        !backupStringValid(data["coverFile"], 255) ||
        !backupStringValid(data["notes"], 4096) ||
        !backupLedArrayValid(data["ledIndices"]))
      return false;

    const int year = data["year"] | 0;
    if (year != 0 && (year < 1000 || year > 2100))
      return false;
    const String id = data["uniqueID"].as<const char *>();
    if (sanitizeFilename(id).isEmpty())
      return false;

    if (strcmp(type, "cd") == 0) {
      const int trackCount = data["trackCount"] | 0;
      return backupStringValid(data["artist"], 512) &&
             backupStringValid(data["barcode"], 128) &&
             backupStringValid(data["releaseMbid"], 128) && trackCount >= 0 &&
             trackCount <= 1000;
    }

    const int pageCount = data["pageCount"] | 0;
    const int currentPage = data["currentPage"] | 0;
    return backupStringValid(data["author"], 512) &&
           backupStringValid(data["isbn"], 128) &&
           backupStringValid(data["publisher"], 512) && pageCount >= 0 &&
           pageCount <= 100000 && currentPage >= 0 &&
           currentPage <= pageCount;
  }

  if (strcmp(type, "tracklist") != 0 ||
      !backupStringValid(doc["mbid"], 128, true) ||
      !backupStringValid(data["cdTitle"], 512) ||
      !backupStringValid(data["cdArtist"], 512) ||
      !backupStringValid(data["fetchedAt"], 64) ||
      !data["tracks"].is<JsonArrayConst>())
    return false;

  JsonArrayConst tracks = data["tracks"].as<JsonArrayConst>();
  if (tracks.size() > 1000)
    return false;
  for (JsonObjectConst track : tracks) {
    const int trackNo = track["trackNo"] | 0;
    if (trackNo < 0 || trackNo > 1000 ||
        !backupStringValid(track["title"], 512, true) ||
        !backupStringValid(track["recordingMbid"], 128) ||
        !track["lyrics"].is<JsonObjectConst>())
      return false;
    JsonObjectConst lyrics = track["lyrics"].as<JsonObjectConst>();
    if (!backupStringValid(lyrics["status"], 32) ||
        !backupLyricsPathValid(lyrics["path"]) ||
        !backupStringValid(lyrics["fetchedAt"], 64) ||
        !backupStringValid(lyrics["lastTriedAt"], 64) ||
        !backupStringValid(lyrics["lang"], 32) ||
        !backupStringValid(lyrics["error"], 1024))
      return false;
  }
  return true;
}

bool LibrarianStorage::importBackup(const char *path, int &itemCount,
                                    int &tracklistCount) {
  itemCount = 0;
  tracklistCount = 0;
  if (!path || path[0] == '\0')
    return false;

  // Copy the upload into PSRAM, then release the shared SD/touch bus before
  // parsing. The web upload is capped at 2 MiB, so this allocation is bounded.
  char *contents = nullptr;
  size_t contentSize = 0;
  {
    ScopedSdAccess sd(5000);
    if (!sd)
      return false;
    File file = SD.open(path, FILE_READ);
    if (!file)
      return false;
    contentSize = file.size();
    if (contentSize == 0 || contentSize > 2 * 1024 * 1024) {
      file.close();
      return false;
    }
    contents = static_cast<char *>(
        heap_caps_malloc(contentSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!contents) {
      file.close();
      return false;
    }
    const size_t bytesRead = readFileYielding(
        file, reinterpret_cast<uint8_t *>(contents), contentSize);
    file.close();
    if (bytesRead != contentSize) {
      heap_caps_free(contents);
      return false;
    }
    contents[contentSize] = '\0';
  }

  // Preflight every record before the first live write. A malformed line near
  // the end of an upload must never leave earlier records partially imported.
  bool preflightValid = false;
  size_t preflightOffset = 0;
  size_t validatedRecords = 0;
  std::vector<PsramString, PsramAllocator<PsramString>> recordKeys;
  while (preflightOffset < contentSize) {
    size_t end = preflightOffset;
    while (end < contentSize && contents[end] != '\n' &&
           contents[end] != '\r')
      end++;
    const size_t lineLength = end - preflightOffset;
    while (end < contentSize &&
           (contents[end] == '\n' || contents[end] == '\r'))
      end++;

    if (lineLength > 0) {
      if (lineLength > 65536) {
        heap_caps_free(contents);
        return false;
      }
      BasicJsonDocument<SpiRamAllocator> doc(65536);
      if (deserializeJson(doc, contents + preflightOffset, lineLength) ||
          !backupDocumentValid(doc)) {
        heap_caps_free(contents);
        return false;
      }
      const char *type = doc["type"] | "";
      const char *identity = strcmp(type, "tracklist") == 0
                                 ? (const char *)(doc["mbid"] | "")
                                 : (const char *)(doc["data"]["uniqueID"] | "");
      String canonicalIdentity = sanitizeFilename(String(identity));
      canonicalIdentity.toLowerCase(); // FAT paths are case-insensitive.
      PsramString recordKey(type);
      recordKey += ':';
      recordKey += canonicalIdentity.c_str();
      if (std::find(recordKeys.begin(), recordKeys.end(), recordKey) !=
          recordKeys.end()) {
        heap_caps_free(contents);
        return false;
      }
      recordKeys.push_back(std::move(recordKey));
      validatedRecords++;
    }
    preflightOffset = end;
    yield();
  }
  preflightValid = validatedRecords > 0;
  if (!preflightValid) {
    heap_caps_free(contents);
    return false;
  }

  struct ImportReplacement {
    PsramString path;
    bool hadOriginal = false;
  };
  std::vector<ImportReplacement, PsramAllocator<ImportReplacement>>
      replacements;
  const IndexVector originalCdIndex = _cdIndex;
  const IndexVector originalBookIndex = _bookIndex;

  auto readImportFileState = [](const String &filePath, bool &exists) {
    ScopedSdAccess sd(2000);
    if (!sd)
      return false;
    recoverInterruptedReplace(filePath);
    exists = SD.exists(filePath);
    return true;
  };

  bool allValid = true;
  bool importedCD = false;
  bool importedBook = false;
  size_t offset = 0;
  while (offset < contentSize) {
    size_t end = offset;
    while (end < contentSize && contents[end] != '\n' && contents[end] != '\r')
      end++;
    const size_t lineLength = end - offset;
    while (end < contentSize &&
           (contents[end] == '\n' || contents[end] == '\r'))
      end++;

    if (lineLength == 0) {
      offset = end;
      continue;
    }
    if (lineLength > 65536) {
      allValid = false;
      offset = end;
      continue;
    }

    BasicJsonDocument<SpiRamAllocator> doc(65536);
    const DeserializationError error =
        deserializeJson(doc, contents + offset, lineLength);
    offset = end;
    if (error) {
      allValid = false;
      continue;
    }

    const char *type = doc["type"] | "";
    JsonObject data = doc["data"];
    bool saved = false;
    if (strcmp(type, "cd") == 0) {
      CD cd;
      cd.title = (const char *)(data["title"] | "");
      cd.artist = (const char *)(data["artist"] | "");
      cd.genre = (const char *)(data["genre"] | "");
      cd.year = data["year"] | 0;
      cd.uniqueID = (const char *)(data["uniqueID"] | "");
      cd.coverUrl = (const char *)(data["coverUrl"] | "");
      cd.coverFile = (const char *)(data["coverFile"] | "");
      cd.favorite = data["favorite"] | false;
      cd.notes = (const char *)(data["notes"] | "");
      cd.barcode = (const char *)(data["barcode"] | "");
      cd.trackCount = data["trackCount"] | 0;
      cd.totalDurationMs = data["totalDurationMs"] | 0;
      cd.releaseMbid = (const char *)(data["releaseMbid"] | "");
      JsonArrayConst ledIndices = data["ledIndices"].as<JsonArrayConst>();
      for (int led : ledIndices)
        cd.ledIndices.push_back(led);
      if (!cd.uniqueID.empty() && !cd.title.empty()) {
        const String detailPath = getFilePath(cd.uniqueID.c_str(), MODE_CD);
        bool hadOriginal = false;
        saved = readImportFileState(detailPath, hadOriginal) &&
                saveCD(cd, nullptr, true, true);
        if (saved)
          replacements.push_back(
              {PsramString(detailPath.c_str()), hadOriginal});
        importedCD = importedCD || saved;
      }
    } else if (strcmp(type, "book") == 0) {
      Book book;
      book.title = (const char *)(data["title"] | "");
      book.author = (const char *)(data["author"] | "");
      book.genre = (const char *)(data["genre"] | "");
      book.year = data["year"] | 0;
      book.uniqueID = (const char *)(data["uniqueID"] | "");
      book.coverUrl = (const char *)(data["coverUrl"] | "");
      book.coverFile = (const char *)(data["coverFile"] | "");
      book.favorite = data["favorite"] | false;
      book.notes = (const char *)(data["notes"] | "");
      book.isbn = (const char *)(data["isbn"] | "");
      book.pageCount = data["pageCount"] | 0;
      book.currentPage = data["currentPage"] | 0;
      book.publisher = (const char *)(data["publisher"] | "");
      JsonArrayConst ledIndices = data["ledIndices"].as<JsonArrayConst>();
      for (int led : ledIndices)
        book.ledIndices.push_back(led);
      if (!book.uniqueID.empty() && !book.title.empty()) {
        const String detailPath = getFilePath(book.uniqueID.c_str(), MODE_BOOK);
        bool hadOriginal = false;
        saved = readImportFileState(detailPath, hadOriginal) &&
                saveBook(book, nullptr, true, true);
        if (saved)
          replacements.push_back(
              {PsramString(detailPath.c_str()), hadOriginal});
        importedBook = importedBook || saved;
      }
    } else if (strcmp(type, "tracklist") == 0) {
      const char *mbid = doc["mbid"] | "";
      if (mbid[0] != '\0') {
        TrackList trackList;
        trackList.releaseMbid = mbid;
        trackList.cdTitle = (const char *)(data["cdTitle"] | "");
        trackList.cdArtist = (const char *)(data["cdArtist"] | "");
        trackList.fetchedAt = (const char *)(data["fetchedAt"] | "");
        for (JsonObject trackData : data["tracks"].as<JsonArray>()) {
          Track track;
          track.trackNo = trackData["trackNo"] | 0;
          track.title = (const char *)(trackData["title"] | "");
          track.durationMs = trackData["durationMs"] | 0;
          track.recordingMbid =
              (const char *)(trackData["recordingMbid"] | "");
          track.isFavoriteTrack = !trackData["isFav"].isNull()
                                      ? (trackData["isFav"] | false)
                                      : (trackData["isFavoriteTrack"] | false);
          JsonObject lyrics = trackData["lyrics"];
          track.lyrics.status = (const char *)(lyrics["status"] | "unchecked");
          track.lyrics.path = (const char *)(lyrics["path"] | "");
          track.lyrics.fetchedAt = (const char *)(lyrics["fetchedAt"] | "");
          track.lyrics.lastTriedAt =
              (const char *)(lyrics["lastTriedAt"] | "");
          track.lyrics.lang = (const char *)(lyrics["lang"] | "");
          track.lyrics.error = (const char *)(lyrics["error"] | "");
          trackList.tracks.push_back(track);
        }
        const String trackPath =
            "/tracks/" + sanitizeFilename(String(mbid)) + ".json";
        bool hadOriginal = false;
        saved = readImportFileState(trackPath, hadOriginal) &&
                saveTracklist(mbid, &trackList, true);
        if (saved) {
          replacements.push_back(
              {PsramString(trackPath.c_str()), hadOriginal});
          tracklistCount++;
        }
      }
      if (!saved) {
        allValid = false;
        break;
      }
      yield();
      continue;
    } else {
      allValid = false;
      yield();
      continue;
    }

    if (saved)
      itemCount++;
    else {
      allValid = false;
      break;
    }
    yield();
  }
  heap_caps_free(contents);

  // Each imported detail transaction updates its in-memory index. Commit each
  // affected index once, rather than once per record.
  if (allValid && importedCD && !rewriteIndex(MODE_CD))
    allValid = false;
  if (allValid && importedBook && !rewriteIndex(MODE_BOOK))
    allValid = false;

  if (!allValid) {
    // Restore every detail/track file in reverse order, then restore the
    // in-memory and durable indexes. Backups are intentionally retained by
    // the import-only save calls until this point.
    {
      ScopedSdAccess sd(5000);
      if (sd) {
        for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
          rollbackReplacedFile(String(it->path.c_str()), it->hadOriginal);
      }
    }
    _cdIndex = originalCdIndex;
    _bookIndex = originalBookIndex;
    if (importedCD)
      rewriteIndex(MODE_CD);
    if (importedBook)
      rewriteIndex(MODE_BOOK);
    itemCount = 0;
    tracklistCount = 0;
    return false;
  }

  {
    ScopedSdAccess sd(5000);
    if (sd) {
      for (const ImportReplacement &replacement : replacements)
        finalizeReplacedFile(String(replacement.path.c_str()));
    }
  }

  {
    ScopedSdAccess sd(2000);
    if (sd && SD.exists(path) && !SD.remove(path))
      allValid = false;
  }
  return allValid && (itemCount > 0 || tracklistCount > 0);
}
