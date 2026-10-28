#ifdef ARDUINO
#include "sd_fs.h"
#include <SPI.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
namespace lp {

const char* SdFs::full(const char* path, char* buf, int cap) {
  if (!path) path = "";
  snprintf(buf, cap, "%s%s%s", kMount, path[0] == '/' ? "" : "/", path);
  return buf;
}

bool SdFs::begin() {
  SPI.begin(40, 39, 14, 12);                         // SCK, MISO, MOSI, CS (Cardputer ADV, SPEC §1)
  return SD.begin(12, SPI, 25000000, kMount, kMaxOpen);   // max_files = our pool: the VFS preallocates one FatFs FIL per slot (FF_MAX_SS 4096 → ≈ 4.2 KB each, for ever); 8 slots exceeded the largest free block (ESP_ERR_NO_MEM)
}

bool SdFs::exists(const char* path) { char b[kMaxPathLen + 8]; struct stat st; return ::stat(full(path, b, sizeof b), &st) == 0; }
bool SdFs::isDir(const char* path) { char b[kMaxPathLen + 8]; struct stat st; return ::stat(full(path, b, sizeof b), &st) == 0 && S_ISDIR(st.st_mode); }
bool SdFs::mkdir(const char* path) { char b[kMaxPathLen + 8]; return ::mkdir(full(path, b, sizeof b), 0777) == 0 || isDir(path); }
bool SdFs::remove(const char* path) { char b[kMaxPathLen + 8]; return ::unlink(full(path, b, sizeof b)) == 0; }

// POSIX rename semantics over the VFS. ESP-IDF's vfs_fat hands FatFs' FR_EXIST back as EEXIST instead of replacing the
// destination, so an existing `to` is unlinked and the rename repeated: between the two the card holds the complete new
// file under its temporary name (Sets writes <name>.tmp whole before calling this, SPEC §9.22), never no copy at all.
bool SdFs::rename(const char* from, const char* to) {
  if (!from || !to) return false;
  char a[kMaxPathLen + 8]; full(from, a, sizeof a);
  char b[kMaxPathLen + 8]; full(to, b, sizeof b);
  if (::rename(a, b) == 0) return true;
  if (errno != EEXIST) return false;
  if (::unlink(b) != 0) return false;
  return ::rename(a, b) == 0;
}

IFile* SdFs::open(const char* path, const char* mode) {
  for (auto& s : pool_) {
    if (s.inUse) continue;
    char b[kMaxPathLen + 8]; full(path, b, sizeof b);
    int flags = O_RDONLY;
    if (mode && mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode && mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    int fd = ::open(b, flags, 0666);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); return nullptr; }
    s.fd = fd; s.inUse = true;
    return &s;
  }
  return nullptr;
}

size_t SdFile::read(void* dst, size_t n) { if (!valid()) return 0; ssize_t r = ::read(fd, dst, n); return r < 0 ? 0 : (size_t)r; }
size_t SdFile::write(const void* src, size_t n) { if (!valid()) return 0; ssize_t r = ::write(fd, src, n); return r < 0 ? 0 : (size_t)r; }
bool SdFile::seek(uint32_t pos) { return valid() && ::lseek(fd, (off_t)pos, SEEK_SET) == (off_t)pos; }
uint32_t SdFile::position() const { if (!valid()) return 0; off_t p = ::lseek(fd, 0, SEEK_CUR); return p < 0 ? 0 : (uint32_t)p; }
uint32_t SdFile::size() const { if (!valid()) return 0; struct stat st; return ::fstat(fd, &st) == 0 ? (uint32_t)st.st_size : 0; }
void SdFile::close() { if (inUse) { if (fd >= 0) ::close(fd); fd = -1; inUse = false; } }
// vfs_fat maps fsync to f_sync: the sector held in the FIL buffer goes out and the directory entry is rewritten with the
// current size and cluster chain (two sector writes at most, a few ms). Without it a power loss leaves a 0-byte file
// (or its last synced size) and a lost chain; the Recorder calls it after each ≤ 512 B flush (SPEC §9.23).
bool SdFile::sync() { return valid() && ::fsync(fd) == 0; }

bool SdFs::walk(const char* path, DirEntryFn fn, void* user) {
  char b[kMaxPathLen + 8]; DIR* d = ::opendir(full(path, b, sizeof b));
  if (!d) return false;
  struct dirent* e;
  while ((e = ::readdir(d)) != nullptr) {
    const char* name = e->d_name;
    if (name[0] == '.') continue;
    const bool isDir = e->d_type == DT_DIR;
    if (!fn(name, isDir, 0, user)) break;          // sizes are not read (an extra stat per entry); no caller uses them
  }
  ::closedir(d);
  return true;
}

}
#endif
