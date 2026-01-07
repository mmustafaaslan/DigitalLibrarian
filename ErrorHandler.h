#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <Arduino.h>
#include <vector>

// Error severity levels
enum ErrorLevel { ERR_INFO, ERR_WARN, ERR_ERROR, ERR_FATAL };

// Error categories
enum ErrorCategory {
  ERR_CAT_NETWORK,
  ERR_CAT_STORAGE,
  ERR_CAT_API,
  ERR_CAT_PARSING,
  ERR_CAT_MEMORY,
  ERR_CAT_HARDWARE,
  ERR_CAT_SYSTEM
};

// Error record
struct ErrorRecord {
  unsigned long timestamp;
  ErrorLevel level;
  ErrorCategory category;
  String message;
  String context; // Additional context (e.g., function name, URL)
};

class ErrorHandler {
private:
  static std::vector<ErrorRecord> recentErrors;
  static const int MAX_RECENT_ERRORS = 20;
  static bool sdLoggingEnabled;

  static void writeToSD(const ErrorRecord &error);
  static String levelToString(ErrorLevel level);
  static String categoryToString(ErrorCategory category);

public:
  // Core logging functions
  static void log(ErrorLevel level, ErrorCategory category,
                  const String &message, const String &context = "");

  // Convenience wrappers
  static void logInfo(ErrorCategory category, const String &message,
                      const String &context = "");
  static void logWarn(ErrorCategory category, const String &message,
                      const String &context = "");
  static void logError(ErrorCategory category, const String &message,
                       const String &context = "");
  static void logFatal(ErrorCategory category, const String &message,
                       const String &context = "");

  // Memory monitoring
  static void checkMemory(const String &context = "");
  static bool isMemoryLow();

  // Error retrieval
  static const std::vector<ErrorRecord> &getRecentErrors();
  static void clearRecentErrors();

  // Configuration
  static void enableSDLogging(bool enable);
  static void init();
};

#endif // ERROR_HANDLER_H
