#ifndef STORAGE_TESTS_H
#define STORAGE_TESTS_H

#include "AppGlobals.h"
#include "Storage.h"
#include <Arduino.h>
#include <Waveshare_ST7262_LVGL.h>
#include <vector>

class StorageTests {
public:
  static String runTests() {
    String log = "";
    int passed = 0;
    int failed = 0;

    auto runAssert = [&](bool condition, String name) {
      if (condition) {
        log += "✅ PASS: " + name + "\n";
        passed++;
        Serial.println("[TEST] PASS: " + name);
      } else {
        log += "❌ FAIL: " + name + "\n";
        failed++;
        Serial.println("[TEST] FAIL: " + name);
      }
    };

    auto checkFileExists = [](String path) {
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, LOW);
      bool exists = SD.exists(path);
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, HIGH);
      return exists;
    };

    auto compareCDs = [](const CD &a, const CD &b) {
      if (a.uniqueID != b.uniqueID)
        return false;
      if (a.title != b.title)
        return false;
      if (a.artist != b.artist)
        return false;
      if (a.genre != b.genre)
        return false;
      if (a.year != b.year)
        return false;
      if (a.barcode != b.barcode)
        return false;
      if (a.releaseMbid != b.releaseMbid)
        return false;
      if (a.trackCount != b.trackCount)
        return false;
      if (a.totalDurationMs != b.totalDurationMs)
        return false;
      if (a.coverUrl != b.coverUrl)
        return false;
      if (a.coverFile != b.coverFile)
        return false;
      if (a.favorite != b.favorite)
        return false;
      if (a.notes != b.notes)
        return false;
      if (a.ledIndices.size() != b.ledIndices.size())
        return false;
      for (size_t i = 0; i < a.ledIndices.size(); i++) {
        if (a.ledIndices[i] != b.ledIndices[i])
          return false;
      }
      return true;
    };

    log += "=== Digital Librarian Comprehensive Storage Tests ===\n";

    // --- CD TEST SUITE ---
    log += "\n[CD Suite]\n";
    CD testCD;
    testCD.uniqueID = "TEST_CD_COMP";
    testCD.title = "Complex Data Test";
    testCD.artist = "Japanese Artist (日本語)";
    testCD.genre = "Classical/Jazz";
    testCD.year = 2026;
    testCD.trackCount = 20;
    testCD.totalDurationMs = 74 * 60 * 1000;
    testCD.ledIndices = {1, 2, 3};
    testCD.barcode = "400012345678";
    testCD.releaseMbid = "mbid-123-456";
    testCD.coverUrl = "http://art.com/img.jpg";
    testCD.coverFile = "/db/covers/test.jpg";
    testCD.favorite = true;
    testCD.notes = "Line 1\n\"Quotes\"\nLine 3";

    runAssert(Storage.saveCD(testCD), "CD Initial Save");

    // Test ID Rename (Cornerstone for "Perfect" persistence)
    String oldID = testCD.uniqueID.c_str();
    testCD.uniqueID = "TEST_CD_RENAMED";
    testCD.title = "Renamed Title";

    runAssert(Storage.saveCD(testCD, oldID.c_str()),
              "CD Save with Rename (Old ID cleanup)");

    // Verify old file is GONE
    runAssert(!checkFileExists("/db/cds/" + oldID + ".json"),
              "Old ID File Deleted");
    // Verify new file EXISTS
    runAssert(checkFileExists("/db/cds/TEST_CD_RENAMED.json"),
              "New ID File Created");

    CD loadedCD;
    runAssert(Storage.loadCDDetail("TEST_CD_RENAMED", loadedCD),
              "Load Renamed CD Detail");
    runAssert(loadedCD.coverUrl == testCD.coverUrl,
              "Non-UI Field Preservation (coverUrl)");
    runAssert(loadedCD.releaseMbid == testCD.releaseMbid,
              "Non-UI Field Preservation (MBID)");
    runAssert(loadedCD.trackCount == testCD.trackCount,
              "Non-UI Field Preservation (Tracks)");

    // --- NEW: LED SUITE ---
    log += "\n[LED Preservation Suite]\n";
    testCD.ledIndices.clear();
    testCD.ledIndices.push_back(10);
    testCD.ledIndices.push_back(11);
    testCD.ledIndices.push_back(12);
    runAssert(Storage.saveCD(testCD), "CD Save with 3 LEDs");
    CD ledCD;
    Storage.loadCDDetail(testCD.uniqueID.c_str(), ledCD);
    runAssert(ledCD.ledIndices.size() == 3, "LED Count Persisted");
    if (ledCD.ledIndices.size() == 3) {
      runAssert(ledCD.ledIndices[0] == 10 && ledCD.ledIndices[2] == 12,
                "LED Values Persisted");
    }

    // --- BOOK TEST SUITE ---
    log += "\n[Book Suite]\n";
    Book testBook;
    testBook.uniqueID = "TEST_BOOK_COMP";
    testBook.title = "Persistent Storage Manual";
    testBook.author = "Antigravity";
    testBook.isbn = "123-456-789";
    testBook.pageCount = 999;
    testBook.publisher = "DeepMind Press";
    testBook.coverUrl = "http://books.com/cover.jpg";
    testBook.ledIndices.push_back(200);

    runAssert(Storage.saveBook(testBook), "Book Initial Save");

    String oldBookID = testBook.uniqueID.c_str();
    testBook.uniqueID = "TEST_BOOK_RENAMED";
    runAssert(Storage.saveBook(testBook, oldBookID.c_str()),
              "Book Save with Rename");
    runAssert(!checkFileExists("/db/books/" + oldBookID + ".json"),
              "Old Book File Deleted");

    Book testLoadedBook; // Renamed to avoid collision
    Storage.loadBookDetail("TEST_BOOK_RENAMED", testLoadedBook);
    runAssert(testLoadedBook.ledIndices.size() == 1,
              "Book LED count preserved");
    if (!testLoadedBook.ledIndices.empty()) {
      runAssert(testLoadedBook.ledIndices[0] == 200,
                "Book LED value preserved");
    }

    runAssert(testLoadedBook.coverUrl == testBook.coverUrl,
              "Book coverUrl Preservation");
    runAssert(testLoadedBook.publisher == testBook.publisher,
              "Book publisher Preservation");

    // --- TRACKLIST TEST SUITE ---
    log += "\n[Tracklist Suite]\n";
    TrackList tl;
    tl.releaseMbid = testCD.releaseMbid.c_str();
    tl.cdTitle = testCD.title.c_str();

    Track t1;
    t1.trackNo = 1;
    t1.title = "First";
    t1.isFavoriteTrack = true;
    tl.tracks.push_back(t1);

    runAssert(Storage.saveTracklist(tl.releaseMbid.c_str(), &tl),
              "Save Tracklist");
    TrackList *lTl = Storage.loadTracklist(tl.releaseMbid.c_str());
    if (lTl) {
      runAssert(lTl->tracks.size() == 1, "Tracklist Integrity");
      runAssert(lTl->tracks[0].isFavoriteTrack == true,
                "Track Favorite Persisted");
      delete lTl;
    } else {
      runAssert(false, "Load Tracklist Failed");
    }

    // --- FINAL CLEANUP ---
    log += "\n[Final Cleanup]\n";
    Storage.deleteItem("TEST_CD_RENAMED", MODE_CD);
    Storage.deleteItem("TEST_BOOK_RENAMED", MODE_BOOK);

    runAssert(!checkFileExists("/db/cds/TEST_CD_RENAMED.json"),
              "Cleanup CD file");
    runAssert(!checkFileExists("/db/books/TEST_BOOK_RENAMED.json"),
              "Cleanup Book file");

    log += "\n=== Global Success Check ===\n";
    log += "Passed: " + String(passed) + ", Failed: " + String(failed) + "\n";
    runAssert(failed == 0, "PERFECT PERSISTENCE TESTS PASSED");

    return log;
  }
};

#endif
