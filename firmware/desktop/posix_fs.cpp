// meehaw live performer — POSIX filesystem for the desktop build and host tests (SPEC §7).
#include "posix_fs.h"
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lp {

// ---- PosixFile ------------------------------------------------------------------------------------

size_t PosixFile::read(void* dst, size_t n) { return fp_ ? fread(dst, 1, n, fp_) : 0; }
size_t PosixFile::write(const void* src, size_t n) { return (fp_ && writable_) ? fwrite(src, 1, n, fp_) : 0; }
bool PosixFile::seek(uint32_t pos) { return fp_ && fseek(fp_, (long)pos, SEEK_SET) == 0; }
uint32_t PosixFile::position() const { if (!fp_) return 0; long p = ftell(fp_); return p < 0 ? 0 : (uint32_t)p; }

uint32_t PosixFile::size() const {
  if (!fp_) return 0;
  if (writable_) fflush(fp_);                         // count buffered writes
  struct stat st;
  if (fstat(fileno(fp_), &st) != 0) return 0;
  return (uint32_t)st.st_size;
}

void PosixFile::close() {
  if (fp_) fclose(fp_);
  fp_ = nullptr;
  writable_ = false;
}

bool PosixFile::sync() {
  if (!fp_ || !writable_) return false;
  if (fflush(fp_) != 0) return false;                // the stdio buffer to the kernel ...
  return fsync(fileno(fp_)) == 0;                    // ... and the kernel's to the disk (the device's f_sync equivalent)
}

// ---- PosixFs --------------------------------------------------------------------------------------

// An empty or null root means the host root "/", so card paths stay absolute ("/lp/midi" -> "/lp/midi")
// instead of silently becoming CWD-relative.
PosixFs::PosixFs(const char* root) {
  root_[0] = 0;
  if (root) { strncpy(root_, root, kRootMax - 1); root_[kRootMax - 1] = 0; }
  size_t n = strlen(root_);
  while (n > 1 && root_[n - 1] == '/') root_[--n] = 0;   // no trailing slash (keep a lone "/")
  if (n == 0) { root_[0] = '/'; root_[1] = 0; }
}

PosixFs::~PosixFs() { for (auto& f : pool_) f.close(); }

void PosixFs::full(char* out, const char* path) const {
  joinPath(out, kFullMax, root_, path ? path : "");
}

// A null path is never the root: every method answers false / nullptr for it, like open() always did.
bool PosixFs::exists(const char* path) {
  if (!path) return false;
  char p[kFullMax]; full(p, path);
  struct stat st;
  return stat(p, &st) == 0;
}

bool PosixFs::isDir(const char* path) {
  if (!path) return false;
  char p[kFullMax]; full(p, path);
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

IFile* PosixFs::open(const char* path, const char* mode) {
  if (!path) return nullptr;
  char p[kFullMax]; full(p, path);
  const char m = (mode && mode[0]) ? mode[0] : 'r';
  if (m == 'r') {                                     // fopen() happily opens directories for reading
    struct stat st;
    if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) return nullptr;
  }
  for (auto& f : pool_) {
    if (f.fp_) continue;
    FILE* fp = fopen(p, m == 'w' ? "wb" : m == 'a' ? "ab" : "rb");
    if (!fp) return nullptr;
    f.fp_ = fp;
    f.writable_ = m != 'r';
    return &f;
  }
  return nullptr;                                     // pool exhausted
}

bool PosixFs::mkdir(const char* path) {
  if (!path) return false;
  char p[kFullMax]; full(p, path);
  if (::mkdir(p, 0755) == 0) return true;
  return errno == EEXIST && isDir(path);
}

bool PosixFs::remove(const char* path) {
  if (!path) return false;
  char p[kFullMax]; full(p, path);
  return ::remove(p) == 0;
}

bool PosixFs::rename(const char* from, const char* to) {
  if (!from || !to) return false;
  char a[kFullMax]; full(a, from);
  char b[kFullMax]; full(b, to);
  return ::rename(a, b) == 0;                         // POSIX: atomic, an existing `to` is replaced
}

bool PosixFs::walk(const char* path, DirEntryFn fn, void* user) {
  if (!path || !fn) return false;
  char p[kFullMax]; full(p, path);
  DIR* d = opendir(p);
  if (!d) return false;
  char child[kFullMax];
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;               // ".", ".." and dotfiles (matches SdFs::walk)
    joinPath(child, kFullMax, p, e->d_name);
    struct stat st;
    bool isDir = false; uint32_t size = 0;
    if (stat(child, &st) == 0) { isDir = S_ISDIR(st.st_mode); size = isDir ? 0 : (uint32_t)st.st_size; }
    if (!fn(e->d_name, isDir, size, user)) break;
  }
  closedir(d);
  return true;
}

int PosixFs::openCount() const {
  int c = 0;
  for (const auto& f : pool_) if (f.fp_) c++;
  return c;
}

} // namespace lp
