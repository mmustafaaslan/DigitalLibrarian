#ifndef BACKGROUND_WORKER_H
#define BACKGROUND_WORKER_H

#include <Arduino.h>
#include "Core_Data.h"
#include <atomic>
#include <functional>
#include <queue>


enum JobType {
  JOB_NONE,
  JOB_METADATA_LOOKUP,
  JOB_ITEM_SAVE,
  JOB_COVER_DOWNLOAD,
  JOB_BULK_SYNC,
  JOB_TRACKLIST_LOAD,
  JOB_LYRICS_LOAD_CACHED,
  JOB_LYRICS_FETCH_ONE,
  JOB_LYRICS_FETCH_ALL,
  JOB_PERSIST_FAVORITE,
  JOB_PERSIST_TRACK_FAVORITE,
  JOB_TRACK_SUMMARY_LOAD,
  JOB_SYNC_WLED
};

struct ItemSavePayload {
  ItemView item;
  MediaMode mode = MODE_CD;
  int editIndex = -1;
  String oldUniqueID;
};

struct BackgroundJob {
  JobType type;
  String id;        // Barcode, ISBN, or URL
  int index;        // Index in the library if applicable
  String extraData; // Search query, save path, etc.
  std::function<void(bool success, String message)> onComplete;
  bool showProgress = true;
  ItemSavePayload *savePayload = nullptr;
};

class BackgroundWorker {
public:
  static void begin();
  static bool addJob(const BackgroundJob &job);
  static bool isBusy();
  static bool shouldShowProgress();
  static JobType getCurrentJobType();
  static int getQueueSize();
  static uint32_t getStackHighWaterMark();

  // UI Helpers
  static String getStatusMessage();
  static float getProgress(); // 0.0 to 1.0
  static void reportProgress(float progress);
  static void requestCancel();
  static bool isCancellationRequested();
  static void requestSkipCurrent();
  static bool isSkipRequested();
  static JobType getLastCompletedJobType();
  static bool wasLastJobSuccessful();
  static bool takeMetadataResult(ItemView &result);
  static bool takeItemSaveCompletion(bool &success, String &message,
                                     ItemView &savedItem, MediaMode &mode,
                                     int &editIndex);
  static bool takeLyricsResult(String &trackTitle, PsramString &lyricsText);
  static bool takeLyricsCompletion(bool &success, String &message,
                                   String &trackTitle,
                                   PsramString &lyricsText);
  static bool takeTracklistCompletion(bool &success, String &message,
                                      int &itemIndex, String &releaseMbid,
                                      TrackList *&trackList);

private:
  static void workerTask(void *pvParameters);
  static std::queue<BackgroundJob> _jobQueue;
  static SemaphoreHandle_t _queueMutex;
  static TaskHandle_t _taskHandle;
  static StackType_t *_taskStackBuffer;
  static StaticTask_t _taskControlBlock;
  static bool _busy;
  static bool _showProgress;
  static JobType _currentJobType;
  static String _statusMsg;
  static float _progress;
  static std::atomic<bool> _cancelRequested;
  static std::atomic<bool> _skipRequested;
  static int _totalJobs;
  static JobType _lastCompletedJobType;
  static bool _lastJobSuccess;
  static ItemView _metadataResult;
  static bool _metadataResultReady;
  static ItemView _itemSaveResult;
  static bool _itemSaveCompletionReady;
  static bool _itemSaveCompletionSuccess;
  static String _itemSaveCompletionMessage;
  static MediaMode _itemSaveResultMode;
  static int _itemSaveResultIndex;
  static String _lyricsResultTitle;
  static PsramString _lyricsResultText;
  static bool _lyricsResultReady;
  static bool _lyricsCompletionReady;
  static bool _lyricsCompletionSuccess;
  static String _lyricsCompletionMessage;
  static TrackList *_tracklistResult;
  static bool _tracklistCompletionReady;
  static bool _tracklistCompletionSuccess;
  static String _tracklistCompletionMessage;
  static int _tracklistResultIndex;
  static String _tracklistResultMbid;
  static void setStatus(const String &message);
  static void setProgress(float progress);
  static void setBusyState(bool busy, bool showProgress);
};

#endif
