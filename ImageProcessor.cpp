#include <Arduino.h>
#include <ESP_IOExpander_Library.h>
#include <FS.h>
#include <SD.h>
#include <TJpg_Decoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppGlobals.h"
#include "ErrorHandler.h"
#include "ImageProcessor.h"
#include "waveshare_sd_card.h"
#include <HTTPClient.h>

// Global buffer reference for the decoder callback
static uint16_t *_activeBuffer = nullptr;
static int _activeMaxWidth = 0;
static int _activeMaxHeight = 0;

static bool tjpg_callback(int16_t x, int16_t y, uint16_t w, uint16_t h,
                          uint16_t *bitmap) {
  if (!_activeBuffer)
    return false;

  for (int16_t j = 0; j < h; j++) {
    int py = y + j;
    if (py >= _activeMaxHeight)
      break; // Optimization: Don't process rows outside buffer

    int copyW = w;
    if (x + copyW > _activeMaxWidth) {
      copyW = _activeMaxWidth - x;
    }

    if (copyW > 0) {
      // Optimization: Use memcpy for row transfer instead of pixel loop
      memcpy(&_activeBuffer[py * _activeMaxWidth + x], &bitmap[j * w],
             copyW * sizeof(uint16_t));
    }
  }
  return true;
}

void ImageProcessor::init() { TJpgDec.setCallback(tjpg_callback); }

bool ImageProcessor::decodeToBuffer(String filename, uint16_t *buffer,
                                    int maxWidth, int maxHeight) {
  bool exists = false;
  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(2000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);
    exists = SD.exists(filename);
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  } else {
    return false;
  }

  if (!exists) {
    ErrorHandler::logWarn(ERR_CAT_STORAGE,
                          String("Image file not found: ") + filename,
                          "ImageProcessor::decodeToBuffer");
    return false;
  }

  _activeBuffer = buffer;
  _activeMaxWidth = maxWidth;
  _activeMaxHeight = maxHeight;

  // Clear buffer first
  memset(buffer, 0, maxWidth * maxHeight * sizeof(uint16_t));

  uint8_t result = 1; // Default fail
  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    // 1. Get dimensions to determine scale
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, filename.c_str());

    // 2. Calculate scale factor to avoid wasted decoding
    uint8_t scale = 1;
    if (w > maxWidth * 8 || h > maxHeight * 8)
      scale = 8;
    else if (w > maxWidth * 4 || h > maxHeight * 4)
      scale = 4;
    else if (w > maxWidth * 2 || h > maxHeight * 2)
      scale = 2;

    TJpgDec.setJpgScale(scale);

    // 3. Decode
    result = TJpgDec.drawSdJpg(0, 0, filename.c_str());

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);

    xSemaphoreGiveRecursive(i2cMutex);
  }

  if (result != 0) {
    ErrorHandler::logError(ERR_CAT_PARSING,
                           String("JPEG decode failed (code ") +
                               String(result) + "): " + filename,
                           "ImageProcessor::decodeToBuffer");
    return false;
  }

  return true;
}

bool ImageProcessor::decodeUrlToBuffer(String url, uint16_t *buffer,
                                       int maxWidth, int maxHeight) {
  // Not implemented: Requires downloading to temp file
  return false;
}
