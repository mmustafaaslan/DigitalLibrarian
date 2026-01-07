#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include "waveshare_sd_card.h"
#include <Arduino.h>
#include <TJpg_Decoder.h>


class ImageProcessor {
public:
  static void init();
  static bool decodeToBuffer(String filename, uint16_t *buffer, int maxWidth,
                             int maxHeight);
  static bool decodeUrlToBuffer(String url, uint16_t *buffer, int maxWidth,
                                int maxHeight);
};

#endif
