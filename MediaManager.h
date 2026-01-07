#ifndef MEDIA_MANAGER_H
#define MEDIA_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <vector>

#include "Core_Data.h"

// Forward declaration of search pagination state
extern std::vector<int> search_matches;
extern int search_display_offset;
extern const int SEARCH_PAGE_SIZE;

// Exposed helper
LyricsResult fetchLyricsIfNeeded(const char *releaseMbid, int trackIndex,
                                 bool force = false);

class MediaManager {
public:
  static void init();

  // Core Library Sync
  static void syncFromStorage();

  // Search & Filter
  static void filter(const char *query, int filterMode, bool ledMasterOn);

  // Metadata Fetching (Online)
  static bool fetchMetadataForBarcode(const char *barcode, ItemView &outView);
  static bool fetchMetadataForISBN(const char *isbn, ItemView &outView);

  // Sorting
  static void sortByArtistOrAuthor();
  static void sortByLedIndex();

  // Background Task Handling
  static void startBackgroundTask();
  static bool isTaskBusy();

  // Internal API Helpers (Made Public for Abstraction Layer)
  static MBRelease fetchReleaseByBarcode(const char *barcode);
  static MBRelease fetchReleaseFromDiscogs(const char *barcode); // Fallback API
  static std::vector<Track> fetchTracklist(const char *releaseMbid,
                                           String *outGenre = NULL);
  static bool fetchBookByISBN(const char *isbn, Book &book);
  static String fetchAlbumCoverUrl(const char *artist, const char *album);

private:
  static bool _taskBusy;
};

#endif // MEDIA_MANAGER_H
