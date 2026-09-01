#include "Utils.h"
#include <ctype.h>
#include <time.h>

namespace {

bool isUtf8Continuation(unsigned char value) {
  return (value & 0xC0) == 0x80;
}

// Decode one UTF-8 code point without ever reading beyond the supplied buffer.
// Invalid sequences consume one byte and become a visible ASCII question mark.
uint32_t decodeUtf8(const char *data, size_t length, size_t &offset) {
  const unsigned char first = static_cast<unsigned char>(data[offset++]);
  if (first < 0x80)
    return first;

  if (first >= 0xC2 && first <= 0xDF && offset < length) {
    const unsigned char second = static_cast<unsigned char>(data[offset]);
    if (isUtf8Continuation(second)) {
      offset++;
      return ((first & 0x1F) << 6) | (second & 0x3F);
    }
  } else if (first >= 0xE0 && first <= 0xEF && offset + 1 < length) {
    const unsigned char second = static_cast<unsigned char>(data[offset]);
    const unsigned char third = static_cast<unsigned char>(data[offset + 1]);
    const bool validSecond =
        isUtf8Continuation(second) &&
        !(first == 0xE0 && second < 0xA0) &&
        !(first == 0xED && second >= 0xA0);
    if (validSecond && isUtf8Continuation(third)) {
      offset += 2;
      return ((first & 0x0F) << 12) | ((second & 0x3F) << 6) |
             (third & 0x3F);
    }
  } else if (first >= 0xF0 && first <= 0xF4 && offset + 2 < length) {
    const unsigned char second = static_cast<unsigned char>(data[offset]);
    const unsigned char third = static_cast<unsigned char>(data[offset + 1]);
    const unsigned char fourth = static_cast<unsigned char>(data[offset + 2]);
    const bool validSecond =
        isUtf8Continuation(second) &&
        !(first == 0xF0 && second < 0x90) &&
        !(first == 0xF4 && second >= 0x90);
    if (validSecond && isUtf8Continuation(third) &&
        isUtf8Continuation(fourth)) {
      offset += 3;
      return ((first & 0x07) << 18) | ((second & 0x3F) << 12) |
             ((third & 0x3F) << 6) | (fourth & 0x3F);
    }
  }

  return '?';
}

void appendAscii(PsramString &output, const char *value) {
  output.append(value);
}

void appendLvglCodePoint(PsramString &output, uint32_t codePoint) {
  if (codePoint < 0x80) {
    // Preserve useful whitespace, but discard control characters that can
    // confuse LVGL's text layout.
    if (codePoint >= 0x20 || codePoint == '\n' || codePoint == '\r' ||
        codePoint == '\t')
      output.push_back(static_cast<char>(codePoint));
    return;
  }

  // Combining accents follow an already rendered base letter.
  if (codePoint >= 0x0300 && codePoint <= 0x036F)
    return;

  switch (codePoint) {
  case 0x00A0:
    output.push_back(' ');
    return;
  case 0x00A3:
    appendAscii(output, "GBP");
    return;
  case 0x00A5:
    appendAscii(output, "YEN");
    return;
  case 0x00A9:
    appendAscii(output, "(c)");
    return;
  case 0x00AE:
    appendAscii(output, "(R)");
    return;
  case 0x00B7:
  case 0x2022:
    output.push_back('-');
    return;
  case 0x00D7:
    output.push_back('x');
    return;
  case 0x00C6:
    appendAscii(output, "AE");
    return;
  case 0x00E6:
    appendAscii(output, "ae");
    return;
  case 0x00D0:
    output.push_back('D');
    return;
  case 0x00F0:
    output.push_back('d');
    return;
  case 0x00DE:
    appendAscii(output, "TH");
    return;
  case 0x00FE:
    appendAscii(output, "th");
    return;
  case 0x00DF:
    appendAscii(output, "ss");
    return;
  case 0x0152:
    appendAscii(output, "OE");
    return;
  case 0x0153:
    appendAscii(output, "oe");
    return;
  case 0x0131:
    output.push_back('i');
    return;
  case 0x0141:
    output.push_back('L');
    return;
  case 0x0142:
    output.push_back('l');
    return;
  case 0x2010:
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
  case 0x2212:
    output.push_back('-');
    return;
  case 0x2018:
  case 0x2019:
  case 0x201A:
  case 0x02BC:
    output.push_back('\'');
    return;
  case 0x201C:
  case 0x201D:
  case 0x201E:
    output.push_back('"');
    return;
  case 0x2026:
    appendAscii(output, "...");
    return;
  case 0x20AC:
    appendAscii(output, "EUR");
    return;
  case 0x2122:
    appendAscii(output, "(TM)");
    return;
  default:
    break;
  }

  // Latin letters with common accents. LVGL's compact Montserrat build only
  // contains the basic glyph range, so transliterate rather than draw boxes.
  if ((codePoint >= 0x00C0 && codePoint <= 0x00C5) ||
      (codePoint >= 0x0100 && codePoint <= 0x0105))
    output.push_back((codePoint & 1) && codePoint >= 0x0100 ? 'a' : 'A');
  else if (codePoint >= 0x00E0 && codePoint <= 0x00E5)
    output.push_back('a');
  else if (codePoint == 0x00C7 || codePoint == 0x0106 ||
           codePoint == 0x0108 || codePoint == 0x010A ||
           codePoint == 0x010C)
    output.push_back('C');
  else if (codePoint == 0x00E7 || codePoint == 0x0107 ||
           codePoint == 0x0109 || codePoint == 0x010B ||
           codePoint == 0x010D)
    output.push_back('c');
  else if (codePoint == 0x010E)
    output.push_back('D');
  else if (codePoint == 0x010F)
    output.push_back('d');
  else if ((codePoint >= 0x00C8 && codePoint <= 0x00CB) ||
           codePoint == 0x0112 || codePoint == 0x0114 ||
           codePoint == 0x0116 || codePoint == 0x0118 ||
           codePoint == 0x011A)
    output.push_back('E');
  else if ((codePoint >= 0x00E8 && codePoint <= 0x00EB) ||
           codePoint == 0x0113 || codePoint == 0x0115 ||
           codePoint == 0x0117 || codePoint == 0x0119 ||
           codePoint == 0x011B)
    output.push_back('e');
  else if (codePoint == 0x011C || codePoint == 0x011E ||
           codePoint == 0x0120 || codePoint == 0x0122)
    output.push_back('G');
  else if (codePoint == 0x011D || codePoint == 0x011F ||
           codePoint == 0x0121 || codePoint == 0x0123)
    output.push_back('g');
  else if (codePoint == 0x0124 || codePoint == 0x0126)
    output.push_back('H');
  else if (codePoint == 0x0125 || codePoint == 0x0127)
    output.push_back('h');
  else if ((codePoint >= 0x00CC && codePoint <= 0x00CF) ||
           codePoint == 0x0128 || codePoint == 0x012A ||
           codePoint == 0x012C || codePoint == 0x012E)
    output.push_back('I');
  else if ((codePoint >= 0x00EC && codePoint <= 0x00EF) ||
           codePoint == 0x0129 || codePoint == 0x012B ||
           codePoint == 0x012D || codePoint == 0x012F)
    output.push_back('i');
  else if (codePoint == 0x0134)
    output.push_back('J');
  else if (codePoint == 0x0135)
    output.push_back('j');
  else if (codePoint == 0x0136)
    output.push_back('K');
  else if (codePoint == 0x0137 || codePoint == 0x0138)
    output.push_back('k');
  else if (codePoint == 0x00D1 || codePoint == 0x0143 ||
           codePoint == 0x0145 || codePoint == 0x0147)
    output.push_back('N');
  else if (codePoint == 0x00F1 || codePoint == 0x0144 ||
           codePoint == 0x0146 || codePoint == 0x0148)
    output.push_back('n');
  else if ((codePoint >= 0x00D2 && codePoint <= 0x00D6) ||
           codePoint == 0x00D8 || codePoint == 0x014C ||
           codePoint == 0x014E || codePoint == 0x0150)
    output.push_back('O');
  else if ((codePoint >= 0x00F2 && codePoint <= 0x00F6) ||
           codePoint == 0x00F8 || codePoint == 0x014D ||
           codePoint == 0x014F || codePoint == 0x0151)
    output.push_back('o');
  else if (codePoint == 0x0154 || codePoint == 0x0156 ||
           codePoint == 0x0158)
    output.push_back('R');
  else if (codePoint == 0x0155 || codePoint == 0x0157 ||
           codePoint == 0x0159)
    output.push_back('r');
  else if (codePoint == 0x015A || codePoint == 0x015C ||
           codePoint == 0x015E || codePoint == 0x0160)
    output.push_back('S');
  else if (codePoint == 0x015B || codePoint == 0x015D ||
           codePoint == 0x015F || codePoint == 0x0161)
    output.push_back('s');
  else if (codePoint == 0x0162 || codePoint == 0x0164 ||
           codePoint == 0x0166)
    output.push_back('T');
  else if (codePoint == 0x0163 || codePoint == 0x0165 ||
           codePoint == 0x0167)
    output.push_back('t');
  else if ((codePoint >= 0x00D9 && codePoint <= 0x00DC) ||
           codePoint == 0x0168 || codePoint == 0x016A ||
           codePoint == 0x016C || codePoint == 0x016E ||
           codePoint == 0x0170 || codePoint == 0x0172)
    output.push_back('U');
  else if ((codePoint >= 0x00F9 && codePoint <= 0x00FC) ||
           codePoint == 0x0169 || codePoint == 0x016B ||
           codePoint == 0x016D || codePoint == 0x016F ||
           codePoint == 0x0171 || codePoint == 0x0173)
    output.push_back('u');
  else if (codePoint == 0x00DD || codePoint == 0x0176 ||
           codePoint == 0x0178)
    output.push_back('Y');
  else if (codePoint == 0x00FD || codePoint == 0x00FF ||
           codePoint == 0x0177)
    output.push_back('y');
  else if (codePoint == 0x0174)
    output.push_back('W');
  else if (codePoint == 0x0175)
    output.push_back('w');
  else if (codePoint == 0x0179 || codePoint == 0x017B ||
           codePoint == 0x017D)
    output.push_back('Z');
  else if (codePoint == 0x017A || codePoint == 0x017C ||
           codePoint == 0x017E)
    output.push_back('z');
  else {
    // One replacement per unsupported code point is substantially clearer
    // than one rectangle per UTF-8 byte.
    output.push_back('?');
  }
}

} // namespace

void sanitizeLvglTextInPlace(PsramString &text) {
  PsramString output;
  output.reserve(text.size());
  size_t offset = 0;
  while (offset < text.size()) {
    const uint32_t codePoint = decodeUtf8(text.data(), text.size(), offset);
    appendLvglCodePoint(output, codePoint);
  }
  text.swap(output);
}

String sanitizeLvglText(const String &text) {
  PsramString output(text.c_str());
  sanitizeLvglTextInPlace(output);
  return String(output.c_str());
}

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
