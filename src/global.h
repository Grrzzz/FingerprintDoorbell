#ifndef GLOBAL_H
#define GLOBAL_H

#include <WString.h>

extern void notifyClients(const String& message);
extern String getTimestampString();

// Parses up to n integers separated by sep; fields that are missing or malformed keep their value. Returns the number parsed.
inline int parseInts(const String& s, char sep, int* out, int n) {
  int count = 0, pos = 0;
  while (count < n && pos <= (int)s.length()) {
    int end = s.indexOf(sep, pos);
    if (end < 0) end = s.length();
    String tok = s.substring(pos, end);
    tok.trim();
    if (tok.length() == 0 || !(isDigit(tok[0]) || tok[0] == '-' || tok[0] == '+')) break;
    out[count++] = tok.toInt();
    pos = end + 1;
  }
  return count;
}

#endif