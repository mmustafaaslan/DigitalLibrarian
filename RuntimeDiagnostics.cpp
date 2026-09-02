#include "RuntimeDiagnostics.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <cstring>

namespace {
static constexpr uint32_t DIAGNOSTIC_MAGIC = 0x444C4243; // "DLBC"

struct RetainedDiagnosticState {
  uint32_t magic;
  uint32_t checksum;
  DiagnosticBreadcrumb breadcrumb;
};

RTC_DATA_ATTR RetainedDiagnosticState retainedState = {};
DiagnosticBreadcrumb previousInterrupted = {};
bool previousInterruptedValid = false;
int previousResetReason = 0;

uint32_t checksumFor(const DiagnosticBreadcrumb &value) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(value); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

bool retainedStateValid() {
  return retainedState.magic == DIAGNOSTIC_MAGIC &&
         retainedState.checksum == checksumFor(retainedState.breadcrumb);
}

void commitRetainedState() {
  retainedState.magic = DIAGNOSTIC_MAGIC;
  retainedState.checksum = checksumFor(retainedState.breadcrumb);
}

void copyText(char *target, size_t capacity, const char *source) {
  if (!target || capacity == 0)
    return;
  if (!source)
    source = "";
  strncpy(target, source, capacity - 1);
  target[capacity - 1] = '\0';
}
} // namespace

void RuntimeDiagnostics::begin(int resetReason) {
  previousResetReason = resetReason;
  if (retainedStateValid() && retainedState.breadcrumb.active) {
    previousInterrupted = retainedState.breadcrumb;
    previousInterruptedValid = true;
    Serial.printf("[REBOOT TRACE] job=%d item=%d/%d phase=%s "
                  "internal=%u largest=%u psram=%u stack=%u\n",
                  previousInterrupted.jobType,
                  previousInterrupted.itemIndex,
                  previousInterrupted.itemCount, previousInterrupted.phase,
                  (unsigned)previousInterrupted.freeInternal,
                  (unsigned)previousInterrupted.largestInternal,
                  (unsigned)previousInterrupted.freePsram,
                  (unsigned)previousInterrupted.workerStackWords);
  }

  const uint32_t nextSequence =
      retainedStateValid() ? retainedState.breadcrumb.sequence + 1 : 1;
  retainedState.breadcrumb = {};
  retainedState.breadcrumb.sequence = nextSequence;
  retainedState.breadcrumb.itemIndex = -1;
  commitRetainedState();
}

void RuntimeDiagnostics::beginOperation(int jobType, int itemIndex,
                                        int itemCount, const char *item) {
  retainedState.breadcrumb.jobType = jobType;
  retainedState.breadcrumb.itemIndex = itemIndex;
  retainedState.breadcrumb.itemCount = itemCount;
  retainedState.breadcrumb.active = 1;
  copyText(retainedState.breadcrumb.item,
           sizeof(retainedState.breadcrumb.item), item);
  markPhase("started");
}

void RuntimeDiagnostics::markPhase(const char *phase,
                                   uint32_t workerStackWords) {
  retainedState.breadcrumb.active = 1;
  retainedState.breadcrumb.uptimeMs = millis();
  retainedState.breadcrumb.freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  retainedState.breadcrumb.largestInternal =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                       MALLOC_CAP_8BIT);
  retainedState.breadcrumb.freePsram =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  retainedState.breadcrumb.workerStackWords = workerStackWords;
  copyText(retainedState.breadcrumb.phase,
           sizeof(retainedState.breadcrumb.phase), phase);
  commitRetainedState();
}

void RuntimeDiagnostics::clearOperation() {
  retainedState.breadcrumb.active = 0;
  copyText(retainedState.breadcrumb.phase,
           sizeof(retainedState.breadcrumb.phase), "idle");
  commitRetainedState();
}

bool RuntimeDiagnostics::hasPreviousInterruptedOperation() {
  return previousInterruptedValid;
}

DiagnosticBreadcrumb RuntimeDiagnostics::getPreviousInterruptedOperation() {
  return previousInterrupted;
}

DiagnosticBreadcrumb RuntimeDiagnostics::getCurrentOperation() {
  return retainedState.breadcrumb;
}

int RuntimeDiagnostics::getPreviousResetReason() {
  return previousResetReason;
}

String RuntimeDiagnostics::previousSummary() {
  if (!previousInterruptedValid)
    return "none";
  String summary = "J" + String(previousInterrupted.jobType) + " " +
                   String(previousInterrupted.itemIndex) + "/" +
                   String(previousInterrupted.itemCount) + " " +
                   String(previousInterrupted.phase) + " L" +
                   String(previousInterrupted.largestInternal / 1024) + "K";
  return summary;
}
