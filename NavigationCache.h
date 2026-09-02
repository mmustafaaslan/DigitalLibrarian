#ifndef NAVIGATION_CACHE_H
#define NAVIGATION_CACHE_H

#include "Core_Data.h"
#include "Storage.h"
#include "mode_abstraction.h" // Needed for ensureItemDetailsLoaded and getItemCount
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <new>

inline bool ensureNavigationCacheAllocated() {
  if (navCacheStorage)
    return true;

  void *storage = heap_caps_malloc(sizeof(NavigationCache),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (storage) {
    navCacheStorage = new (storage) NavigationCache{};
    Serial.printf("Navigation cache allocated in PSRAM (%u bytes)\n",
                  (unsigned)sizeof(NavigationCache));
  } else {
    // Retain a functional fallback if PSRAM is unexpectedly unavailable.
    navCacheStorage = new (std::nothrow) NavigationCache{};
    Serial.println("WARNING: Navigation cache fell back to internal RAM");
  }
  return navCacheStorage != nullptr;
}

// Initialize the cache for the current mode
inline void initNavigationCache() {
  Serial.println("Initializing navigation cache...");
  if (!ensureNavigationCacheAllocated()) {
    Serial.println("ERROR: Navigation cache allocation failed");
    return;
  }

  // Set cache size based on user setting (default 5 items per side = 11 total)
  int itemsPerSide = constrain(setting_cache_size, 1, 15);
  navCache.cacheSize = (itemsPerSide * 2) + 1; // e.g., 5*2+1 = 11
  navCache.cacheCenter = itemsPerSide;         // e.g., 5

  Serial.printf("Cache size: %d items (%d per side)\n", navCache.cacheSize,
                itemsPerSide);

  // Clear all validity flags
  for (int i = 0; i < MAX_CACHE_WINDOW_SIZE; i++) {
    navCache.cdCacheValid[i] = false;
    navCache.bookCacheValid[i] = false;
  }

  navCache.cdCacheStartIndex = -1;
  navCache.bookCacheStartIndex = -1;

  Serial.println("Navigation cache initialized");
}

inline void invalidateNavigationCache() {
  if (!ensureNavigationCacheAllocated())
    return;
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_CACHE_WINDOW_SIZE; i++) {
    navCache.cdCacheValid[i] = false;
    navCache.bookCacheValid[i] = false;
  }
  navCache.cdCacheStartIndex = -1;
  navCache.bookCacheStartIndex = -1;
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

// Load an item from SD into cache at specific cache index
inline bool loadItemIntoCache(int libraryIndex, int cacheIndex) {
  if (!ensureNavigationCacheAllocated())
    return false;
  if (cacheIndex < 0 || cacheIndex >= navCache.cacheSize) {
    return false;
  }

  // Clear previous validity
  switch (currentMode) {
  case MODE_CD:
    navCache.cdCacheValid[cacheIndex] = false;
    break;
  case MODE_BOOK:
    navCache.bookCacheValid[cacheIndex] = false;
    break;
  default:
    break;
  }

  if (libraryIndex < 0 || libraryIndex >= getItemCount()) {
    return false;
  }

  switch (currentMode) {
  case MODE_CD:
    if (libraryIndex >= 0 && libraryIndex < (int)cdLibrary.size()) {
      const CD &libraryItem = cdLibrary[libraryIndex];
      bool success = false;
      if (libraryItem.detailsLoaded) {
        navCache.cdCache[cacheIndex] = libraryItem;
        success = true;
      } else {
        success = Storage.loadCDDetail(libraryItem.uniqueID.c_str(),
                                       navCache.cdCache[cacheIndex]);
      }
      navCache.cdCacheValid[cacheIndex] = success;
      return success;
    }
    break;

  case MODE_BOOK:
    if (libraryIndex >= 0 && libraryIndex < (int)bookLibrary.size()) {
      const Book &libraryItem = bookLibrary[libraryIndex];
      bool success = false;
      if (libraryItem.detailsLoaded) {
        navCache.bookCache[cacheIndex] = libraryItem;
        success = true;
      } else {
        success = Storage.loadBookDetail(libraryItem.uniqueID.c_str(),
                                         navCache.bookCache[cacheIndex]);
      }
      navCache.bookCacheValid[cacheIndex] = success;
      return success;
    }
    break;

  default:
    break;
  }

  return false;
}

// Rebuild cache centered on current index
inline void rebuildNavigationCache(int centerIndex) {
  if (!ensureNavigationCacheAllocated())
    return;
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);

  Serial.printf("Rebuilding navigation cache centered on index %d\n",
                centerIndex);

  int totalItems = getItemCount();
  if (totalItems == 0) {
    initNavigationCache();
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
    return;
  }

  // Calculate start index (center - N)
  int startIndex = centerIndex - navCache.cacheCenter;

  switch (currentMode) {
  case MODE_CD:
    navCache.cdCacheStartIndex = startIndex;
    for (int i = 0; i < navCache.cacheSize; i++) {
      loadItemIntoCache(startIndex + i, i);
    }
    break;

  case MODE_BOOK:
    navCache.bookCacheStartIndex = startIndex;
    for (int i = 0; i < navCache.cacheSize; i++) {
      loadItemIntoCache(startIndex + i, i);
    }
    break;

  default:
    break;
  }

  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

inline ItemView buildCachedCDView(const CD &detail, int libraryIndex) {
  ItemView view{};
  const CD &indexItem = cdLibrary[libraryIndex];
  view.title = indexItem.title.c_str();
  view.artistOrAuthor = indexItem.artist.c_str();
  view.genre = indexItem.genre.c_str();
  view.year = indexItem.year;
  view.ledIndices = indexItem.ledIndices;
  view.uniqueID = indexItem.uniqueID.c_str();
  view.coverFile = indexItem.coverFile.c_str();
  view.favorite = indexItem.favorite;
  view.codecOrIsbn = indexItem.barcode.c_str();
  view.trackCount = indexItem.trackCount;

  view.coverUrl = detail.coverUrl.c_str();
  view.notes = detail.notes.c_str();
  view.releaseMbid = detail.releaseMbid.c_str();
  view.totalDurationMs = detail.totalDurationMs;
  view.detailsLoaded = true;
  int minutes = detail.totalDurationMs / 60000;
  view.extraInfo = getExtraInfoKey() + ": " + String(indexItem.barcode.c_str()) +
                   " | Trk: " + String(indexItem.trackCount) + " | " +
                   String(minutes) + " " + getExtraInfoUnit();
  view.isValid = true;
  return view;
}

inline ItemView buildCachedBookView(const Book &detail, int libraryIndex) {
  ItemView view{};
  const Book &indexItem = bookLibrary[libraryIndex];
  view.title = indexItem.title.c_str();
  view.artistOrAuthor = indexItem.author.c_str();
  view.genre = indexItem.genre.c_str();
  view.year = indexItem.year;
  view.ledIndices = indexItem.ledIndices;
  view.uniqueID = indexItem.uniqueID.c_str();
  view.coverFile = indexItem.coverFile.c_str();
  view.favorite = indexItem.favorite;
  view.codecOrIsbn = indexItem.isbn.c_str();
  view.pageCount = indexItem.pageCount;
  view.currentPage = indexItem.currentPage;

  view.coverUrl = detail.coverUrl.c_str();
  view.notes = detail.notes.c_str();
  view.publisher = detail.publisher.c_str();
  view.detailsLoaded = true;
  if (indexItem.currentPage > 0) {
    view.extraInfo = getExtraInfoKey() + ": " + String(indexItem.isbn.c_str()) +
                     " | Progress: " + String(indexItem.currentPage) + " / " +
                     String(indexItem.pageCount) + " " + getExtraInfoUnit();
  } else {
    view.extraInfo = getExtraInfoKey() + ": " + String(indexItem.isbn.c_str()) +
                     " | " + getExtraInfoUnit() + ": " +
                     String(indexItem.pageCount);
  }
  view.isValid = true;
  return view;
}

// Get item from cache if available, otherwise load from SD
inline ItemView getItemFromCache(int libraryIndex) {
  ItemView result{};
  result.isValid = false;
  if (!ensureNavigationCacheAllocated())
    return getItemAtSD(libraryIndex);
  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);

  const int totalItems = currentMode == MODE_CD
                             ? (int)cdLibrary.size()
                             : (currentMode == MODE_BOOK
                                    ? (int)bookLibrary.size()
                                    : 0);
  if (libraryIndex < 0 || libraryIndex >= totalItems) {
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
    return result;
  }

  int cacheStartIndex = (currentMode == MODE_CD) ? navCache.cdCacheStartIndex
                                                 : navCache.bookCacheStartIndex;

  if (cacheStartIndex > -MAX_CACHE_WINDOW_SIZE) {
    int cacheOffset = libraryIndex - cacheStartIndex;

    if (cacheOffset >= 0 && cacheOffset < navCache.cacheSize) {
      bool isValid = (currentMode == MODE_CD)
                         ? navCache.cdCacheValid[cacheOffset]
                         : navCache.bookCacheValid[cacheOffset];

      if (isValid) {
        if (currentMode == MODE_CD) {
          result = buildCachedCDView(navCache.cdCache[cacheOffset], libraryIndex);
        } else if (currentMode == MODE_BOOK) {
          result = buildCachedBookView(navCache.bookCache[cacheOffset],
                                       libraryIndex);
        }
        if (libraryMutex)
          xSemaphoreGiveRecursive(libraryMutex);
        return result;
      }
    }
  }

  // Cache MISS
  result = getItemAtSD(libraryIndex);
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
  return result;
}

extern bool filter_active;

// Global wrapper to use cache for all lookups
inline ItemView getItemAt(int index) {
  return getItemFromCache(index);
}

// Shift cache window (for NEXT/PREV navigation)
inline void shiftCacheWindow(bool forward) {
  if (!ensureNavigationCacheAllocated())
    return;
  if (filter_active)
    return; // No cache operations during filtering

  if (libraryMutex)
    xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);

  int currentIndex = getCurrentItemIndex();
  int totalItems = getItemCount();

  if (totalItems == 0) {
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
    return;
  }

  int cacheStartIndex = (currentMode == MODE_CD) ? navCache.cdCacheStartIndex
                                                 : navCache.bookCacheStartIndex;

  int distanceFromCenter =
      currentIndex - (cacheStartIndex + navCache.cacheCenter);

  // USER LOGIC: If we are STILL inside the cache window buffer, do nothing!
  if (currentIndex >= cacheStartIndex &&
      currentIndex < (cacheStartIndex + navCache.cacheSize)) {
    if (abs(distanceFromCenter) < (navCache.cacheCenter - 1)) {
      if (libraryMutex)
        xSemaphoreGiveRecursive(libraryMutex);
      return;
    }
  }

  // Outside or near edge - Rebuild or Shift
  if (abs(distanceFromCenter) > navCache.cacheCenter) {
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
    rebuildNavigationCache(currentIndex);
    return;
  } else {
    // Proactive shift by 1
    if (currentMode == MODE_CD) {
      if (forward) {
        for (int i = 0; i < navCache.cacheSize - 1; i++) {
          navCache.cdCache[i] = navCache.cdCache[i + 1];
          navCache.cdCacheValid[i] = navCache.cdCacheValid[i + 1];
        }
        navCache.cdCacheStartIndex++;
        loadItemIntoCache(navCache.cdCacheStartIndex + navCache.cacheSize - 1,
                          navCache.cacheSize - 1);
      } else {
        for (int i = navCache.cacheSize - 1; i > 0; i--) {
          navCache.cdCache[i] = navCache.cdCache[i - 1];
          navCache.cdCacheValid[i] = navCache.cdCacheValid[i - 1];
        }
        navCache.cdCacheStartIndex--;
        loadItemIntoCache(navCache.cdCacheStartIndex, 0);
      }
    } else if (currentMode == MODE_BOOK) {
      if (forward) {
        for (int i = 0; i < navCache.cacheSize - 1; i++) {
          navCache.bookCache[i] = navCache.bookCache[i + 1];
          navCache.bookCacheValid[i] = navCache.bookCacheValid[i + 1];
        }
        navCache.bookCacheStartIndex++;
        loadItemIntoCache(navCache.bookCacheStartIndex + navCache.cacheSize - 1,
                          navCache.cacheSize - 1);
      } else {
        for (int i = navCache.cacheSize - 1; i > 0; i--) {
          navCache.bookCache[i] = navCache.bookCache[i - 1];
          navCache.bookCacheValid[i] = navCache.bookCacheValid[i - 1];
        }
        navCache.bookCacheStartIndex--;
        loadItemIntoCache(navCache.bookCacheStartIndex, 0);
      }
    }
  }

  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
}

#endif // NAVIGATION_CACHE_H
