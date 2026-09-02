#ifndef RUNTIME_DIAGNOSTICS_H
#define RUNTIME_DIAGNOSTICS_H

#include <Arduino.h>

// A compact operation breadcrumb stored in RTC memory. It survives software,
// watchdog and most panic resets, so the next boot can report what the worker
// was doing immediately before the reset without writing flash or the SD card
// for every network phase.
struct DiagnosticBreadcrumb {
  uint32_t sequence;
  int16_t jobType;
  int16_t itemIndex;
  int16_t itemCount;
  uint8_t active;
  uint32_t uptimeMs;
  uint32_t freeInternal;
  uint32_t largestInternal;
  uint32_t freePsram;
  uint32_t workerStackWords;
  char phase[32];
  char item[40];
};

class RuntimeDiagnostics {
public:
  static void begin(int resetReason);
  static void beginOperation(int jobType, int itemIndex, int itemCount,
                             const char *item);
  static void markPhase(const char *phase, uint32_t workerStackWords = 0);
  static void clearOperation();

  static bool hasPreviousInterruptedOperation();
  static DiagnosticBreadcrumb getPreviousInterruptedOperation();
  static DiagnosticBreadcrumb getCurrentOperation();
  static int getPreviousResetReason();
  static String previousSummary();
};

#endif // RUNTIME_DIAGNOSTICS_H
