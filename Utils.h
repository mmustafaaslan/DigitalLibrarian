#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>
#include <lvgl.h>

String sanitizeText(String text);
String sanitizeFilename(String filename);
void decodeHTMLEntities(String &text);
String escapeJSON(String text);
String escapeHTML(String text);
String urlEncode(String text);
String extractJSONString(const String &json, const String &key,
                         int startPos = 0);
int extractJSONInt(const String &json, const String &key, int startPos = 0);
String getCurrentISO8601Timestamp();
String formatDuration(unsigned long ms);
String padTrackNumber(int trackNo);
const char *getLyricsStatusIcon(const char *status);
String toTitleCase(String text); // New function

#endif // UTILS_H
