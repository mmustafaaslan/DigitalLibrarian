#include "Utils.h"
#include <ctype.h>
#include <time.h>

String sanitizeText(String input) {
  String output = input;

  // 1. Hyphens and Dashes
  output.replace("\xe2\x80\x90", "-"); // Hyphen (U+2010)
  output.replace("\xe2\x80\x91", "-"); // Non-breaking hyphen (U+2011)
  output.replace("\xe2\x80\x92", "-"); // Figure dash (U+2012)
  output.replace("\xe2\x80\x93", "-"); // En dash (U+2013)
  output.replace("\xe2\x80\x94", "-"); // Em dash (U+2014)
  output.replace("\xe2\x80\x95", "-"); // Horizontal bar (U+2015)

  // 2. Quotes and Apostrophes
  output.replace("\xe2\x80\x98", "'");  // Left single quote
  output.replace("\xe2\x80\x99", "'");  // Right single quote (apostrophe)
  output.replace("\xe2\x80\x9c", "\""); // Left double quote
  output.replace("\xe2\x80\x9d", "\""); // Right double quote

  // 3. Other Common Symbols
  output.replace("\xe2\x80\xa6", "..."); // Ellipsis (...)
  output.replace("\xc2\xa0", " ");       // Non-breaking space
  output.replace("\xc2\xb7", "-");       // Middle Dot (·) -> Hyphen
  output.replace("\\", "");              // Remove literal backslashes

  // 4. Latin-1 Supplement Transliteration (to ASCII)
  // A variants
  output.replace("\xc3\x80", "A");
  output.replace("\xc3\x81", "A");
  output.replace("\xc3\x82", "A");
  output.replace("\xc3\x83", "A");
  output.replace("\xc3\x84", "A");
  output.replace("\xc3\x85", "A");
  output.replace("\xc3\xa0", "a");
  output.replace("\xc3\xa1", "a");
  output.replace("\xc3\xa2", "a");
  output.replace("\xc3\xa3", "a");
  output.replace("\xc3\xa4", "a");
  output.replace("\xc3\xa5", "a");
  // E variants
  output.replace("\xc3\x88", "E");
  output.replace("\xc3\x89", "E");
  output.replace("\xc3\x8a", "E");
  output.replace("\xc3\x8b", "E");
  output.replace("\xc3\xa8", "e");
  output.replace("\xc3\xa9", "e");
  output.replace("\xc3\xaa", "e");
  output.replace("\xc3\xab", "e");
  // I variants
  output.replace("\xc3\x8c", "I");
  output.replace("\xc3\x8d", "I");
  output.replace("\xc3\x8e", "I");
  output.replace("\xc3\x8f", "I");
  output.replace("\xc3\xac", "i");
  output.replace("\xc3\xad", "i");
  output.replace("\xc3\xae", "i");
  output.replace("\xc3\xaf", "i");
  // O variants
  output.replace("\xc3\x92", "O");
  output.replace("\xc3\x93", "O");
  output.replace("\xc3\x94", "O");
  output.replace("\xc3\x95", "O");
  output.replace("\xc3\x96", "O");
  output.replace("\xc3\x98", "O");
  output.replace("\xc3\xb2", "o");
  output.replace("\xc3\xb3", "o");
  output.replace("\xc3\xb4", "o");
  output.replace("\xc3\xb5", "o");
  output.replace("\xc3\xb6", "o");
  output.replace("\xc3\xb8", "o");
  // U variants
  output.replace("\xc3\x99", "U");
  output.replace("\xc3\x9a", "U");
  output.replace("\xc3\x9b", "U");
  output.replace("\xc3\x9c", "U");
  output.replace("\xc3\xb9", "u");
  output.replace("\xc3\xba", "u");
  output.replace("\xc3\xbb", "u");
  output.replace("\xc3\xbc", "u");
  // Y variants
  output.replace("\xc3\x9d", "Y");
  output.replace("\xc3\xbd", "y");
  output.replace("\xc3\xbf", "y");
  // Others
  output.replace("\xc3\x91", "N");
  output.replace("\xc3\xb1", "n");
  output.replace("\xc3\x87", "C");
  output.replace("\xc3\xa7", "c");
  output.replace("\xc3\x9f", "ss"); // Eszett

  return output;
}

String sanitizeFilename(String input) {
  // First call sanitizeText to handle smart quotes/dashes
  String output = sanitizeText(input);

  // Replace common invalid FAT32 characters
  output.replace(" ", "_");
  output.replace("/", "-");
  output.replace("\\", "-");
  output.replace(":", "-");
  output.replace("*", "");
  output.replace("?", "");
  output.replace("\"", "");
  output.replace("<", "");
  output.replace(">", "");
  output.replace("|", "");
  output.replace("'", ""); // Remove apostrophes to avoid FS/Library issues

  return output;
}

void decodeHTMLEntities(String &str) {
  str.replace("&amp;", "&");
  str.replace("&quot;", "\"");
  str.replace("&#39;", "'");
  str.replace("&apos;", "'");
  str.replace("&nbsp;", " ");
  str.replace("&lt;", "<");
  str.replace("&gt;", ">");

  // Unicode escapes
  str.replace("\\u0020", " ");
  str.replace("\\u00a0", " ");

  // Generic cleanup for remaining \uXXXX sequences
  while (str.indexOf("\\u") >= 0) {
    int idx = str.indexOf("\\u");
    if (idx + 6 <= str.length()) {
      str.remove(idx, 6);
      String rest = str.substring(idx);
      str = str.substring(0, idx) + " " + rest;
    } else {
      break;
    }
  }
}

String escapeJSON(String s) {
  String out = "";
  out.reserve(s.length() + 10);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '/')
      out += "\\/";
    else if (c == '\b')
      out += "\\b";
    else if (c == '\f')
      out += "\\f";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (c == '`')
      out += "\\`";
    else if (c >= 0 && (unsigned char)c <= 0x1f) {
      // Skip control chars
    } else {
      out += c;
    }
  }
  return out;
}

String escapeHTML(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String urlEncode(String str) {
  String encoded = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') {
      encoded += "%20";
    } else if (isalnum((unsigned char)c)) {
      encoded += c;
    } else {
      char code0, code1;
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9)
        code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9)
        code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

String unescapeJSON(String s) {
  String out = "";
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.length()) {
      char next = s[i + 1];
      if (next == '"')
        out += '"';
      else if (next == '\\')
        out += '\\';
      else if (next == '/')
        out += '/';
      else if (next == 'b')
        out += '\b';
      else if (next == 'f')
        out += '\f';
      else if (next == 'n')
        out += '\n';
      else if (next == 'r')
        out += '\r';
      else if (next == 't')
        out += '\t';
      else if (next == 'u' && i + 5 < s.length()) {
        // Very basic uXXXX skip
        i += 4;
      } else {
        out += next;
      }
      i++;
    } else {
      out += c;
    }
  }
  return out;
}

String extractJSONString(const String &json, const String &key,
                         int searchStart) {
  String searchKey = String("\"") + key + "\":\"";
  int keyIndex = json.indexOf(searchKey, searchStart);
  if (keyIndex < 0)
    return "";
  int valueStart = keyIndex + searchKey.length();
  int valueEnd = json.indexOf("\"", valueStart);
  if (valueEnd < 0)
    return "";
  return unescapeJSON(json.substring(valueStart, valueEnd));
}

int extractJSONInt(const String &json, const String &key, int searchStart) {
  String searchKey = String("\"") + key + "\":";
  int keyIndex = json.indexOf(searchKey, searchStart);
  if (keyIndex < 0)
    return 0;
  int valueStart = keyIndex + searchKey.length();
  int valueEnd = json.indexOf(",", valueStart);
  if (valueEnd < 0)
    valueEnd = json.indexOf("}", valueStart);
  if (valueEnd < 0)
    return 0;
  String valueStr = json.substring(valueStart, valueEnd);
  valueStr.trim();
  return valueStr.toInt();
}

String getCurrentISO8601Timestamp() {
  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

String formatDuration(unsigned long ms) {
  if (ms == 0)
    return "--:--";
  int totalSeconds = ms / 1000;
  int minutes = totalSeconds / 60;
  int seconds = totalSeconds % 60;
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
  return String(buffer);
}

String padTrackNumber(int trackNo) {
  if (trackNo < 10)
    return "0" + String(trackNo);
  return String(trackNo);
}

const char *getLyricsStatusIcon(const char *status) {
  if (strcmp(status, "cached") == 0) {
    return LV_SYMBOL_OK; // ✓ - Lyrics available
  } else if (strcmp(status, "missing") == 0) {
    return LV_SYMBOL_WARNING; // ⚠ - Not found
  } else {
    return LV_SYMBOL_REFRESH; // ↻ - Fetch lyrics (unchecked)
  }
}

String toTitleCase(String text) {
  String output = "";
  bool newWord = true;
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    // Treat spaces, hyphens, and parentheses as word boundaries
    if (c == ' ' || c == '-' || c == '(' || c == '[' || c == '.' || c == '/') {
      newWord = true;
      output += c;
    } else {
      if (newWord) {
        output += (char)toupper(c);
        newWord = false;
      } else {
        output += (char)tolower(c); // Lowercase the rest
      }
    }
  }
  return output;
}
