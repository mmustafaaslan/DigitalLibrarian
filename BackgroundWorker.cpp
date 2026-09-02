#include <Arduino.h>
#include <ESP_IOExpander_Library.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <queue>
#include <utility>

#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "ErrorHandler.h"
#include "MediaManager.h"
#include "NavigationCache.h"
#include "NetworkManager.h"
#include "RuntimeDiagnostics.h"
#include "Storage.h"
#include "Utils.h"
#include "mode_abstraction.h"
#include "waveshare_sd_card.h"

// Static members
std::queue<BackgroundJob> BackgroundWorker::_jobQueue;
SemaphoreHandle_t BackgroundWorker::_queueMutex = NULL;
TaskHandle_t BackgroundWorker::_taskHandle = NULL;
StackType_t *BackgroundWorker::_taskStackBuffer = nullptr;
StaticTask_t BackgroundWorker::_taskControlBlock;
bool BackgroundWorker::_busy = false;
bool BackgroundWorker::_showProgress = false;
JobType BackgroundWorker::_currentJobType = JOB_NONE;
String BackgroundWorker::_statusMsg = "Idle";
float BackgroundWorker::_progress = 0.0f;
std::atomic<bool> BackgroundWorker::_cancelRequested{false};
std::atomic<bool> BackgroundWorker::_skipRequested{false};
int BackgroundWorker::_totalJobs = 0;
JobType BackgroundWorker::_lastCompletedJobType = JOB_NONE;
bool BackgroundWorker::_lastJobSuccess = false;
uint32_t BackgroundWorker::_coverCompletionSequence = 0;
bool BackgroundWorker::_lastCoverSuccess = false;
String BackgroundWorker::_lastCoverMessage = "Not run";
String BackgroundWorker::_lastCoverItemId = "";
uint32_t BackgroundWorker::_webAddCompletionSequence = 0;
bool BackgroundWorker::_lastWebAddSuccess = false;
String BackgroundWorker::_lastWebAddMessage = "Not run";
String BackgroundWorker::_lastWebAddRequestCode = "";
bool BackgroundWorker::_favoriteFailureReady = false;
String BackgroundWorker::_favoriteFailureItemId = "";
bool BackgroundWorker::_favoriteFailureAttemptedValue = false;
ItemView BackgroundWorker::_metadataResult;
bool BackgroundWorker::_metadataResultReady = false;
ItemView BackgroundWorker::_itemSaveResult;
bool BackgroundWorker::_itemSaveCompletionReady = false;
bool BackgroundWorker::_itemSaveCompletionSuccess = false;
String BackgroundWorker::_itemSaveCompletionMessage = "";
MediaMode BackgroundWorker::_itemSaveResultMode = MODE_CD;
int BackgroundWorker::_itemSaveResultIndex = -1;
String BackgroundWorker::_lyricsResultTitle = "";
PsramString BackgroundWorker::_lyricsResultText;
bool BackgroundWorker::_lyricsResultReady = false;
bool BackgroundWorker::_lyricsCompletionReady = false;
bool BackgroundWorker::_lyricsCompletionSuccess = false;
String BackgroundWorker::_lyricsCompletionMessage = "";
TrackList *BackgroundWorker::_tracklistResult = nullptr;
bool BackgroundWorker::_tracklistCompletionReady = false;
bool BackgroundWorker::_tracklistCompletionSuccess = false;
String BackgroundWorker::_tracklistCompletionMessage = "";
int BackgroundWorker::_tracklistResultIndex = -1;
String BackgroundWorker::_tracklistResultMbid = "";

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

  // This board is an ESP32-S3 dual-core target. Keep network/SD/JSON work on
  // core 0 so TLS handshakes and provider timeouts cannot starve LVGL/touch on
  // core 1. The installed MbedTLS build must allocate its 16 KB record buffers
  // from internal RAM, whereas ESP-IDF explicitly permits task stacks in
  // external RAM on this target. Moving the unchanged 32 KB worker stack to
  // PSRAM restores a contiguous internal block for TLS without reducing stack
  // safety or changing the board package.
  static constexpr uint32_t WORKER_STACK_BYTES = 32768;
  _taskStackBuffer = static_cast<StackType_t *>(heap_caps_calloc(
      WORKER_STACK_BYTES, sizeof(StackType_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_taskStackBuffer) {
    _taskHandle = xTaskCreateStaticPinnedToCore(
        workerTask, "BG_Worker", WORKER_STACK_BYTES, NULL, 1,
        _taskStackBuffer, &_taskControlBlock, 0);
  }
  if (!_taskHandle) {
    if (_taskStackBuffer) {
      heap_caps_free(_taskStackBuffer);
      _taskStackBuffer = nullptr;
    }
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
  const bool interactiveJob =
      job.type == JOB_TRACKLIST_LOAD || job.type == JOB_LYRICS_LOAD_CACHED ||
      job.type == JOB_LYRICS_FETCH_ONE || job.type == JOB_METADATA_LOOKUP ||
      job.type == JOB_ITEM_SAVE || job.type == JOB_COVER_DOWNLOAD ||
      job.type == JOB_WEB_METADATA_ADD || job.type == JOB_COVER_DELETE;
  if (interactiveJob && !_jobQueue.empty()) {
    // Touch-driven work should not wait behind maintenance tasks such as track
    // summaries, favorites, or WLED persistence.
    std::queue<BackgroundJob> prioritized;
    prioritized.push(job);
    while (!_jobQueue.empty()) {
      prioritized.push(_jobQueue.front());
      _jobQueue.pop();
    }
    _jobQueue.swap(prioritized);
  } else {
    _jobQueue.push(job);
  }
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
JobType BackgroundWorker::getCurrentJobType() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return JOB_NONE;
  const JobType value = _currentJobType;
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
uint32_t BackgroundWorker::getStackHighWaterMark() {
  return _taskHandle ? (uint32_t)uxTaskGetStackHighWaterMark(_taskHandle) : 0;
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

void BackgroundWorker::reportProgress(float progress) {
  setProgress(progress);
}

void BackgroundWorker::reportStatus(const String &message) {
  setStatus(message);
}

void BackgroundWorker::requestCancel() {
  _cancelRequested.store(true, std::memory_order_relaxed);
  setStatus("Cancelling at the next safe checkpoint...");
}

bool BackgroundWorker::isCancellationRequested() {
  return _cancelRequested.load(std::memory_order_relaxed);
}

void BackgroundWorker::requestSkipCurrent() {
  _skipRequested.store(true, std::memory_order_relaxed);
  setStatus("Skipping the current record...");
}

bool BackgroundWorker::isSkipRequested() {
  return _skipRequested.load(std::memory_order_relaxed);
}

JobType BackgroundWorker::getLastCompletedJobType() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return JOB_NONE;
  const JobType value = _lastCompletedJobType;
  xSemaphoreGive(_queueMutex);
  return value;
}

bool BackgroundWorker::wasLastJobSuccessful() {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool value = _lastJobSuccess;
  xSemaphoreGive(_queueMutex);
  return value;
}

void BackgroundWorker::getLastCoverCompletion(uint32_t &sequence,
                                               bool &success,
                                               String &message,
                                               String *itemId) {
  if (!_queueMutex ||
      xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    sequence = 0;
    success = false;
    message = "Busy";
    if (itemId)
      *itemId = "";
    return;
  }
  sequence = _coverCompletionSequence;
  success = _lastCoverSuccess;
  message = _lastCoverMessage;
  if (itemId)
    *itemId = _lastCoverItemId;
  xSemaphoreGive(_queueMutex);
}

void BackgroundWorker::getLastWebAddCompletion(uint32_t &sequence,
                                               bool &success,
                                               String &message,
                                               String *requestCode) {
  if (!_queueMutex ||
      xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    sequence = 0;
    success = false;
    message = "Busy";
    if (requestCode)
      *requestCode = "";
    return;
  }
  sequence = _webAddCompletionSequence;
  success = _lastWebAddSuccess;
  message = _lastWebAddMessage;
  if (requestCode)
    *requestCode = _lastWebAddRequestCode;
  xSemaphoreGive(_queueMutex);
}

bool BackgroundWorker::takeFavoritePersistenceFailure(String &itemId,
                                                       bool &attemptedValue) {
  if (!_queueMutex ||
      xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _favoriteFailureReady;
  if (ready) {
    itemId = _favoriteFailureItemId;
    attemptedValue = _favoriteFailureAttemptedValue;
    _favoriteFailureReady = false;
  }
  xSemaphoreGive(_queueMutex);
  return ready;
}

bool BackgroundWorker::takeMetadataResult(ItemView &result) {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _metadataResultReady;
  if (ready) {
    result = _metadataResult;
    _metadataResultReady = false;
  }
  xSemaphoreGive(_queueMutex);
  return ready;
}

bool BackgroundWorker::takeItemSaveCompletion(bool &success, String &message,
                                              ItemView &savedItem,
                                              MediaMode &mode,
                                              int &editIndex) {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _itemSaveCompletionReady;
  if (ready) {
    success = _itemSaveCompletionSuccess;
    message = _itemSaveCompletionMessage;
    savedItem = _itemSaveResult;
    mode = _itemSaveResultMode;
    editIndex = _itemSaveResultIndex;
    _itemSaveCompletionReady = false;
    _itemSaveCompletionMessage = "";
  }
  xSemaphoreGive(_queueMutex);
  return ready;
}

bool BackgroundWorker::takeLyricsResult(String &trackTitle,
                                        PsramString &lyricsText) {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _lyricsResultReady;
  if (ready) {
    trackTitle = std::move(_lyricsResultTitle);
    lyricsText = std::move(_lyricsResultText);
    _lyricsResultReady = false;
    _lyricsResultTitle = "";
    _lyricsResultText.clear();
  }
  xSemaphoreGive(_queueMutex);
  return ready;
}

bool BackgroundWorker::takeLyricsCompletion(bool &success, String &message,
                                            String &trackTitle,
                                            PsramString &lyricsText) {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _lyricsCompletionReady;
  if (ready) {
    success = _lyricsCompletionSuccess;
    message = _lyricsCompletionMessage;
    if (success && _lyricsResultReady) {
      trackTitle = std::move(_lyricsResultTitle);
      lyricsText = std::move(_lyricsResultText);
    }
    _lyricsCompletionReady = false;
    _lyricsResultReady = false;
    _lyricsResultTitle = "";
    _lyricsResultText.clear();
    _lyricsCompletionMessage = "";
  }
  xSemaphoreGive(_queueMutex);
  return ready;
}

bool BackgroundWorker::takeTracklistCompletion(bool &success, String &message,
                                               int &itemIndex,
                                               String &releaseMbid,
                                               TrackList *&trackList) {
  if (!_queueMutex || xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  const bool ready = _tracklistCompletionReady;
  if (ready) {
    success = _tracklistCompletionSuccess;
    message = _tracklistCompletionMessage;
    itemIndex = _tracklistResultIndex;
    releaseMbid = _tracklistResultMbid;
    trackList = _tracklistResult;
    _tracklistResult = nullptr; // Ownership transfers to the UI.
    _tracklistCompletionReady = false;
    _tracklistCompletionMessage = "";
    _tracklistResultIndex = -1;
    _tracklistResultMbid = "";
  }
  xSemaphoreGive(_queueMutex);
  return ready;
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
        _cancelRequested.store(false, std::memory_order_relaxed);
        _skipRequested.store(false, std::memory_order_relaxed);
        _busy = true;
        _showProgress = currentJob.showProgress;
        _currentJobType = currentJob.type;
        _progress = 0.0f;
      } else {
        _busy = false;
        _showProgress = false;
        _currentJobType = JOB_NONE;
      }
      xSemaphoreGive(_queueMutex);
    }

    if (hasJob) {
      bool success = false;
      String resultMsg = "";
      bool restartAfterCompletion = false;

      switch (currentJob.type) {
      case JOB_METADATA_LOOKUP: {
        if (WiFi.status() != WL_CONNECTED) {
          resultMsg = "No WiFi connection. Connect and try again.";
          break;
        }
        setStatus("Looking up " + currentJob.id);
        setProgress(0.15f);
        ItemView staged;
        // Seed an edit lookup from the existing record so a changed barcode
        // cannot accidentally replace its unique ID, cover, LEDs, notes, or
        // known MusicBrainz release ID. This SD read remains on the worker.
        if (currentJob.index >= 0 && currentJob.index < getItemCount())
          staged = getItemAtSD(currentJob.index);
        success = fetchModeMetadata(currentJob.id, staged);
        if (isCancellationRequested()) {
          success = false;
          resultMsg = "Cancelled";
          break;
        }
        if (success) {
          if (_queueMutex &&
              xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            _metadataResult = staged;
            _metadataResultReady = true;
            xSemaphoreGive(_queueMutex);
          }
          setProgress(1.0f);
          resultMsg = "Metadata found for " + staged.title;
        } else {
          resultMsg = "No metadata was found for that code";
        }
      } break;

      case JOB_WEB_METADATA_ADD: {
        if (WiFi.status() != WL_CONNECTED) {
          resultMsg = "No WiFi connection. Connect and try again.";
          break;
        }
        setStatus("Looking up " + currentJob.id + "...");
        setProgress(0.10f);
        ItemView staged{};
        success = fetchModeMetadata(currentJob.id, staged);
        if (!success || isCancellationRequested()) {
          success = false;
          resultMsg = isCancellationRequested()
                          ? "Cancelled"
                          : "No metadata was found for " + currentJob.id;
          break;
        }

        // Barcode providers commonly use the barcode as the initial ID. A
        // forced duplicate must get its own detail file rather than silently
        // overwriting or sharing the first copy's cover.
        if (findItemIndex(staged.uniqueID) >= 0) {
          staged.uniqueID += "_" + String(millis()) + "_" +
                             String(esp_random() & 0xFFFF, HEX);
          staged.coverFile = "";
        }
        if (staged.uniqueID.length() == 0)
          staged.uniqueID = String(millis()) + "_" +
                            String(esp_random() & 0xFFFF, HEX);
        if (staged.ledIndices.empty())
          staged.ledIndices.push_back(getNextLedIndex());
        staged.detailsLoaded = true;
        staged.isValid = true;

        setStatus("Saving " + staged.title + "...");
        setProgress(0.85f);
        const MediaMode savedMode = currentMode;
        success = saveItemViewToStorage(staged, savedMode) &&
                  addItemToLibrary(staged);
        if (success) {
          invalidateNavigationCache();
          setProgress(1.0f);
          resultMsg = "Added " + staged.title;
        } else {
          resultMsg = "Metadata was found but could not be saved";
        }
      } break;

      case JOB_ITEM_SAVE: {
        ItemSavePayload *payload = currentJob.savePayload;
        if (!payload) {
          resultMsg = "The save request was incomplete";
          break;
        }

        setStatus(payload->mode == MODE_BOOK ? "Saving book to SD..."
                                             : "Saving CD to SD...");
        setProgress(0.25f);
        success = saveItemViewToStorage(
            payload->item, payload->mode,
            payload->oldUniqueID.length() > 0
                ? payload->oldUniqueID.c_str()
                : nullptr);
        setProgress(0.90f);

        if (_queueMutex &&
            xSemaphoreTake(_queueMutex, portMAX_DELAY) == pdTRUE) {
          _itemSaveResult = payload->item;
          _itemSaveResultMode = payload->mode;
          _itemSaveResultIndex = payload->editIndex;
          xSemaphoreGive(_queueMutex);
        }

        resultMsg = success ? "Saved and verified"
                            : "The SD save could not be verified";
        delete payload;
        currentJob.savePayload = nullptr;
      } break;

      case JOB_BULK_SYNC: {
        if (WiFi.status() != WL_CONNECTED) {
          resultMsg = "No WiFi connection. Cover sync was not started.";
          break;
        }
        is_sync_stopping = false;
        int total = getItemCount();
        int downloadedCount = 0;
        bool persistenceFailed = false;
        bool networkLost = false;
        bool indexDirty = false;

        for (int i = 0; i < total; i++) {
          if (is_sync_stopping || isCancellationRequested()) {
            Serial.println("BG_Worker: Sync stopping requested");
            break;
          }
          if (_skipRequested.exchange(false, std::memory_order_relaxed)) {
            Serial.printf("BG_Worker: Skipped sync item %d before work\n", i);
            continue;
          }
          if (WiFi.status() != WL_CONNECTED) {
            networkLost = true;
            Serial.println("BG_Worker: Sync stopped because WiFi disconnected");
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
          const String itemProgress = "Sync " + String(i + 1) + "/" +
                                      String(total) + ": ";
          setStatus(itemProgress + item.title);

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
                      if (!Storage.saveCD(cdLibrary[i], nullptr, true))
                        persistenceFailed = true;
                      else
                        indexDirty = true;
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      if (!Storage.saveBook(bookLibrary[i], nullptr, true))
                        persistenceFailed = true;
                      else
                        indexDirty = true;
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

          if (_skipRequested.exchange(false, std::memory_order_relaxed)) {
            Serial.printf("BG_Worker: Skipped sync item %d after local check\n",
                          i);
            continue;
          }

          // 3. Network & Persistence (NO Library Lock held during HTTP)
          if (missing) {
            String downloadUrl = item.coverUrl;
            const String archiveUrl = getCoverArtArchiveUrl(item);
            bool usingArchiveCandidate = false;
            Serial.printf("[SYNC] CoverUrl in DB: '%s' (length: %d)\n",
                          downloadUrl.c_str(), downloadUrl.length());

            // Prefer the record's exact MusicBrainz release before a fuzzy
            // artist/title search. Do not persist this candidate until its
            // image has actually downloaded successfully.
            if (downloadUrl.length() == 0 &&
                !isCancellationRequested() && !isSkipRequested()) {
              downloadUrl = archiveUrl;
              usingArchiveCandidate = downloadUrl.length() > 0;
            }

            bool coverDownloaded = false;
            if (downloadUrl.length() > 0 && !isCancellationRequested() &&
                !isSkipRequested() && WiFi.status() == WL_CONNECTED) {
              setStatus(itemProgress + "Downloading " + item.title);
              coverDownloaded = AppNetworkManager::downloadCoverImage(
                  downloadUrl, savePath, true);
            }

            // A release can legitimately have no archived front image. Keep
            // the existing short iTunes lookup as a bounded fallback.
            if (!coverDownloaded && item.coverUrl.length() == 0 &&
                !isCancellationRequested() && !isSkipRequested() &&
                WiFi.status() == WL_CONNECTED) {
              setStatus(itemProgress + "Finding cover for " + item.title);
              String providerUrl =
                  fetchCoverUrlForIndex(i, true); // Internal locking
              if (providerUrl.length() > 0 && providerUrl != downloadUrl) {
                downloadUrl = providerUrl;
                usingArchiveCandidate = false;
                setStatus(itemProgress + "Downloading " + item.title);
                coverDownloaded = AppNetworkManager::downloadCoverImage(
                    downloadUrl, savePath, true);
              }
            }

            if (coverDownloaded) {
              setItemCoverUrl(i, downloadUrl);
              if (usingArchiveCandidate)
                Serial.printf("[SYNC] Cover found by MusicBrainz release ID\n");
              if (!isCancellationRequested() && !isSkipRequested()) {
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
                      if (!Storage.saveCD(cdLibrary[i], nullptr, true))
                        persistenceFailed = true;
                      else
                        indexDirty = true;
                    break;
                  case MODE_BOOK:
                    if (i < (int)bookLibrary.size())
                      if (!Storage.saveBook(bookLibrary[i], nullptr, true))
                        persistenceFailed = true;
                      else
                        indexDirty = true;
                    break;
                  default:
                    break;
                  }
                  xSemaphoreGiveRecursive(libraryMutex);
                }
              }
            }
          }

          if (_skipRequested.exchange(false, std::memory_order_relaxed))
            Serial.printf("BG_Worker: Skipped sync item %d\n", i);

          delay(10); // Yield to other operations
        }

        if (indexDirty && !Storage.rewriteIndex(currentMode))
          persistenceFailed = true;
        if (indexDirty)
          invalidateNavigationCache();

        const bool cancelled =
            is_sync_stopping || isCancellationRequested();
        success = !cancelled && !networkLost && !persistenceFailed;
        setProgress(1.0f);
        resultMsg =
            success
                ? "Sync Complete"
                : (cancelled
                       ? "Cancelled"
                       : (networkLost ? "Sync stopped: WiFi disconnected"
                                      : "Sync save failed"));
      } break;

      case JOB_COVER_DOWNLOAD: {
        if (WiFi.status() != WL_CONNECTED) {
          resultMsg = "No WiFi connection. Connect and try again.";
          break;
        }
        int itemIndex = currentJob.index;
        ItemView item = getItemAtSD(itemIndex);
        if (!item.isValid ||
            (currentJob.id.length() > 0 && item.uniqueID != currentJob.id)) {
          itemIndex = findItemIndex(currentJob.id);
          item = getItemAtSD(itemIndex);
        }
        if (!item.isValid) {
          resultMsg = "This item is no longer available";
          break;
        }
        const bool explicitUrl = currentJob.extraData.length() > 0;

        // Re-running Cover Search for an item that already has a valid local
        // JPEG needlessly repeats several TLS handshakes and rewrites the same
        // SD file. The cover can still be replaced deliberately by tapping it,
        // deleting it, and searching again.
        bool localCoverReady = false;
        if (!explicitUrl && item.coverFile.length() > 4 && sdExpander &&
            i2cMutex &&
            xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
          sdExpander->digitalWrite(SD_CS, LOW);
          File localCover = SD.open("/covers/" + item.coverFile, FILE_READ);
          if (localCover && localCover.size() >= 4) {
            const int first = localCover.read();
            const int second = localCover.read();
            localCoverReady = first == 0xFF && second == 0xD8;
          }
          if (localCover)
            localCover.close();
          sdExpander->digitalWrite(SD_CS, HIGH);
          xSemaphoreGiveRecursive(i2cMutex);
        }
        if (localCoverReady) {
          success = true;
          setProgress(1.0f);
          resultMsg = "Cover is already available";
          break;
        }

        String uid = item.uniqueID;
        if (uid.length() == 0) {
          uid = String(millis()) + "_" + String(random(9999));
          setItemID(itemIndex, uid);
        }
        const String fileName =
            getUidPrefix() + sanitizeFilename(uid) + ".jpg";
        const String savePath = "/covers/" + fileName;

        // Bulk sync and manual Cover Search must agree. Sync can download a
        // persisted provider URL, so try that same URL before asking the
        // provider to discover the album again.
        String url = item.coverUrl;
        if (explicitUrl)
          url = currentJob.extraData;
        bool downloaded = false;
        bool replacementFound = false;
        bool lookupConnectionFailed = false;
        String lookupErrorDetail;
        String downloadErrorDetail;
        const bool hadSavedUrl = url.length() > 0;
        if (hadSavedUrl) {
          setStatus("Trying the saved cover for " + item.title + "...");
          setProgress(0.20f);
          downloaded = AppNetworkManager::downloadCoverImage(
              url, savePath, false, &downloadErrorDetail, !explicitUrl);
        }

        // CD records already carry an exact MusicBrainz release ID. The Cover
        // Art Archive endpoint is both more precise and more consistent with
        // metadata sync than a fuzzy Apple text search.
        const String archiveUrl = getCoverArtArchiveUrl(item);
        if (!downloaded && archiveUrl.length() > 0 && archiveUrl != url &&
            !isCancellationRequested()) {
          setStatus("Checking the release cover archive...");
          setProgress(0.40f);
          if (AppNetworkManager::downloadCoverImage(
                  archiveUrl, savePath, false, &downloadErrorDetail)) {
            url = archiveUrl;
            replacementFound = true;
            downloaded = true;
          }
        }

        if (!downloaded && !isCancellationRequested()) {
          setStatus(hadSavedUrl ? "Searching for a replacement cover..."
                                : "Finding a cover for " + item.title + "...");
          setProgress(0.45f);
          String discoveredUrl = fetchCoverUrlForIndex(
              itemIndex, false, &lookupConnectionFailed,
              &lookupErrorDetail);
          if (discoveredUrl.length() > 0) {
            replacementFound = true;
            url = discoveredUrl;
            setStatus("Downloading the cover image...");
            setProgress(0.65f);
            downloaded = AppNetworkManager::downloadCoverImage(
                url, savePath, false, &downloadErrorDetail);
          }
        }

        if (!downloaded) {
          if (hadSavedUrl || replacementFound) {
            ErrorHandler::logError(ERR_CAT_NETWORK,
                                   String("Cover download failed: ") + savePath,
                                   "BackgroundWorker::JOB_COVER_DOWNLOAD");
          }
          if (hadSavedUrl && !replacementFound) {
            resultMsg =
                "The saved cover was unavailable and no replacement was found";
          } else if (lookupConnectionFailed) {
            resultMsg = "Cover service connection failed";
            if (lookupErrorDetail.length() > 0)
              resultMsg += ": " + lookupErrorDetail;
          } else if (!hadSavedUrl && !replacementFound) {
            resultMsg = "No cover was found online";
          } else {
            resultMsg =
                "A cover was found, but it could not be downloaded or saved";
            if (downloadErrorDetail.length() > 0)
              resultMsg += ": " + downloadErrorDetail;
          }
          break;
        }

        setStatus("Saving the cover...");
        setProgress(0.90f);
        ItemView updated = getItemAtSD(itemIndex);
        if (!updated.isValid)
          updated = item;
        updated.coverUrl = url;
        updated.coverFile = fileName;
        updated.detailsLoaded = true;
        success = saveItemViewToStorage(updated, currentMode);
        if (success) {
          setItem(itemIndex, updated);
          invalidateNavigationCache();
        }
        setProgress(1.0f);
        resultMsg = success ? "Cover downloaded and saved"
                            : "Cover downloaded, but could not be fully saved";
      } break;

      case JOB_COVER_DELETE: {
        int itemIndex = currentJob.index;
        ItemView original = getItemAtSD(itemIndex);
        if (!original.isValid || original.uniqueID != currentJob.id) {
          itemIndex = findItemIndex(currentJob.id);
          original = getItemAtSD(itemIndex);
        }
        if (!original.isValid) {
          resultMsg = "This item is no longer available";
          break;
        }

        setStatus("Updating the cover record...");
        setProgress(0.25f);
        ItemView updated = original;
        updated.coverFile = "";
        updated.coverUrl = "";
        const MediaMode savedMode = currentMode;
        success = saveItemViewToStorage(updated, savedMode,
                                        original.uniqueID.c_str());
        if (!success) {
          resultMsg = "The cover was kept because its record could not be saved";
          break;
        }

        setItem(itemIndex, updated);
        invalidateNavigationCache();
        setProgress(0.75f);

        // Delete the JPEG only after the metadata transaction is durable. A
        // failed deletion leaves an unreferenced file, not a broken record.
        bool imageRemoved = true;
        const String safeCoverName = sanitizeFilename(original.coverFile);
        if (safeCoverName.length() > 0) {
          imageRemoved = false;
          if (i2cMutex &&
              xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) ==
                  pdPASS) {
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, LOW);
            const String path = "/covers/" + safeCoverName;
            imageRemoved = !SD.exists(path) || SD.remove(path);
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, HIGH);
            xSemaphoreGiveRecursive(i2cMutex);
          }
        }
        if (!imageRemoved)
          ErrorHandler::logWarn(ERR_CAT_STORAGE,
                                "Cover metadata cleared; orphan JPEG remains",
                                "BackgroundWorker::JOB_COVER_DELETE");
        setProgress(1.0f);
        resultMsg = imageRemoved ? "Cover deleted"
                                 : "Cover removed from the record";
      } break;

      case JOB_BACKUP_IMPORT: {
        setStatus("Importing backup...");
        setProgress(0.10f);
        int itemCount = 0;
        int tracklistCount = 0;
        success = Storage.importBackup(currentJob.id.c_str(), itemCount,
                                       tracklistCount);
        setProgress(1.0f);
        if (success) {
          invalidateNavigationCache();
          resultMsg = "Imported " + String(itemCount) + " items and " +
                      String(tracklistCount) + " tracklists";
          restartAfterCompletion = true;
        } else {
          resultMsg = "Backup import failed validation or storage verification";
        }
      } break;

      case JOB_TRACKLIST_LOAD: {
        setStatus("Opening songs...");
        setProgress(0.15f);

        String releaseMbid = currentJob.id;
        if (releaseMbid.length() == 0 && currentJob.extraData.length() > 0) {
          // Older/index-only records may not have their detail fields in RAM.
          // Load that detail on the worker as well; never make the LVGL/touch
          // task wait for SD access.
          CD detail;
          if (Storage.loadCDDetail(currentJob.extraData, detail))
            releaseMbid = detail.releaseMbid.c_str();
        }

        TrackList *loaded = nullptr;
        if (releaseMbid.length() == 0) {
          resultMsg = "This CD has no saved MusicBrainz track-list data.";
        } else {
          setProgress(0.40f);
          loaded = Storage.loadTracklist(releaseMbid.c_str());
          if (!loaded) {
            resultMsg =
                "No readable track list is saved for this CD. Edit it and "
                "use FETCH to retrieve the metadata again.";
          } else if (loaded->tracks.empty()) {
            Storage.deleteTracklist(loaded);
            loaded = nullptr;
            resultMsg = "The saved track list is empty.";
          }
        }

        if (_queueMutex &&
            xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (_tracklistResult)
            Storage.deleteTracklist(_tracklistResult);
          _tracklistResult = loaded;
          _tracklistResultIndex = currentJob.index;
          _tracklistResultMbid = releaseMbid;
          loaded = nullptr;
          xSemaphoreGive(_queueMutex);
          success = _tracklistResult != nullptr;
        }
        if (loaded)
          Storage.deleteTracklist(loaded);

        if (success) {
          setProgress(1.0f);
          resultMsg = "Songs ready";
        } else if (resultMsg.length() == 0) {
          resultMsg = "The track list could not be prepared.";
        }
      } break;

      case JOB_LYRICS_LOAD_CACHED: {
        setStatus("Opening cached lyrics...");
        setProgress(0.35f);
        PsramString lyricsText;
        if (!Storage.loadLyrics(currentJob.id.c_str(), lyricsText)) {
          resultMsg = "The cached lyrics file could not be read";
          break;
        }
        if (_queueMutex &&
            xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          _lyricsResultTitle = currentJob.extraData;
          _lyricsResultText = std::move(lyricsText);
          _lyricsResultReady = true;
          xSemaphoreGive(_queueMutex);
        }
        setProgress(1.0f);
        success = true;
        resultMsg = "Lyrics ready";
      } break;

      case JOB_LYRICS_FETCH_ONE: {
        String trackTitle = "";
        String lyricsPath = "";
        TrackList *trackList = Storage.loadTracklist(currentJob.id.c_str());
        if (!trackList || currentJob.index < 0 ||
            currentJob.index >= (int)trackList->tracks.size()) {
          if (trackList)
            Storage.deleteTracklist(trackList);
          resultMsg = "The selected track could not be loaded";
          break;
        }

        trackTitle = trackList->tracks[currentJob.index].title.c_str();
        const int trackCount = (int)trackList->tracks.size();
        const bool alreadyCached =
            trackList->tracks[currentJob.index].lyrics.status == "cached";
        if (alreadyCached)
          lyricsPath = trackList->tracks[currentJob.index].lyrics.path.c_str();
        Storage.deleteTracklist(trackList);

        if (alreadyCached) {
          setStatus("Loading cached lyrics...");
          setProgress(0.60f);
        } else {
          if (WiFi.status() != WL_CONNECTED) {
            resultMsg = "No WiFi connection. Connect and try again.";
            break;
          }
          setStatus("Downloading lyrics for " + trackTitle + "...");
          setProgress(0.20f);
          RuntimeDiagnostics::beginOperation(
              JOB_LYRICS_FETCH_ONE, currentJob.index + 1,
              trackCount, trackTitle.c_str());
          const LyricsResult result = fetchLyricsIfNeeded(
              currentJob.id.c_str(), currentJob.index, false);
          RuntimeDiagnostics::clearOperation();
          if (result != LYRICS_FETCHED_NOW &&
              result != LYRICS_ALREADY_CACHED) {
            resultMsg = "Lyrics were not found for this track";
            break;
          }

          trackList = Storage.loadTracklist(currentJob.id.c_str());
          if (trackList && currentJob.index >= 0 &&
              currentJob.index < (int)trackList->tracks.size()) {
            lyricsPath =
                trackList->tracks[currentJob.index].lyrics.path.c_str();
          }
          if (trackList)
            Storage.deleteTracklist(trackList);
          setProgress(0.80f);
        }

        PsramString lyricsText;
        if (!Storage.loadLyrics(lyricsPath.c_str(), lyricsText)) {
          resultMsg = "The lyrics file was empty or could not be read";
          break;
        }

        if (_queueMutex &&
            xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          _lyricsResultTitle = trackTitle;
          _lyricsResultText = std::move(lyricsText);
          _lyricsResultReady = true;
          xSemaphoreGive(_queueMutex);
        }
        setProgress(1.0f);
        success = true;
        resultMsg = "Lyrics ready";
      } break;

      case JOB_LYRICS_FETCH_ALL: {
        if (WiFi.status() != WL_CONNECTED) {
          resultMsg = "No WiFi connection. Lyrics download was not started.";
          break;
        }
        is_sync_stopping = false;
        String targetMbid = currentJob.id;
        if (targetMbid.length() > 0) {
          setStatus("Fetching lyrics for CD...");
          TrackList *tl = Storage.loadTracklist(targetMbid.c_str());
          if (tl) {
            int trackCount = (int)tl->tracks.size();
            // Do not retain one full list while fetchLyricsIfNeeded loads and
            // saves another copy for every track. This was a large avoidable
            // peak during TLS/JSON work on multi-disc releases.
            Storage.deleteTracklist(tl);
            tl = nullptr;
            int fetched = 0;
            bool stoppedForSafety = false;
            int stoppedAt = 0;
            for (int i = 0; i < trackCount; i++) {
              if (is_sync_stopping || isCancellationRequested())
                break;
              if (WiFi.status() != WL_CONNECTED) {
                stoppedForSafety = true;
                stoppedAt = i + 1;
                break;
              }
              setProgress(trackCount > 0 ? (float)i / trackCount : 0.0f);
              setStatus("Lyrics: track " + String(i + 1) + " of " +
                        String(trackCount));

              // This will check cache first, then hit APIs if missing
              RuntimeDiagnostics::beginOperation(
                  JOB_LYRICS_FETCH_ALL, i + 1, trackCount,
                  targetMbid.c_str());
              LyricsResult res =
                  fetchLyricsIfNeeded(targetMbid.c_str(), i, false);
              RuntimeDiagnostics::clearOperation();
              if (res == LYRICS_FETCHED_NOW || res == LYRICS_ALREADY_CACHED) {
                fetched++;
              } else if (res == LYRICS_ERROR) {
                stoppedForSafety = true;
                stoppedAt = i + 1;
                break;
              }
              // Give TLS cleanup and the UI task breathing room between
              // providers/tracks; repeated handshakes otherwise fragment the
              // small internal heap very quickly.
              delay(WiFi.RSSI() <= -75 ? 2500 : 750);
            }
            if (stoppedForSafety) {
              resultMsg = "Stopped safely at track " + String(stoppedAt) +
                          "/" + String(trackCount) +
                          ". Check WiFi or memory, then retry.";
              success = false;
            } else {
              resultMsg = "Fetched " + String(fetched) + "/" +
                          String(trackCount);
              success = true;
            }
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
            if (is_sync_stopping || isCancellationRequested())
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
                RuntimeDiagnostics::beginOperation(
                    JOB_LYRICS_FETCH_ALL, t + 1,
                    std::min(trackCount, 5), mbid.c_str());
                const LyricsResult result =
                    fetchLyricsIfNeeded(mbid.c_str(), t, false);
                RuntimeDiagnostics::clearOperation();
                if (result == LYRICS_ERROR ||
                    WiFi.status() != WL_CONNECTED) {
                  is_sync_stopping = true;
                  break;
                }
                delay(WiFi.RSSI() <= -75 ? 2500 : 750);
              }
            }
            if (is_sync_stopping)
              break;
          }
          resultMsg = is_sync_stopping
                          ? "Lyrics scan stopped safely. Check WiFi or memory."
                          : "Scan complete";
          success = !is_sync_stopping;
        }
      } break;

      case JOB_PERSIST_FAVORITE: {
        const bool isBook = currentJob.extraData.startsWith("book:");
        const bool favorite = currentJob.extraData.endsWith(":1");
        setStatus("Saving favorite...");

        MediaMode mode = isBook ? MODE_BOOK : MODE_CD;
        success = Storage.updateFavorite(currentJob.id, mode, favorite);
        if (!success && libraryMutex &&
            xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(1000)) ==
                pdPASS) {
          if (mode == MODE_CD) {
            for (CD &item : cdLibrary) {
              if (item.uniqueID == currentJob.id.c_str() &&
                  item.favorite == favorite) {
                item.favorite = !favorite;
                break;
              }
            }
          } else {
            for (Book &item : bookLibrary) {
              if (item.uniqueID == currentJob.id.c_str() &&
                  item.favorite == favorite) {
                item.favorite = !favorite;
                break;
              }
            }
          }
          xSemaphoreGiveRecursive(libraryMutex);
        }
        if (!success && _queueMutex &&
            xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          _favoriteFailureReady = true;
          _favoriteFailureItemId = currentJob.id;
          _favoriteFailureAttemptedValue = favorite;
          xSemaphoreGive(_queueMutex);
        }
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

      case JOB_DIAGNOSTIC_LYRICS_STRESS:
        setStatus("Preparing lyrics TLS stress test...");
        success = MediaManager::stressTestLyricsTransport(
            currentJob.index > 0 ? currentJob.index : 5, resultMsg);
        RuntimeDiagnostics::clearOperation();
        break;

      default:
        break;
      }

      if (isCancellationRequested() &&
          (currentJob.type == JOB_METADATA_LOOKUP ||
           currentJob.type == JOB_LYRICS_FETCH_ONE ||
           currentJob.type == JOB_LYRICS_FETCH_ALL ||
           currentJob.type == JOB_WEB_METADATA_ADD ||
           currentJob.type == JOB_COVER_DOWNLOAD ||
           currentJob.type == JOB_BULK_SYNC ||
           currentJob.type == JOB_DIAGNOSTIC_LYRICS_STRESS)) {
        success = false;
        resultMsg = "Cancelled";
      }

      if (resultMsg.length() > 0)
        setStatus(resultMsg);

      if (_queueMutex &&
          xSemaphoreTake(_queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _lastCompletedJobType = currentJob.type;
        _lastJobSuccess = success;
        if (currentJob.type == JOB_COVER_DOWNLOAD) {
          _coverCompletionSequence++;
          _lastCoverSuccess = success;
          _lastCoverMessage = resultMsg;
          _lastCoverItemId = currentJob.id;
        }
        if (currentJob.type == JOB_WEB_METADATA_ADD) {
          _webAddCompletionSequence++;
          _lastWebAddSuccess = success;
          _lastWebAddMessage = resultMsg;
          _lastWebAddRequestCode = currentJob.id;
        }
        if (currentJob.type == JOB_LYRICS_LOAD_CACHED ||
            currentJob.type == JOB_LYRICS_FETCH_ONE) {
          _lyricsCompletionReady = true;
          _lyricsCompletionSuccess = success;
          _lyricsCompletionMessage = resultMsg;
        } else if (currentJob.type == JOB_TRACKLIST_LOAD) {
          _tracklistCompletionReady = true;
          _tracklistCompletionSuccess = success;
          _tracklistCompletionMessage = resultMsg;
        } else if (currentJob.type == JOB_ITEM_SAVE) {
          _itemSaveCompletionReady = true;
          _itemSaveCompletionSuccess = success;
          _itemSaveCompletionMessage = resultMsg;
        }
        xSemaphoreGive(_queueMutex);
      }

      if (currentJob.onComplete) {
        currentJob.onComplete(success, resultMsg);
      }
      _cancelRequested.store(false, std::memory_order_relaxed);
      _skipRequested.store(false, std::memory_order_relaxed);
      if (restartAfterCompletion) {
        setStatus(resultMsg + ". Restarting...");
        delay(1500);
        ESP.restart();
      }
    } else {
      delay(100); // Wait longer when idle to reduce bus load
    }
    delay(10);
  }
}
