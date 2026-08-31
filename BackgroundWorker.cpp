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
TaskHandle_t BackgroundWorker::_taskHandle = NULL;
bool BackgroundWorker::_busy = false;
bool BackgroundWorker::_showProgress = false;
String BackgroundWorker::_statusMsg = "Idle";
float BackgroundWorker::_progress = 0.0f;
int BackgroundWorker::_totalJobs = 0;

void BackgroundWorker::begin() {
  if (_taskHandle)
    return;

  if (_queueMutex == NULL) {
    _queueMutex = xSemaphoreCreateMutex();
  }
  if (!_queueMutex) {
    ErrorHandler::logError(ERR_CAT_SYSTEM,
                           "Could not allocate background queue mutex",
                           "BackgroundWorker::begin");
    return;
  }

  // Let FreeRTOS schedule the worker away from whichever core is currently
  // busiest. This prevents long network/JSON work from being permanently tied
  // to the LVGL loop core.
  const BaseType_t created =
      xTaskCreate(workerTask, "BG_Worker", 32768, NULL, 1, &_taskHandle);
  if (created != pdPASS) {
    _taskHandle = NULL;
    vSemaphoreDelete(_queueMutex);
    _queueMutex = NULL;
    ErrorHandler::logError(ERR_CAT_SYSTEM,
                           "Could not create background worker task",
                           "BackgroundWorker::begin");
  }
}

bool BackgroundWorker::addJob(const BackgroundJob &job) {
  if (!_queueMutex ||
      xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return false;

  std::queue<BackgroundJob> copy = _jobQueue;
  while (!copy.empty()) {
    const BackgroundJob &queued = copy.front();
    if (queued.type == job.type && queued.id == job.id &&
        queued.index == job.index) {
      xSemaphoreGive(_queueMutex);
      return true;
    }
    copy.pop();
  }
  if (_jobQueue.size() >= 16) {
    xSemaphoreGive(_queueMutex);
    ErrorHandler::logWarn(ERR_CAT_SYSTEM, "Background queue full",
                          "BackgroundWorker::addJob");
    return false;
  }
  _jobQueue.push(job);
  xSemaphoreGive(_queueMutex);
  return true;
}

bool BackgroundWorker::isBusy() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return true;
  const bool value = _busy;
  xSemaphoreGive(_queueMutex);
  return value;
}
bool BackgroundWorker::shouldShowProgress() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool value = _busy && _showProgress;
  xSemaphoreGive(_queueMutex);
  return value;
}
int BackgroundWorker::getQueueSize() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return 0;
  const int value = (int)_jobQueue.size();
  xSemaphoreGive(_queueMutex);
  return value;
}
String BackgroundWorker::getStatusMessage() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return "Working...";
  const String value = _statusMsg;
  xSemaphoreGive(_queueMutex);
  return value;
}
float BackgroundWorker::getProgress() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return 0.0f;
  const float value = _progress;
  xSemaphoreGive(_queueMutex);
  return value;
}

void BackgroundWorker::setStatus(const String &message) {
  if (_queueMutex && xSemaphoreTake(_queueMutex, portMAX_DELAY) == pdTRUE) {
    _statusMsg = message;
    xSemaphoreGive(_queueMutex);
  }
}

void BackgroundWorker::setProgress(float progress) {
  if (_queueMutex && xSemaphoreTake(_queueMutex, portMAX_DELAY) == pdTRUE) {
    _progress = constrain(progress, 0.0f, 1.0f);
    xSemaphoreGive(_queueMutex);
  }
}

void BackgroundWorker::setBusyState(bool busy, bool showProgress) {
  if (_queueMutex && xSemaphoreTake(_queueMutex, portMAX_DELAY) == pdTRUE) {
    _busy = busy;
    _showProgress = showProgress;
    xSemaphoreGive(_queueMutex);
  }
}

void BackgroundWorker::workerTask(void *pvParameters) {
  while (true) {
    BackgroundJob currentJob;
    bool hasJob = false;

    if (xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(50))) {
      if (!_jobQueue.empty()) {
        currentJob = _jobQueue.front();
        _jobQueue.pop();
        hasJob = true;
        _busy = true;
        _showProgress = currentJob.showProgress;
      } else {
        _busy = false;
        _showProgress = false;
      }
      xSemaphoreGive(_queueMutex);
    }

    if (hasJob) {
      bool success = false;
      String resultMsg = "";

      switch (currentJob.type) {
      case JOB_METADATA_LOOKUP: {
        setStatus("Looking up " + currentJob.id);
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
        bool persistenceFailed = false;

        for (int i = 0; i < total; i++) {
          if (is_sync_stopping) {
            Serial.println("BG_Worker: Sync stopping requested");
            break;
          }

          setProgress(total > 0 ? (float)i / total : 0.0f);

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
          setStatus("Sync: " + item.title);

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
                      if (!Storage.saveCD(cdLibrary[i]))
                        persistenceFailed = true;
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      if (!Storage.saveBook(bookLibrary[i]))
                        persistenceFailed = true;
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
                      if (!Storage.saveCD(cdLibrary[i]))
                        persistenceFailed = true;
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      if (!Storage.saveBook(bookLibrary[i]))
                        persistenceFailed = true;
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

        success = !is_sync_stopping && !persistenceFailed;
        setProgress(1.0f);
        setStatus(success ? "Sync Complete"
                          : (persistenceFailed ? "Sync save failed"
                                               : "Sync Stopped"));
      } break;

      case JOB_COVER_DOWNLOAD: {
        setStatus("Downloading cover...");
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

      case JOB_LYRICS_FETCH_ONE: {
        setStatus("Fetching lyrics...");
        LyricsResult result =
            fetchLyricsIfNeeded(currentJob.id.c_str(), currentJob.index, false);
        success = result == LYRICS_FETCHED_NOW ||
                  result == LYRICS_ALREADY_CACHED;
        resultMsg = success ? "Lyrics ready" : "Lyrics not found";
      } break;

      case JOB_LYRICS_FETCH_ALL: {
        String targetMbid = currentJob.id;
        if (targetMbid.length() > 0) {
          setStatus("Fetching lyrics for CD...");
          TrackList *tl = Storage.loadTracklist(targetMbid.c_str());
          if (tl) {
            int trackCount = (int)tl->tracks.size();
            int fetched = 0;
            for (int i = 0; i < trackCount; i++) {
              if (is_sync_stopping)
                break;
              setProgress(trackCount > 0 ? (float)i / trackCount : 0.0f);
              setStatus("Lyrics: " + String(tl->tracks[i].title.c_str()));

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
          setStatus("Lyrics: Full Scan");
          int cdCount = 0;
          if (libraryMutex)
            xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
          cdCount = (int)cdLibrary.size();
          if (libraryMutex)
            xSemaphoreGiveRecursive(libraryMutex);
          for (int i = 0; i < cdCount; i++) {
            if (is_sync_stopping)
              break;
            setProgress(cdCount > 0 ? (float)i / cdCount : 0.0f);
            String mbid;
            String title;
            int trackCount = 0;
            if (libraryMutex)
              xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
            if (i < (int)cdLibrary.size()) {
              mbid = cdLibrary[i].releaseMbid.c_str();
              title = cdLibrary[i].title.c_str();
              trackCount = cdLibrary[i].trackCount;
            }
            if (libraryMutex)
              xSemaphoreGiveRecursive(libraryMutex);
            if (mbid.length() > 0) {
              setStatus("Lyrics: " + title);
              // Just fetch first 5 tracks in full scan to avoid API ban
              for (int t = 0; t < std::min(trackCount, 5); t++) {
                fetchLyricsIfNeeded(mbid.c_str(), t, false);
                delay(100);
              }
            }
          }
          resultMsg = "Scan complete";
          success = true;
        }
      } break;

      case JOB_PERSIST_FAVORITE: {
        const bool isBook = currentJob.extraData.startsWith("book:");
        const bool favorite = currentJob.extraData.endsWith(":1");
        setStatus("Saving favorite...");

        MediaMode mode = isBook ? MODE_BOOK : MODE_CD;
        success = Storage.updateFavorite(currentJob.id, mode, favorite);
        resultMsg = success ? "Favorite saved" : "Favorite save failed";
      } break;

      case JOB_PERSIST_TRACK_FAVORITE: {
        setStatus("Saving track favorite...");
        TrackList *trackList = Storage.loadTracklist(currentJob.id.c_str());
        if (trackList && currentJob.index >= 0 &&
            currentJob.index < (int)trackList->tracks.size()) {
          trackList->tracks[currentJob.index].isFavoriteTrack =
              currentJob.extraData == "1";
          success = Storage.saveTracklist(currentJob.id.c_str(), trackList);
        }
        if (trackList)
          Storage.deleteTracklist(trackList);
        resultMsg = success ? "Track favorite saved"
                            : "Track favorite save failed";
      } break;

      case JOB_TRACK_SUMMARY_LOAD: {
        setStatus("Loading track favorites...");
        TrackList *trackList = Storage.loadTracklist(currentJob.id.c_str());
        int favoriteCount = 0;
        resultMsg = "Fav: ";
        if (trackList) {
          for (const Track &track : trackList->tracks) {
            if (!track.isFavoriteTrack)
              continue;
            if (favoriteCount > 0)
              resultMsg += " | ";
            resultMsg += String(track.trackNo) + ". " +
                         String(track.title.c_str());
            favoriteCount++;
          }
          Storage.deleteTracklist(trackList);
        }
        success = favoriteCount > 0;
      } break;

      case JOB_SYNC_WLED:
        setStatus("Syncing shelf lights...");
        AppNetworkManager::forceUpdateWLED();
        success = true;
        resultMsg = "Shelf lights synced";
        break;

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
