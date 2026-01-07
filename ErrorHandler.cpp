#include "ErrorHandler.h"
#include "AppGlobals.h"
#include "waveshare_sd_card.h"
#include <SD.h>
#include <time.h>


// Static member initialization
std::vector<ErrorRecord> ErrorHandler::recentErrors;
bool ErrorHandler::sdLoggingEnabled = true;

void ErrorHandler::init() {
  recentErrors.clear();
  recentErrors.reserve(MAX_RECENT_ERRORS);
  logInfo(ERR_CAT_SYSTEM, "ErrorHandler initialized", "ErrorHandler::init");
}

String ErrorHandler::levelToString(ErrorLevel level) {
  switch (level) {
  case ERR_INFO:
    return "INFO";
  case ERR_WARN:
    return "WARN";
  case ERR_ERROR:
    return "ERROR";
  case ERR_FATAL:
    return "FATAL";
  default:
    return "UNKNOWN";
  }
}

String ErrorHandler::categoryToString(ErrorCategory category) {
  switch (category) {
  case ERR_CAT_NETWORK:
    return "NETWORK";
  case ERR_CAT_STORAGE:
    return "STORAGE";
  case ERR_CAT_API:
    return "API";
  case ERR_CAT_PARSING:
    return "PARSING";
  case ERR_CAT_MEMORY:
    return "MEMORY";
  case ERR_CAT_HARDWARE:
    return "HARDWARE";
  case ERR_CAT_SYSTEM:
    return "SYSTEM";
  default:
    return "UNKNOWN";
  }
}

void ErrorHandler::log(ErrorLevel level, ErrorCategory category,
                       const String &message, const String &context) {
  // Create error record
  ErrorRecord error;
  error.timestamp = millis();
  error.level = level;
  error.category = category;
  error.message = message;
  error.context = context;

  // Add to recent errors (keep only last N)
  if (recentErrors.size() >= MAX_RECENT_ERRORS) {
    recentErrors.erase(recentErrors.begin());
  }
  recentErrors.push_back(error);

  // Serial output
  Serial.printf("[%s][%s] %s", levelToString(level).c_str(),
                categoryToString(category).c_str(), message.c_str());
  if (context.length() > 0) {
    Serial.printf(" (%s)", context.c_str());
  }
  Serial.println();

  // SD card logging (if enabled)
  if (sdLoggingEnabled && level >= ERR_WARN) {
    writeToSD(error);
  }

  // Fatal errors: additional handling
  if (level == ERR_FATAL) {
    Serial.println("!!! FATAL ERROR DETECTED !!!");
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
  }
}

void ErrorHandler::writeToSD(const ErrorRecord &error) {
  if (!sdExpander)
    return;

  sdExpander->digitalWrite(SD_CS, LOW);

  // Ensure logs directory exists
  if (!SD.exists("/logs")) {
    SD.mkdir("/logs");
  }

  // Open log file in append mode
  File logFile = SD.open("/logs/errors.log", FILE_APPEND);
  if (logFile) {
    // Format: [timestamp][level][category] message (context)
    logFile.printf(
        "[%lu][%s][%s] %s", error.timestamp, levelToString(error.level).c_str(),
        categoryToString(error.category).c_str(), error.message.c_str());
    if (error.context.length() > 0) {
      logFile.printf(" (%s)", error.context.c_str());
    }
    logFile.println();
    logFile.close();
  }

  sdExpander->digitalWrite(SD_CS, HIGH);
}

void ErrorHandler::logInfo(ErrorCategory category, const String &message,
                           const String &context) {
  log(ERR_INFO, category, message, context);
}

void ErrorHandler::logWarn(ErrorCategory category, const String &message,
                           const String &context) {
  log(ERR_WARN, category, message, context);
}

void ErrorHandler::logError(ErrorCategory category, const String &message,
                            const String &context) {
  log(ERR_ERROR, category, message, context);
}

void ErrorHandler::logFatal(ErrorCategory category, const String &message,
                            const String &context) {
  log(ERR_FATAL, category, message, context);
}

void ErrorHandler::checkMemory(const String &context) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();

  // Log if memory is getting low (< 50KB free)
  if (freeHeap < 50000) {
    logWarn(ERR_CAT_MEMORY,
            String("Low memory: ") + String(freeHeap) + " bytes free", context);
  }

  // Log critical memory situation (< 20KB free)
  if (freeHeap < 20000) {
    logError(ERR_CAT_MEMORY,
             String("Critical memory: ") + String(freeHeap) + " bytes free",
             context);
  }
}

bool ErrorHandler::isMemoryLow() { return ESP.getFreeHeap() < 50000; }

const std::vector<ErrorRecord> &ErrorHandler::getRecentErrors() {
  return recentErrors;
}

void ErrorHandler::clearRecentErrors() { recentErrors.clear(); }

void ErrorHandler::enableSDLogging(bool enable) { sdLoggingEnabled = enable; }
