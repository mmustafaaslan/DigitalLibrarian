#ifndef STORAGE_H
#define STORAGE_H

#include "Core_Data.h"
#include "waveshare_sd_card.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// Forward declaration for the IO Expander
class ESP_IOExpander_CH422G;
extern ESP_IOExpander_CH422G *sdExpander;

// Lightweight Index Item - Kept in RAM
struct LibraryIndexItem {
  PsramString uniqueID;
  PsramString title;
  PsramString artist; // Author for books
  PsramString coverFile;
  int year;
  PsramString genre; // Useful for filtering
  bool favorite;

  // For mapping to physical shelf
  PsramIntVector ledIndices;

  // Metadata for list display (Page Count / Track Count)
  int metaInt = 0;        // pageCount (Book) or trackCount (CD)
  PsramString metaString; // ISBN (Book) or Barcode (CD)
};

#include "PsramAllocator.h"

// Define PSRAM-backed vector for index items
typedef std::vector<LibraryIndexItem, PsramAllocator<LibraryIndexItem>>
    IndexVector;

class LibrarianStorage {
public:
  LibrarianStorage();

  // Initialization
  bool begin();

  // Index Management
  bool loadIndex(MediaMode mode); // Loads index.jsonl into RAM
  IndexVector &getIndex();        // Access the RAM list (PSRAM now)
  IndexVector &getVectorForMode(MediaMode mode);

  // CRUD Operations
  // Returns true if found and populated, false otherwise
  bool loadCDDetail(String uniqueID, CD &outCD);
  bool loadBookDetail(String uniqueID, Book &outBook);

  bool saveCD(const CD &cd, const char *oldUniqueID = nullptr,
              bool skipIndexRewrite = false);
  bool saveBook(const Book &book, const char *oldUniqueID = nullptr,
                bool skipIndexRewrite = false);

  bool deleteItem(String uniqueID, MediaMode mode);
  bool wipeLibrary(MediaMode mode); // Delete ALL data for a mode

  // Tracklist Management
  TrackList *loadTracklist(const char *releaseMbid);
  bool saveTracklist(const char *releaseMbid, TrackList *trackList);
  void deleteTracklist(TrackList *trackList);

  // Chapter code removed

  bool rewriteIndex(MediaMode mode);

  // Lyrics Management
  String loadLyrics(const char *lyricsPath);
  bool saveLyrics(const char *lyricsPath, String lyricsText,
                  String lang = "en");

private:
  IndexVector _cdIndex;
  IndexVector _bookIndex;

  // Helper to generate consistent file paths
  String getFilePath(String uniqueID, MediaMode mode);
  String getIndexPath(MediaMode mode);

  // Helper to append/rewrite index file
  bool appendToIndex(const LibraryIndexItem &item, MediaMode mode);
};

// Global Instance
extern LibrarianStorage Storage;

#endif // STORAGE_H
