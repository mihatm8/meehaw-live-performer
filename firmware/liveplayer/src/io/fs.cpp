// meehaw live performer — filesystem helpers shared by the SD and POSIX implementations (SPEC §6).
// Everything is walk-based: no allocation, names are copied into caller-provided buffers.
#include "fs.h"
#include <string.h>

namespace lp {

namespace {

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Shared walk state for countEntries / entryAt.
struct Scan {
  const char* ext;     // extension filter or nullptr (= every entry, directories included)
  int count;           // matches seen so far
  int want;            // ordinal to copy out, or -1 to count everything
  char* out;           // destination for the wanted name
  int cap;
  bool found;          // the wanted entry exists and its name fitted in out
};

// With an extension only plain files match; without one every entry matches (so "/lp/packs" lists
// genre folders, SPEC §6). The SD walk already skips dotfiles.
inline bool matches(const Scan& s, const char* name, bool isDir) {
  if (!s.ext) return true;
  return !isDir && hasExt(name, s.ext);
}

bool scanFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  Scan& s = *static_cast<Scan*>(user);
  if (!matches(s, name, isDir)) return true;
  if (s.count == s.want) {
    // A name that does not fit is not copied at all: a silently truncated name would later join into a
    // path that does not exist, so the caller gets false (and an empty out) instead.
    if (s.out && s.cap > 0) {
      int i = 0;
      for (; name[i] && i < s.cap - 1; i++) s.out[i] = name[i];
      s.out[i] = 0;
      if (name[i]) { s.out[0] = 0; return false; }     // too long for the caller buffer: not found
    }
    s.found = true;
    return false;                      // stop the walk
  }
  s.count++;
  return true;
}

} // namespace

int countEntries(IFileSystem& fs, const char* path, const char* ext) {
  Scan s = {ext, 0, -1, nullptr, 0, false};
  if (!path || !fs.walk(path, scanFn, &s)) return 0;
  return s.count;
}

bool entryAt(IFileSystem& fs, const char* path, const char* ext, int n, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!path || n < 0) return false;
  Scan s = {ext, 0, n, out, cap, false};
  if (!fs.walk(path, scanFn, &s)) return false;
  return s.found;
}

bool hasExt(const char* name, const char* ext) {
  if (!ext || !ext[0]) return true;
  if (!name) return false;
  size_t nl = strlen(name), el = strlen(ext);
  if (nl < el) return false;
  const char* tail = name + nl - el;
  for (size_t i = 0; i < el; i++) if (lower(tail[i]) != lower(ext[i])) return false;
  return true;
}

// joinPath("/lp", "midi") -> "/lp/midi"; a single '/' is inserted between the parts, never doubled.
// An empty (or null) a keeps b verbatim, so joinPath("", "/lp/midi") stays the absolute "/lp/midi"
// rather than turning into a CWD-relative "lp/midi". Works in place when out == a. Truncates at
// cap - 1 and always NUL-terminates.
void joinPath(char* out, int cap, const char* a, const char* b) {
  if (!out || cap <= 0) return;
  int n = 0;
  if (a) for (; a[n] && n < cap - 1; n++) out[n] = a[n];
  if (b) {
    if (n > 0) {
      while (*b == '/') b++;                         // the separator comes from us
      if (*b && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
    }
    for (; *b && n < cap - 1; b++) out[n++] = *b;
  }
  out[n] = 0;
}

} // namespace lp
