#include <Arduino.h>
#include <ESP_IOExpander_Library.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <queue>

#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "ErrorHandler.h"
#include "MediaManager.h"
#include "NetworkManager.h"
#include "Storage.h"
#include "Utils.h"
#include "mode_abstraction.h"
#include "waveshare_sd_card.h"

// Static members
std::queue<BackgroundJob> BackgroundWorker::_jobQueue;
SemaphoreHandle_t BackgroundWorker::_queueMutex = NULL;
bool BackgroundWorker::_busy = false;
String BackgroundWorker::_statusMsg = "Idle";
float BackgroundWorker::_progress = 0.0f;
int BackgroundWorker::_totalJobs = 0;

void BackgroundWorker::begin() {
  if (_queueMutex == NULL) {
    _queueMutex = xSemaphoreCreateMutex();
  }
  // Background task on Core 1 (Same as UI/Main Loop) to avoid Core 0
  // System/WiFi contention Increase stack to 32KB for heavy network/JSON
  // operations
  xTaskCreatePinnedToCore(workerTask, "BG_Worker", 32768, NULL, 1, NULL, 1);
}

void BackgroundWorker::addJob(BackgroundJob job) {
  if (xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100))) {
    _jobQueue.push(job);
    xSemaphoreGive(_queueMutex);
  }
}

bool BackgroundWorker::isBusy() { return _busy; }
int BackgroundWorker::getQueueSize() { return (int)_jobQueue.size(); }
String BackgroundWorker::getStatusMessage() { return _statusMsg; }
float BackgroundWorker::getProgress() { return _progress; }

void BackgroundWorker::workerTask(void *pvParameters) {
  while (true) {
    BackgroundJob currentJob;
    bool hasJob = false;

    if (xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(50))) {
      if (!_jobQueue.empty()) {
        currentJob = _jobQueue.front();
        _jobQueue.pop();
        hasJob = true;
        _totalJobs = getItemCount(); // Current total
        _busy = true;
      } else {
        _busy = false;
      }
      xSemaphoreGive(_queueMutex);
    }

    if (hasJob) {
      bool success = false;
      String resultMsg = "";

      switch (currentJob.type) {
      case JOB_METADATA_LOOKUP: {
        _statusMsg = "Looking up " + currentJob.id;
        ItemView staged;
        success = MediaManager::fetchMetadataForBarcode(currentJob.id.c_str(),
                                                        staged);
        if (success) {
          resultMsg = "Fetched: " + staged.title;
        }
      } break;

      case JOB_BULK_SYNC: {
        is_sync_stopping = false;
        int total = getItemCount();
        int downloadedCount = 0;

        for (int i = 0; i < total; i++) {
          if (is_sync_stopping) {
            Serial.println("BG_Worker: Sync stopping requested");
            break;
          }

          _progress = (float)i / total;

          // 1. Initial Data Fetch (Short Lock)
          ItemView item;
          if (libraryMutex) {
            if (xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(5000)) ==
                pdPASS) {
              ensureItemDetailsLoaded(i);
              item = getItemAtSD(i);

              // Ensure ID exists while locked
              if (item.isValid && item.uniqueID.length() == 0) {
                String newID =
                    (item.codecOrIsbn.length() > 0)
                        ? item.codecOrIsbn
                        : (String(millis()) + "_" + String(random(9999)));
                setItemID(i, newID);
                item.uniqueID = newID;
              }
              xSemaphoreGiveRecursive(libraryMutex);
            } else {
              continue;
            }
          }

          if (!item.isValid)
            continue;
          _statusMsg = "Sync: " + item.title;

          // 2. Hardware Check (I2C Lock only, NO Library Lock)
          bool missing = true;
          String savePath = "";
          String foundFileName = "";
          if (sdExpander && i2cMutex) {
            if (xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) ==
                pdPASS) {
              sdExpander->digitalWrite(SD_CS, LOW);

              if (item.coverFile.length() > 4 &&
                  SD.exists("/covers/" + item.coverFile)) {
                missing = false;
              } else {
                String prefix = getUidPrefix();
                String safeID = sanitizeFilename(item.uniqueID);
                savePath = "/covers/" + prefix + safeID + ".jpg";
                if (SD.exists(savePath)) {
                  missing = false;
                  foundFileName = prefix + safeID + ".jpg";
                }
              }
              sdExpander->digitalWrite(SD_CS, HIGH);
              xSemaphoreGiveRecursive(i2cMutex);

              // Move setter OUTSIDE i2c lock to prevent libraryMutex hierarchy
              // violation
              if (foundFileName.length() > 0) {
                setItemCoverFile(i, foundFileName);

                // PERSIST: If we found a missing path on disk, save it to the
                // detail file!
                if (libraryMutex &&
                    xSemaphoreTakeRecursive(libraryMutex,
                                            pdMS_TO_TICKS(1000)) == pdPASS) {
                  switch (currentMode) {
                  case MODE_CD:
                    if (i < (int)cdLibrary.size())
                      Storage.saveCD(cdLibrary[i], nullptr, true);
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      Storage.saveBook(bookLibrary[i], nullptr, true);
                    break;
                  default:
                    break;
                  }
                  xSemaphoreGiveRecursive(libraryMutex);
                }
              }
            }
          }

          Serial.printf(
              "[SYNC] Item %d: '%s' | CoverFile: '%s' | Missing: %s\n", i,
              item.title.c_str(), item.coverFile.c_str(),
              missing ? "YES" : "NO");

          // 3. Network & Persistence (NO Library Lock held during HTTP)
          if (missing) {
            String downloadUrl = item.coverUrl;
            Serial.printf("[SYNC] CoverUrl in DB: '%s' (length: %d)\n",
                          downloadUrl.c_str(), downloadUrl.length());

            // Always re-fetch if we're missing the file, even if we have a URL
            // This ensures we get fresh Google Books URLs instead of stale Open
            // Library ones
            Serial.printf(
                "[SYNC] Cover file missing, re-fetching URL for item %d...\n",
                i);
            downloadUrl = fetchCoverUrlForIndex(i); // Internal locking
            if (downloadUrl.length() > 0) {
              setItemCoverUrl(i, downloadUrl);
            } else {
              Serial.printf("[SYNC] Failed to fetch cover URL\n");
            }

            if (downloadUrl.length() > 0) {
              if (AppNetworkManager::downloadCoverImage(downloadUrl,
                                                        savePath)) {
                downloadedCount++;
                String prefix = getUidPrefix();
                String fileName =
                    prefix + sanitizeFilename(item.uniqueID) + ".jpg";
                setItemCoverFile(i, fileName);

                // Save to storage (Requires short lock for vector access)
                if (libraryMutex &&
                    xSemaphoreTakeRecursive(libraryMutex,
                                            pdMS_TO_TICKS(5000)) == pdPASS) {
                  switch (currentMode) {
                  case MODE_CD:
                    if (i < (int)cdLibrary.size())
                      Storage.saveCD(cdLibrary[i], nullptr, true); // Batch save
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      Storage.saveBook(bookLibrary[i], nullptr,
                                       true); // Batch save
                    break;
                  default:
                    break;
                  }
                  xSemaphoreGiveRecursive(libraryMutex);
                }
              }
            }
          }

          delay(10); // Yield to other operations
        }

        // Final index rewrite after batch sync completes
        Storage.rewriteIndex(currentMode);

        success = !is_sync_stopping;
        _progress = 1.0f;
        _statusMsg = success ? "Sync Complete" : "Sync Stopped";
      } break;

      case JOB_COVER_DOWNLOAD: {
        _statusMsg = "Downloading cover...";
        String savePath = currentJob.extraData;
        String url = currentJob.id;

        if (savePath.length() > 0 && url.length() > 0) {
          if (AppNetworkManager::downloadCoverImage(url, savePath)) {
            resultMsg = "Downloaded to " + savePath;
            success = true;
          } else {
            ErrorHandler::logError(ERR_CAT_NETWORK,
                                   String("Cover download failed: ") + savePath,
                                   "BackgroundWorker::JOB_COVER_DOWNLOAD");
            resultMsg = "Download Failed";
            success = false;
          }
        } else {
          resultMsg = "Invalid Params";
          success = false;
        }
      } break;

      case JOB_LYRICS_FETCH_ALL: {
        String targetMbid = currentJob.id;
        if (targetMbid.length() > 0) {
          _statusMsg = "Fetching lyrics for CD...";
          TrackList *tl = Storage.loadTracklist(targetMbid.c_str());
          if (tl) {
            int trackCount = (int)tl->tracks.size();
            int fetched = 0;
            for (int i = 0; i < trackCount; i++) {
              if (is_sync_stopping)
                break;
              _progress = (float)i / trackCount;
              _statusMsg = "Lyrics: " + String(tl->tracks[i].title.c_str());

              // This will check cache first, then hit APIs if missing
              LyricsResult res =
                  fetchLyricsIfNeeded(targetMbid.c_str(), i, false);
              if (res == LYRICS_FETCHED_NOW || res == LYRICS_ALREADY_CACHED) {
                fetched++;
              }
              delay(50); // Small gap between API requests
            }
            resultMsg = "Fetched " + String(fetched) + "/" + String(trackCount);
            success = true;
            delete tl;
          } else {
            resultMsg = "Tracklist missing";
            success = false;
          }
        } else {
          // If no specific CD, fetch for ALL items in library that have MBID
          _statusMsg = "Lyrics: Full Scan";
          int cdCount = (int)cdLibrary.size();
          for (int i = 0; i < cdCount; i++) {
            if (is_sync_stopping)
              break;
            _progress = (float)i / cdCount;
            CD &cd = cdLibrary[i];
            if (cd.releaseMbid.length() > 0) {
              _statusMsg = "Lyrics: " + String(cd.title.c_str());
              // Just fetch first 5 tracks in full scan to avoid API ban
              for (int t = 0; t < std::min((int)cd.trackCount, 5); t++) {
                fetchLyricsIfNeeded(cd.releaseMbid.c_str(), t, false);
                delay(100);
              }
            }
          }
          resultMsg = "Scan complete";
          success = true;
        }
      } break;

      default:
        break;
      }

      if (currentJob.onComplete) {
        currentJob.onComplete(success, resultMsg);
      }
    } else {
      delay(100); // Wait longer when idle to reduce bus load
    }
    delay(10);
  }
}
