#ifndef BACKGROUND_WORKER_H
#define BACKGROUND_WORKER_H

#include <Arduino.h>
#include <functional>
#include <queue>


enum JobType {
  JOB_NONE,
  JOB_METADATA_LOOKUP,
  JOB_COVER_DOWNLOAD,
  JOB_BULK_SYNC,
  JOB_LYRICS_FETCH_ALL
};

struct BackgroundJob {
  JobType type;
  String id;        // Barcode, ISBN, or URL
  int index;        // Index in the library if applicable
  String extraData; // Search query, save path, etc.
  std::function<void(bool success, String message)> onComplete;
};

class BackgroundWorker {
public:
  static void begin();
  static void addJob(BackgroundJob job);
  static bool isBusy();
  static int getQueueSize();

  // UI Helpers
  static String getStatusMessage();
  static float getProgress(); // 0.0 to 1.0

private:
  static void workerTask(void *pvParameters);
  static std::queue<BackgroundJob> _jobQueue;
  static SemaphoreHandle_t _queueMutex;
  static bool _busy;
  static String _statusMsg;
  static float _progress;
  static int _totalJobs;
};

#endif
