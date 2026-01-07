#ifndef MODE_ABSTRACTION_H
#define MODE_ABSTRACTION_H

// ============================================================================
// MODE ABSTRACTION LAYER - Switch-based for extensibility
// ============================================================================
//
// This abstraction layer provides a unified interface for accessing items
// across different media types (CDs, Books, and future modes like Vinyl, Games,
// etc.)
//
// NOTE: Include this file AFTER all global declarations in DigitalLibrarian.ino
//       It depends on: currentMode, bookLibrary, cdLibrary, Book, CD structs
#include "Core_Data.h"
#include "Storage.h"

//
// Usage:
//   ItemView item = getItemAt(index);
//   Serial.println(item.title + " by " + item.artistOrAuthor);
//

#include "MediaManager.h"
#include <lvgl.h>

extern int currentCDIndex;
extern int currentBookIndex;
extern MediaMode currentMode;
extern SemaphoreHandle_t libraryMutex;

// --- Unified Item View Structure ---
// Now defined in Core_Data.h to support centralization

// --- Mode Registry (Unified Source of Truth) ---

struct ModeDefinition {
  MediaMode mode;
  String name;
  String namePlural;
  String shortName;
  String artistLabel;
  String codeLabel;
  String fileName;
  String uidPrefix;
  String mediaTerm;
  String scannerTitle;
  String artToolTitle;
  String scannerManualDesc;
  String extraInfoKey;
  String extraInfoUnit;
  bool hasTracklist;
  const char *icon;
  const char *otherModeIcon;
  int *activeIndex;
  int *ledStart;
  uint32_t *themeColor; // Pointer to the global setting
};

extern ModeDefinition registry[];

inline const ModeDefinition &getModeDef(MediaMode m) {
  // Fix: Cannot use range-based for on extern array of unknown size
  for (int i = 0; i < 2; i++) {
    if (registry[i].mode == m)
      return registry[i];
  }
  return registry[0]; // Fallback to CD
}

inline const ModeDefinition &curr() { return getModeDef(currentMode); }

inline String getModeName() { return curr().name; }
inline String getModeNamePlural() { return curr().namePlural; }
inline String getModeShortName() { return curr().shortName; }
inline String getArtistOrAuthorLabel() { return curr().artistLabel; }
inline String getCodeLabel() { return curr().codeLabel + ":"; }
inline String getArtistLabel() { return curr().artistLabel + ":"; }
inline String getLibraryFileName() { return curr().fileName; }
inline String getUidPrefix() { return curr().uidPrefix; }
inline String getMediaTerm() { return curr().mediaTerm; }
inline String getScannerTitle() { return curr().scannerTitle; }
inline String getArtToolTitle() { return curr().artToolTitle; }
inline int getSettingLedStart() { return *curr().ledStart; }
inline uint32_t getCurrentThemeColor() { return *curr().themeColor; }
inline String getExtraInfoKey() { return curr().extraInfoKey; }
inline String getExtraInfoUnit() { return curr().extraInfoUnit; }
inline bool hasTracklist() { return curr().hasTracklist; }

inline MediaMode getOtherMode() {
  return (currentMode == MODE_CD) ? MODE_BOOK : MODE_CD;
}

inline String getOtherModeNamePlural() {
  return getModeDef(getOtherMode()).namePlural;
}

inline const char *getModeIcon() { return curr().icon; }
inline const char *getOtherModeIcon() { return curr().otherModeIcon; }
inline String getScannerManualDesc() { return curr().scannerManualDesc; }
inline String getArtistOrAuthorLabelUpper() {
  String label = curr().artistLabel;
  label.toUpperCase();
  return label;
}

// --- Core Index Management ---

inline int getCurrentItemIndex() { return *curr().activeIndex; }

inline void setCurrentItemIndex(int index) { *curr().activeIndex = index; }

// --- Add/Edit Staging Helpers ---

inline ItemView getCurrentEditItem() {
  ItemView view;
  view.isValid = true;
  switch (currentMode) {
  case MODE_BOOK:
    view.title = currentEditBook.title.c_str();
    view.artistOrAuthor = currentEditBook.author.c_str();
    view.genre = currentEditBook.genre.c_str();
    view.year = currentEditBook.year;
    view.ledIndices = currentEditBook.ledIndices;
    view.uniqueID = currentEditBook.uniqueID.c_str();
    view.coverUrl = currentEditBook.coverUrl.c_str();
    view.coverFile = currentEditBook.coverFile.c_str();
    view.favorite = currentEditBook.favorite;
    view.notes = currentEditBook.notes.c_str();
    view.codecOrIsbn = currentEditBook.isbn.c_str();
    view.pageCount = currentEditBook.pageCount;
    view.currentPage = currentEditBook.currentPage;
    view.publisher = currentEditBook.publisher.c_str();
    view.detailsLoaded = currentEditBook.detailsLoaded;
    break;
  case MODE_CD:
    view.title = currentEditCD.title.c_str();
    view.artistOrAuthor = currentEditCD.artist.c_str();
    view.genre = currentEditCD.genre.c_str();
    view.year = currentEditCD.year;
    view.ledIndices = currentEditCD.ledIndices;
    view.uniqueID = currentEditCD.uniqueID.c_str();
    view.coverUrl = currentEditCD.coverUrl.c_str();
    view.coverFile = currentEditCD.coverFile.c_str();
    view.favorite = currentEditCD.favorite;
    view.notes = currentEditCD.notes.c_str();
    view.codecOrIsbn = currentEditCD.barcode.c_str();
    view.trackCount = currentEditCD.trackCount;
    view.releaseMbid = currentEditCD.releaseMbid.c_str();
    view.totalDurationMs = currentEditCD.totalDurationMs;
    view.detailsLoaded = currentEditCD.detailsLoaded;
    break;
  default:
    view.isValid = false;
    break;
  }
  return view;
}

inline void updateCurrentEditItem(const ItemView &view) {
  switch (currentMode) {
  case MODE_BOOK:
    currentEditBook.title = view.title.c_str();
    currentEditBook.author = view.artistOrAuthor.c_str();
    currentEditBook.genre = view.genre.c_str();
    currentEditBook.year = view.year;
    currentEditBook.ledIndices = view.ledIndices;
    currentEditBook.uniqueID = view.uniqueID.c_str();
    currentEditBook.coverUrl = view.coverUrl.c_str();
    currentEditBook.coverFile = view.coverFile.c_str();
    currentEditBook.favorite = view.favorite;
    currentEditBook.notes = view.notes.c_str();
    currentEditBook.isbn = view.codecOrIsbn.c_str();
    currentEditBook.pageCount = view.pageCount;
    currentEditBook.currentPage = view.currentPage;
    currentEditBook.publisher = view.publisher.c_str();
    currentEditBook.detailsLoaded = view.detailsLoaded;
    break;
  case MODE_CD:
    currentEditCD.title = view.title.c_str();
    currentEditCD.artist = view.artistOrAuthor.c_str();
    currentEditCD.genre = view.genre.c_str();
    currentEditCD.year = view.year;
    currentEditCD.ledIndices = view.ledIndices;
    currentEditCD.uniqueID = view.uniqueID.c_str();
    currentEditCD.coverUrl = view.coverUrl.c_str();
    currentEditCD.coverFile = view.coverFile.c_str();
    currentEditCD.favorite = view.favorite;
    currentEditCD.notes = view.notes.c_str();
    currentEditCD.barcode = view.codecOrIsbn.c_str();
    currentEditCD.trackCount = view.trackCount;
    currentEditCD.releaseMbid = view.releaseMbid.c_str();
    currentEditCD.totalDurationMs = view.totalDurationMs;
    currentEditCD.detailsLoaded = view.detailsLoaded;
    break;
  default:
    break;
  }
}

inline bool saveCurrentEditItem(const char *oldUniqueID = nullptr) {
  switch (currentMode) {
  case MODE_BOOK:
    return Storage.saveBook(currentEditBook, oldUniqueID);
  case MODE_CD:
    return Storage.saveCD(currentEditCD, oldUniqueID);
  default:
    return false;
  }
}

// --- Core Access Functions ---

// Find index of item by uniqueID or Barcode/ISBN
inline int findItemIndex(String query) {
  if (query.length() == 0)
    return -1;

  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);

  int found = -1;
  switch (currentMode) {
  case MODE_BOOK:
    for (int i = 0; i < (int)bookLibrary.size(); i++) {
      if (strcmp(bookLibrary[i].uniqueID.c_str(), query.c_str()) == 0 ||
          strcmp(bookLibrary[i].isbn.c_str(), query.c_str()) == 0) {
        found = i;
        break;
      }
    }
    break;
  case MODE_CD:
    for (int i = 0; i < (int)cdLibrary.size(); i++) {
      if (strcmp(cdLibrary[i].uniqueID.c_str(), query.c_str()) == 0 ||
          strcmp(cdLibrary[i].barcode.c_str(), query.c_str()) == 0) {
        found = i;
        break;
      }
    }
    break;
  case MODE_ALL:
    break;
  }

  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);

  return found;
}

// Get total item count for current mode
inline int getItemCount() {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  int count = 0;
  switch (currentMode) {
  case MODE_BOOK:
    count = bookLibrary.size();
    break;
  case MODE_CD:
    count = cdLibrary.size();
    break;
  case MODE_ALL:
    count = cdLibrary.size() + bookLibrary.size();
    break;
  default:
    count = 0;
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
  return count;
}

// Ensure item details are loaded from SD
inline void ensureItemDetailsLoaded(int index) {
  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < (int)bookLibrary.size()) {
      if (bookLibrary[index].notes.length() == 0) {
        Storage.loadBookDetail(bookLibrary[index].uniqueID.c_str(),
                               bookLibrary[index]);
      }
    }
    break;
  case MODE_CD:
    if (index >= 0 && index < (int)cdLibrary.size()) {
      // Thread Safety: Double Checked Locking pattern
      // 1. Check if already loaded to avoid mutex overhead
      if (cdLibrary[index].detailsLoaded) {
        return;
      }

      // 2. Lock
      if (libraryMutex)
        xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);

      // 3. Re-check state inside lock
      if (!cdLibrary[index].detailsLoaded) {
        Serial.printf("ensureItemDetailsLoaded: CD[%d] ID=%s detailsLoaded=%d. "
                      "Current Mbid='%s'\n",
                      index, cdLibrary[index].uniqueID.c_str(),
                      cdLibrary[index].detailsLoaded,
                      cdLibrary[index].releaseMbid.c_str());

        Serial.println("  -> Loading details from storage...");
        Storage.loadCDDetail(cdLibrary[index].uniqueID.c_str(),
                             cdLibrary[index]);
        Serial.printf("  -> Load Complete. New Mbid='%s'\n",
                      cdLibrary[index].releaseMbid.c_str());
      }

      if (libraryMutex)
        xSemaphoreGiveRecursive(libraryMutex);
    }
    break;
  default:
    break;
  }
}

// Get item title at specific index
inline String getItemTitle(int index) {
  switch (currentMode) {
  case MODE_BOOK:
    return (index >= 0 && index < (int)bookLibrary.size())
               ? bookLibrary[index].title.c_str()
               : "Unknown Book";
  case MODE_CD:
    return (index >= 0 && index < (int)cdLibrary.size())
               ? cdLibrary[index].title.c_str()
               : "Unknown CD";
  default:
    return "Unknown Item";
  }
}

inline String getItemUniqueID(int index) {
  switch (currentMode) {
  case MODE_BOOK:
    return (index >= 0 && index < (int)bookLibrary.size())
               ? bookLibrary[index].uniqueID.c_str()
               : "";
  case MODE_CD:
    return (index >= 0 && index < (int)cdLibrary.size())
               ? cdLibrary[index].uniqueID.c_str()
               : "";
  default:
    return "";
  }
}

inline String getItemCodecOrIsbn(int index) {
  switch (currentMode) {
  case MODE_BOOK:
    return (index >= 0 && index < (int)bookLibrary.size())
               ? bookLibrary[index].isbn.c_str()
               : "";
  case MODE_CD:
    return (index >= 0 && index < (int)cdLibrary.size())
               ? cdLibrary[index].barcode.c_str()
               : "";
  default:
    return "";
  }
}

// Get item at specific index (RAM-only access, no SD hit, no mutex)
inline ItemView getItemAtRAM(int index) {
  ItemView view;
  view.isValid = false;

  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < bookLibrary.size()) {
      const Book &b = bookLibrary[index];
      view.title = b.title.c_str();
      view.artistOrAuthor = b.author.c_str();
      view.genre = b.genre.c_str();
      view.year = b.year;
      view.ledIndices = b.ledIndices;
      view.uniqueID = b.uniqueID.c_str();
      view.coverFile = b.coverFile.c_str();
      view.favorite = b.favorite;
      view.codecOrIsbn = b.isbn.c_str();
      view.pageCount = b.pageCount;
      view.currentPage = b.currentPage;
      view.detailsLoaded =
          b.detailsLoaded; // Reflect if details are loaded in RAM
      if (b.currentPage > 0) {
        view.extraInfo = getExtraInfoKey() + ": " + b.isbn.c_str() +
                         " | Progress: " + String(b.currentPage) + " / " +
                         String(b.pageCount) + " " + getExtraInfoUnit();
      } else {
        view.extraInfo = getExtraInfoKey() + ": " + b.isbn.c_str() + " | " +
                         getExtraInfoUnit() + ": " + String(b.pageCount);
      }
      view.isValid = true;
    }
    break;

  case MODE_CD:
    if (index >= 0 && index < cdLibrary.size()) {
      const CD &c = cdLibrary[index];
      view.title = c.title.c_str();
      view.artistOrAuthor = c.artist.c_str();
      view.genre = c.genre.c_str();
      view.year = c.year;
      view.ledIndices = c.ledIndices;
      view.uniqueID = c.uniqueID.c_str();
      view.coverFile = c.coverFile.c_str();
      view.favorite = c.favorite;
      view.codecOrIsbn = c.barcode.c_str();
      view.trackCount = c.trackCount;
      view.detailsLoaded =
          c.detailsLoaded; // Reflect if details are loaded in RAM

      int minutes = c.totalDurationMs / 60000;
      view.extraInfo = getExtraInfoKey() + ": " + c.barcode.c_str() +
                       " | Trk: " + String(c.trackCount) + " | " +
                       String(minutes) + " " + getExtraInfoUnit();
      view.isValid = true;
    }
    break;

  case MODE_ALL:
    // Future: handle mixed mode
    break;
  }
  return view;
}

// Get item at specific index (Full details, may hit SD)
inline ItemView getItemAtSD(int index) {
  ensureItemDetailsLoaded(index); // Ensure details are loaded on SD access
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  ItemView view;
  view.isValid = false;

  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < bookLibrary.size()) {
      Book &b = bookLibrary[index];
      view.title = b.title.c_str();
      view.artistOrAuthor = b.author.c_str();
      view.genre = b.genre.c_str();
      view.year = b.year;
      view.ledIndices = b.ledIndices;
      view.uniqueID = b.uniqueID.c_str();
      view.coverUrl = b.coverUrl.c_str();
      view.coverFile = b.coverFile.c_str();
      view.favorite = b.favorite;
      view.notes = b.notes.c_str();
      view.codecOrIsbn = b.isbn.c_str();
      view.pageCount = b.pageCount;
      view.currentPage = b.currentPage;
      if (b.currentPage > 0) {
        view.extraInfo = getExtraInfoKey() + ": " + b.isbn.c_str() +
                         " | Progress: " + String(b.currentPage) + " / " +
                         String(b.pageCount) + " " + getExtraInfoUnit();
      } else {
        view.extraInfo = getExtraInfoKey() + ": " + b.isbn.c_str() + " | " +
                         getExtraInfoUnit() + ": " + String(b.pageCount);
      }
      view.isValid = true;
    }
    break;

  case MODE_CD:
    if (index >= 0 && index < cdLibrary.size()) {
      CD &c = cdLibrary[index];
      view.title = c.title.c_str();
      view.artistOrAuthor = c.artist.c_str();
      view.genre = c.genre.c_str();
      view.year = c.year;
      view.ledIndices = c.ledIndices;
      view.uniqueID = c.uniqueID.c_str();
      view.coverUrl = c.coverUrl.c_str();
      view.coverFile = c.coverFile.c_str();
      view.favorite = c.favorite;
      view.notes = c.notes.c_str();
      view.codecOrIsbn = c.barcode.c_str();

      // CRITICAL: Copy full metadata to allow lossless editing
      view.trackCount = c.trackCount;
      view.releaseMbid = c.releaseMbid.c_str();
      view.totalDurationMs = c.totalDurationMs;
      view.detailsLoaded = c.detailsLoaded;

      int minutes = c.totalDurationMs / 60000;
      view.extraInfo = getExtraInfoKey() + ": " + c.barcode.c_str() +
                       " | Trk: " + String(c.trackCount) + " | " +
                       String(minutes) + " " + getExtraInfoUnit();
      view.isValid = true;
    }
    break;

  case MODE_ALL:
    // Future: handle mixed mode
    break;
  }

  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
  return view;
}

inline void setItem(int index, const ItemView &view) {
  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < (int)bookLibrary.size()) {
      Book &b = bookLibrary[index];
      b.title = view.title.c_str();
      b.author = view.artistOrAuthor.c_str();
      b.genre = view.genre.c_str();
      b.year = view.year;
      b.uniqueID = view.uniqueID.c_str();
      b.isbn = view.codecOrIsbn.c_str();
      b.coverUrl = view.coverUrl.c_str();
      b.coverFile = view.coverFile.c_str();
      b.favorite = view.favorite;
      b.notes = view.notes.c_str();
      b.ledIndices = view.ledIndices;
      b.pageCount = view.pageCount;
      b.currentPage = view.currentPage;
      b.publisher = view.publisher.c_str();
      b.detailsLoaded = view.detailsLoaded;
    }
    break;
  case MODE_CD:
    if (index >= 0 && index < (int)cdLibrary.size()) {
      CD &c = cdLibrary[index];
      c.title = view.title.c_str();
      c.artist = view.artistOrAuthor.c_str();
      c.genre = view.genre.c_str();
      c.year = view.year;
      c.uniqueID = view.uniqueID.c_str();
      c.barcode = view.codecOrIsbn.c_str();
      c.coverUrl = view.coverUrl.c_str();
      c.coverFile = view.coverFile.c_str();
      c.favorite = view.favorite;
      c.notes = view.notes.c_str();
      c.ledIndices = view.ledIndices;
      c.trackCount = view.trackCount;
      c.releaseMbid = view.releaseMbid.c_str();
      c.totalDurationMs = view.totalDurationMs;
      c.detailsLoaded = view.detailsLoaded;
    }
    break;
  default:
    break;
  }
}

// --- Persistence Functions ---

// --- Persistence Functions ---

// Save current library to SD
// Save current library (index) to SD
inline bool saveLibrary() {
  auto &index = Storage.getIndex();
  index.clear();

  switch (currentMode) {
  case MODE_BOOK:
    for (const auto &b : bookLibrary) {
      LibraryIndexItem item;
      item.uniqueID = b.uniqueID.c_str();
      item.title = b.title.c_str();
      item.artist = b.author.c_str();
      item.coverFile = b.coverFile.c_str();
      item.year = b.year;
      item.genre = b.genre.c_str();
      item.favorite = b.favorite;
      item.ledIndices.assign(b.ledIndices.begin(), b.ledIndices.end());
      item.metaInt = b.pageCount;
      item.metaString = b.isbn.c_str();
      index.push_back(item);
    }
    break;
  case MODE_CD:
    for (const auto &c : cdLibrary) {
      LibraryIndexItem item;
      item.uniqueID = c.uniqueID.c_str();
      item.title = c.title.c_str();
      item.artist = c.artist.c_str();
      item.coverFile = c.coverFile.c_str();
      item.year = c.year;
      item.genre = c.genre.c_str();
      item.favorite = c.favorite;
      item.ledIndices.assign(c.ledIndices.begin(), c.ledIndices.end());
      item.metaInt = c.trackCount;
      item.metaString = c.barcode.c_str();
      index.push_back(item);
    }
    break;
  default:
    break;
  }

  return Storage.rewriteIndex(currentMode);
}

// Load current library from SD
inline bool loadCurrentLibrary() {
  switch (currentMode) {
  case MODE_BOOK:
    return Storage.loadIndex(MODE_BOOK);
  case MODE_CD:
    return Storage.loadIndex(MODE_CD);
  case MODE_ALL:
    return Storage.loadIndex(MODE_BOOK) && Storage.loadIndex(MODE_CD);
  default:
    return false;
  }
}

// --- Modification Functions ---

// Delete item at index
inline bool deleteItemAt(int index) {
  bool success = false;

  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < bookLibrary.size()) {
      Serial.println("Deleting book...");
      String uid = bookLibrary[index].uniqueID.c_str();
      if (Storage.deleteItem(uid, MODE_BOOK)) {
        // Storage.deleteItem updates the index file and RAM index
        // But we must also update our local 'bookLibrary' vector which mirrors
        // the index Actually, Storage.getIndex() returns a reference to the RAM
        // index. If we are using bookLibrary as a COPY, we must sync it. But
        // bookLibrary IS keeping a copy in DigitalLibrarian.ino? No,
        // DigitalLibrarian.ino populates bookLibrary FROM Storage.index. We
        // should just reload/resync or manually erase.
        bookLibrary.erase(bookLibrary.begin() + index);
        success = true;
      }
    }
    break;

  case MODE_CD:
    if (index >= 0 && index < cdLibrary.size()) {
      Serial.println("Deleting CD...");
      String uid = cdLibrary[index].uniqueID.c_str();
      if (Storage.deleteItem(uid, MODE_CD)) {
        cdLibrary.erase(cdLibrary.begin() + index);
        success = true;
      }
    }
    break;

  case MODE_ALL:
    break;
  }

  return success;
}

// Toggle favorite status at index
inline bool toggleFavoriteAt(int index) {
  bool success = false;

  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < bookLibrary.size()) {
      // Toggle RAM
      bookLibrary[index].favorite = !bookLibrary[index].favorite;
      // Save deep storage
      // Note: We need full detail to save? Storage.saveBook requires a full
      // Book object. bookLibrary[index] only has Index data (plus maybe notes
      // if lazy loaded). Ideally we load detail, toggle, save.
      Book fullBook;
      if (Storage.loadBookDetail(bookLibrary[index].uniqueID.c_str(),
                                 fullBook)) {
        fullBook.favorite = bookLibrary[index].favorite; // Apply toggle
        success = Storage.saveBook(fullBook);
      }
    }
    break;

  case MODE_CD:
    if (index >= 0 && index < cdLibrary.size()) {
      cdLibrary[index].favorite = !cdLibrary[index].favorite;
      CD fullCD;
      if (Storage.loadCDDetail(cdLibrary[index].uniqueID.c_str(), fullCD)) {
        fullCD.favorite = cdLibrary[index].favorite;
        success = Storage.saveCD(fullCD);
      }
    }
    break;

  case MODE_ALL:
    break;
  }

  return success;
}

// Get LED indices for item at index
inline std::vector<int> getItemLedIndices(int index) {
  std::vector<int> indices;

  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < bookLibrary.size()) {
      indices = bookLibrary[index].ledIndices;
    }
    break;

  case MODE_CD:
    if (index >= 0 && index < cdLibrary.size()) {
      indices = cdLibrary[index].ledIndices;
    }
    break;

  case MODE_ALL:
    // Future: handle mixed mode
    break;
  }

  return indices;
}

// --- Update Functions ---

inline void setItemID(int index, String newID) {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < (int)bookLibrary.size()) {
      String oldID = bookLibrary[index].uniqueID.c_str();
      bookLibrary[index].uniqueID = newID.c_str();

      auto &vec = Storage.getVectorForMode(MODE_BOOK);
      for (auto &item : vec) {
        if (item.uniqueID == oldID.c_str()) {
          item.uniqueID = newID.c_str();
          break;
        }
      }
    }
    break;
  case MODE_CD:
    if (index >= 0 && index < (int)cdLibrary.size()) {
      String oldID = cdLibrary[index].uniqueID.c_str();
      cdLibrary[index].uniqueID = newID.c_str();

      auto &vec = Storage.getVectorForMode(MODE_CD);
      for (auto &item : vec) {
        if (item.uniqueID == oldID.c_str()) {
          item.uniqueID = newID.c_str();
          break;
        }
      }
    }
    break;
  case MODE_ALL:
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

inline void setItemCoverFile(int index, String filename) {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < (int)bookLibrary.size()) {
      String uid = bookLibrary[index].uniqueID.c_str();
      bookLibrary[index].coverFile = filename.c_str();

      auto &vec = Storage.getVectorForMode(MODE_BOOK);
      for (auto &item : vec) {
        if (item.uniqueID == uid.c_str()) {
          item.coverFile = filename.c_str();
          break;
        }
      }
    }
    break;
  case MODE_CD:
    if (index >= 0 && index < (int)cdLibrary.size()) {
      String uid = cdLibrary[index].uniqueID.c_str();
      cdLibrary[index].coverFile = filename.c_str();

      auto &vec = Storage.getVectorForMode(MODE_CD);
      for (auto &item : vec) {
        if (item.uniqueID == uid.c_str()) {
          item.coverFile = filename.c_str();
          break;
        }
      }
    }
    break;
  case MODE_ALL:
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

inline void setItemCoverUrl(int index, String url) {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_BOOK:
    if (index >= 0 && index < (int)bookLibrary.size()) {
      bookLibrary[index].coverUrl = url.c_str();
    }
    break;
  case MODE_CD:
    if (index >= 0 && index < (int)cdLibrary.size()) {
      cdLibrary[index].coverUrl = url.c_str();
    }
    break;
  default:
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

// Get the next available LED index for the current mode
inline int getNextLedIndex() {
  int nextLed = 0;
  if (libraryMutex) {
    if (xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(1000)) != pdPASS) {
      Serial.println("!!! LOCK FAIL: getNextLedIndex");
      return 0;
    }
  }

  // 1. Find the highest assigned LED index in BOTH libraries to avoid overlap
  int maxExisting = -1;
  for (const auto &c : cdLibrary) {
    for (int l : c.ledIndices)
      if (l > maxExisting)
        maxExisting = l;
  }
  for (const auto &b : bookLibrary) {
    for (int l : b.ledIndices)
      if (l > maxExisting)
        maxExisting = l;
  }

  // 2. Start from the preferred mode start or maxExisting + 1
  int modeStart = (currentMode == MODE_BOOK) ? setting_books_led_start
                                             : setting_cds_led_start;

  // Use the larger of the two to ensure we append to the very end of the
  // populated belt
  nextLed = std::max(modeStart, maxExisting + 1);

  Serial.printf(
      "DEBUG: [getNextLedIndex] Result: %d (Mode: %d, MaxExisting: %d)\n",
      nextLed, (int)currentMode, maxExisting);

  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
  return nextLed;
}

// Add a new item to the correct library
inline void addItemToLibrary(const ItemView &item) {
  if (libraryMutex) {
    if (xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(5000)) != pdPASS) {
      Serial.println("!!! DEADLOCK: addItemToLibrary failed to get mutex");
      return;
    }
  }
  Serial.printf("addItem: Entering (%s)\n", item.title.c_str());
  switch (currentMode) {
  case MODE_BOOK: {
    Book b;
    b.title = item.title.c_str();
    b.author = item.artistOrAuthor.c_str();
    b.genre = item.genre.c_str();
    b.year = item.year;
    b.uniqueID = item.uniqueID.c_str();
    b.coverUrl = item.coverUrl.c_str();
    b.coverFile = item.coverFile.c_str();
    b.favorite = item.favorite;
    b.notes = item.notes.c_str();
    b.isbn = item.codecOrIsbn.c_str();
    b.ledIndices = item.ledIndices;
    b.pageCount = item.pageCount;
    b.currentPage = item.currentPage;
    b.detailsLoaded = item.detailsLoaded; // Preserve status if provided

    bookLibrary.push_back(b);
  } break;
  case MODE_CD: {
    CD c;
    c.title = item.title.c_str();
    c.artist = item.artistOrAuthor.c_str();
    c.genre = item.genre.c_str();
    c.year = item.year;
    c.uniqueID = item.uniqueID.c_str();
    c.coverUrl = item.coverUrl.c_str();
    c.coverFile = item.coverFile.c_str();
    c.favorite = item.favorite;
    c.notes = item.notes.c_str();
    c.barcode = item.codecOrIsbn.c_str();
    c.ledIndices = item.ledIndices;
    c.trackCount = item.trackCount;
    c.releaseMbid = item.releaseMbid.c_str();
    c.totalDurationMs = item.totalDurationMs;
    c.detailsLoaded = item.detailsLoaded; // Preserve status if provided

    Serial.println("--- ADDING CD TO LIBRARY (Lazy Index) ---");
    Serial.printf("ID: %s | Title: %s | DetailsLoaded: %d | LEDs: %d\n",
                  c.uniqueID.c_str(), c.title.c_str(), c.detailsLoaded,
                  c.ledIndices.size());

    cdLibrary.push_back(c);
  } break;
  default:
    break;
  }
  Serial.println("addItem: Giving mutex");
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

// --- Metadata Fetching (Unified) ---

// --- Metadata Fetching (Unified) ---

inline bool fetchModeMetadata(String code, ItemView &out) {
  out.isValid = false;
  switch (currentMode) {
  case MODE_BOOK:
    return MediaManager::fetchMetadataForISBN(code.c_str(), out);
  case MODE_CD:
    return MediaManager::fetchMetadataForBarcode(code.c_str(), out);
  default:
    return false;
  }
}

// Fetch cover URL for an item at index
inline String fetchCoverUrlForIndex(int index) {
  ItemView item = getItemAtSD(index);
  if (!item.isValid)
    return "";

  switch (currentMode) {
  case MODE_BOOK:
    if (item.codecOrIsbn.length() > 0) {
      Book tmp;
      if (MediaManager::fetchBookByISBN(item.codecOrIsbn.c_str(), tmp)) {
        Serial.printf("fetchCoverUrlForIndex: Book ISBN %s -> URL: %s\n",
                      item.codecOrIsbn.c_str(), tmp.coverUrl.c_str());
        return tmp.coverUrl.c_str();
      } else {
        Serial.printf(
            "fetchCoverUrlForIndex: Failed to fetch book for ISBN %s\n",
            item.codecOrIsbn.c_str());
      }
    }
    break;
  case MODE_CD:
    return MediaManager::fetchAlbumCoverUrl(item.artistOrAuthor.c_str(),
                                            item.title.c_str());
  default:
    break;
  }
  return "";
}

// --- Library Management ---

inline void clearCurrentLibrary() {
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  switch (currentMode) {
  case MODE_BOOK:
    bookLibrary.clear();
    break;
  case MODE_CD:
    cdLibrary.clear();
    break;
  default:
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

inline void syncLibraryFromStorage() {
  Serial.println("syncLibrary: Attempting lock...");
  if (libraryMutex) {
    if (xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(5000)) != pdPASS) {
      Serial.println("!!! DEADLOCK: syncLibrary failed to get mutex");
      return;
    }
  }
  Serial.println("syncLibrary: Lock acquired.");

  clearCurrentLibrary();
  for (auto &item : Storage.getIndex()) {
    ItemView view;
    view.uniqueID = item.uniqueID.c_str();
    view.title = item.title.c_str();
    view.artistOrAuthor = item.artist.c_str();
    view.coverFile = item.coverFile.c_str();
    view.year = item.year;
    view.genre = item.genre.c_str();
    view.favorite = item.favorite;
    view.ledIndices.assign(item.ledIndices.begin(), item.ledIndices.end());
    view.isValid = true;

    // Populate generic metadata
    switch (currentMode) {
    case MODE_BOOK:
      view.pageCount = item.metaInt;
      view.codecOrIsbn = item.metaString.c_str();
      break;
    case MODE_CD:
      view.trackCount = item.metaInt;
      view.codecOrIsbn = item.metaString.c_str();
      break;
    default:
      break;
    }

    addItemToLibrary(view);
  }
  Serial.println("syncLibrary: Giving mutex");
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

// --- Sorting Functions ---

inline void sortByArtistOrAuthor() {
  switch (currentMode) {
  case MODE_BOOK:
    std::sort(bookLibrary.begin(), bookLibrary.end(),
              [](const Book &a, const Book &b) {
                String authorA = a.author.c_str();
                authorA.toLowerCase();
                String authorB = b.author.c_str();
                authorB.toLowerCase();
                if (authorA != authorB)
                  return authorA < authorB;
                return strcmp(a.title.c_str(), b.title.c_str()) < 0;
              });
    break;

  case MODE_CD:
    std::sort(cdLibrary.begin(), cdLibrary.end(), [](const CD &a, const CD &b) {
      String artistA = a.artist.c_str();
      artistA.toLowerCase();
      String artistB = b.artist.c_str();
      artistB.toLowerCase();
      if (artistA != artistB)
        return artistA < artistB;
      return a.year < b.year;
    });
    break;

  case MODE_ALL:
    // Future: handle mixed mode sorting
    break;
  }
}

inline void sortByLedIndex() {
  switch (currentMode) {
  case MODE_BOOK:
    std::sort(bookLibrary.begin(), bookLibrary.end(),
              [](const Book &a, const Book &b) {
                int aL = a.ledIndices.empty() ? 0 : a.ledIndices[0];
                int bL = b.ledIndices.empty() ? 0 : b.ledIndices[0];
                return aL < bL;
              });
    break;

  case MODE_CD:
    std::sort(cdLibrary.begin(), cdLibrary.end(), [](const CD &a, const CD &b) {
      int aL = a.ledIndices.empty() ? 0 : a.ledIndices[0];
      int bL = b.ledIndices.empty() ? 0 : b.ledIndices[0];
      return aL < bL;
    });
    break;

  case MODE_ALL:
    // Future: handle mixed mode sorting
    break;
  }
}

// --- Future Extension Template ---
//
// To add a new mode (e.g., MODE_VINYL, MODE_GAME):
//
// 1. Add to MediaMode enum in Core_Data.h.
// 2. Add an entry to the `registry` array at the top of this file.
// 3. Update the switch statements in functions that need deep logic (sorting,
// item views).
//
// All UI and high-level functions will automatically support the new mode.
//

#endif // MODE_ABSTRACTION_H
