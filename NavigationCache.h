#ifndef NAVIGATION_CACHE_H
#define NAVIGATION_CACHE_H

#include "Core_Data.h"
#include "Storage.h"
#include "mode_abstraction.h" // Needed for ensureItemDetailsLoaded and getItemCount
#include <Arduino.h>

// Initialize the cache for the current mode
inline void initNavigationCache() {
  Serial.println("Initializing navigation cache...");

  // Set cache size based on user setting (default 5 items per side = 11 total)
  int itemsPerSide = setting_cache_size;
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

// Load an item from SD into cache at specific cache index
inline bool loadItemIntoCache(int libraryIndex, int cacheIndex) {
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
      PsramString uniqueID = cdLibrary[libraryIndex].uniqueID;
      bool success =
          Storage.loadCDDetail(uniqueID.c_str(), navCache.cdCache[cacheIndex]);
      navCache.cdCacheValid[cacheIndex] = success;
      if (success) {
        cdLibrary[libraryIndex].detailsLoaded = true;
      }
      return success;
    }
    break;

  case MODE_BOOK:
    if (libraryIndex >= 0 && libraryIndex < (int)bookLibrary.size()) {
      PsramString uniqueID = bookLibrary[libraryIndex].uniqueID;
      bool success = Storage.loadBookDetail(uniqueID.c_str(),
                                            navCache.bookCache[cacheIndex]);
      navCache.bookCacheValid[cacheIndex] = success;
      if (success) {
        bookLibrary[libraryIndex].detailsLoaded = true;
      }
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

// Get item from cache if available, otherwise load from SD
inline ItemView getItemFromCache(int libraryIndex) {
  int cacheStartIndex = (currentMode == MODE_CD) ? navCache.cdCacheStartIndex
                                                 : navCache.bookCacheStartIndex;

  if (cacheStartIndex > -MAX_CACHE_WINDOW_SIZE) {
    int cacheOffset = libraryIndex - cacheStartIndex;

    if (cacheOffset >= 0 && cacheOffset < navCache.cacheSize) {
      bool isValid = (currentMode == MODE_CD)
                         ? navCache.cdCacheValid[cacheOffset]
                         : navCache.bookCacheValid[cacheOffset];

      if (isValid) {
        // Build view from cache
        if (currentMode == MODE_CD) {
          const CD &c = navCache.cdCache[cacheOffset];
          // ... build view ... (calling getItemAtRAM or similar logic)
          return getItemAtRAM(
              libraryIndex); // Use RAM getter since it's already in cache
        } else if (currentMode == MODE_BOOK) {
          return getItemAtRAM(libraryIndex);
        }
      }
    }
  }

  // Cache MISS
  return getItemAtSD(libraryIndex);
}

extern bool filter_active;

// Global wrapper to use cache for all lookups
inline ItemView getItemAt(int index) {
  if (filter_active)
    return getItemAtSD(index); // Bypass cache during filtering
  return getItemFromCache(index);
}

// Shift cache window (for NEXT/PREV navigation)
inline void shiftCacheWindow(bool forward) {
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
