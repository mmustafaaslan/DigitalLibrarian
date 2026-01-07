#ifndef CORE_DATA_H
#define CORE_DATA_H

#include "PsramAllocator.h"
#include <Arduino.h>
#include <string>
#include <vector>

// Define a String class that uses PSRAM
typedef std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>
    PsramString;

// Define an Int Vector that uses PSRAM (for LED indices)
typedef std::vector<int, PsramAllocator<int>> PsramIntVector;

// --- Lyrics Metadata Structure ---
struct LyricsMetadata {
  PsramString status;      // "unchecked", "cached", "missing"
  PsramString path;        // "/lyrics/xxx/01.json"
  PsramString fetchedAt;   // ISO-8601 timestamp
  PsramString lastTriedAt; // ISO-8601 timestamp (for missing)
  PsramString lang;        // "en", "fr", etc.
  PsramString error;       // Error message if failed
  int offset = 0;          // Synchronization offset in milliseconds
};

// --- Track Structure ---
struct Track {
  int trackNo;
  PsramString title;
  unsigned long durationMs;
  PsramString recordingMbid;
  LyricsMetadata lyrics;
  bool isFavoriteTrack = false; // NEW: Track favorite status
};

// --- TrackList Structure (loaded on-demand from SD) ---
struct TrackList {
  PsramString releaseMbid;
  PsramString cdTitle;
  PsramString cdArtist;
  PsramString fetchedAt;
  std::vector<Track, PsramAllocator<Track>> tracks;
};

// Chapter/ChapterList structs removed

// --- MusicBrainz Release Result Structure ---
struct MBRelease {
  String releaseMbid;
  String title;
  String artist;
  String genre; // Added to support Discogs genre data
  int year;
  bool success;
};

// --- Lyrics Fetch Result Enum ---
enum LyricsResult {
  LYRICS_ALREADY_CACHED,
  LYRICS_FETCHED_NOW,
  LYRICS_NOT_FOUND,
  LYRICS_ERROR
};

// --- Core Media Structures ---

// Note: In the future, CD and Book could inherit from a common 'MediaItem' base
// class. For now, we preserve the existing layout to minimize breakage during
// refactoring.

struct CD {
  PsramString title = "";
  PsramString artist = "";
  PsramString genre = "Unknown";
  int year = 0;
  std::vector<int> ledIndices;
  PsramString uniqueID = "";
  PsramString coverUrl = "";
  PsramString coverFile = "";
  bool favorite = false;
  PsramString notes = "";
  PsramString barcode = "";

  // NEW FIELDS - Tracklist & Lyrics Feature
  PsramString releaseMbid = "";      // MusicBrainz Release ID
  int trackCount = 0;                // Number of tracks (for quick display)
  unsigned long totalDurationMs = 0; // Total album duration

  // Book-specific fields (legacy compatibility)
  PsramString isbn = "";
  PsramString publisher = "";
  int pageCount = 0;

  bool detailsLoaded = false; // Runtime flag
};

struct Book {
  PsramString title = "";        // "1984"
  PsramString author = "";       // "George Orwell"
  PsramString genre = "Unknown"; // "Fiction", "Science", "Biography"
  int year = 0;                  // 1949
  std::vector<int> ledIndices;   // Physical shelf location(s)
  PsramString uniqueID = "";     // "orwell_1984_1949"
  PsramString coverUrl = "";     // Google Books API cover URL
  PsramString coverFile = "";    // Filename on SD card
  bool favorite = false;
  PsramString notes = "";
  PsramString isbn = "";
  PsramString publisher = "";
  int pageCount = 0;
  int currentPage = 0;

  bool detailsLoaded = false; // Runtime flag
};

#include <FastLED.h>

// --- Global Hardware & State Externs ---
extern int led_count;
extern bool led_master_on;
extern CRGB *leds;
extern CRGB COLOR_FAVORITE;
extern CRGB COLOR_SELECTED;
extern CRGB COLOR_FILTERED;

// --- Unified Item View Structure ---
// Provides a common interface for all item types
struct ItemView {
  String title;
  String artistOrAuthor;
  String genre;
  int year;
  std::vector<int> ledIndices;
  String uniqueID;
  String coverUrl;
  String coverFile;
  bool favorite;
  String notes;
  String codecOrIsbn; // Barcode for CDs, ISBN for Books

  // Type-specific extras
  String extraInfo; // For display: "ISBN: xxx | Pages: yyy" or "Barcode: xxx |
                    // Tracks: yyy"
  int pageCount = 0;
  int currentPage = 0;
  int trackCount = 0;
  uint32_t totalDurationMs = 0; // Added for CD
  String releaseMbid;           // Hidden metadata
  String publisher;             // Hidden metadata
  bool detailsLoaded = false;   // Track if full record is in RAM
  bool isValid;
};

// Media mode toggle
enum MediaMode { MODE_CD, MODE_BOOK, MODE_ALL };
extern MediaMode currentMode;

// --- Global Library & Index Externs ---
#include "PsramAllocator.h"

// ...

// --- Global Library & Index Externs ---
// Use PSRAM Allocator to support 1000+ items
typedef std::vector<CD, PsramAllocator<CD>> CDVector;
typedef std::vector<Book, PsramAllocator<Book>> BookVector;

extern CDVector cdLibrary;
extern int currentCDIndex;
extern BookVector bookLibrary;
extern int currentBookIndex;

// --- Global Edit State Externs ---
extern CD currentEditCD;
extern Book currentEditBook;

// --- Sliding Window Cache for Fast Navigation ---
// Cache holds current item + N before + N after for instant navigation
// Max cache size to support (user can configure smaller)
#define MAX_CACHE_WINDOW_SIZE 31 // Support up to 15 items per side

struct NavigationCache {
  CD cdCache[MAX_CACHE_WINDOW_SIZE];
  Book bookCache[MAX_CACHE_WINDOW_SIZE];
  int cdCacheStartIndex;   // Library index of cache[0]
  int bookCacheStartIndex; // Library index of cache[0]
  bool cdCacheValid[MAX_CACHE_WINDOW_SIZE];
  bool bookCacheValid[MAX_CACHE_WINDOW_SIZE];
  int cacheSize;   // Actual cache size (user configurable)
  int cacheCenter; // Center index (cacheSize / 2)
};

extern NavigationCache navCache;

// --- Settings Externs ---
extern bool setting_enable_cds;
extern bool setting_enable_books;
extern int setting_books_led_start;
extern int setting_cds_led_start;
extern uint32_t setting_theme_cd;
extern uint32_t setting_theme_book;
extern int setting_cache_size; // Items per side: 5, 10, or 15

#endif // CORE_DATA_H
