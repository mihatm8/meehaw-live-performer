// IFileSystem over the ESP-IDF VFS of the SD card (FatFs mounted at /sd by the Arduino SD library). Files are POSIX
// descriptors (open / read / lseek / write / close): one FatFs FIL of ≈ 1.1 KB per open file. The Arduino `File`
// path (fopen + a stdio buffer) cost ≈ 4.9 KB per open file (measured 2026-10-16, SPEC §9.19 §2), which with two
// vocal streams and two loop slots open left no heap for anything else. With descriptors an open costs no heap at
// run time: the FatFs FILs (4 KB sector buffer each, FF_MAX_SS = 4096 in this core) are preallocated at mount for
// max_files slots, so kMaxOpen is both the pool size and the mount's max_files.
// Directory walks use opendir / readdir (entry sizes are not read: no caller needs them). Device only.
#pragma once
#ifdef ARDUINO
#include <SD.h>
#include "../io/fs.h"
#include "../core/config.h"
namespace lp {
class SdFile : public IFile {
 public:
  int fd = -1; bool inUse = false;
  bool valid() const override { return inUse && fd >= 0; }
  size_t read(void* dst, size_t n) override;
  size_t write(const void* src, size_t n) override;
  bool seek(uint32_t pos) override;
  uint32_t position() const override;
  uint32_t size() const override;
  void close() override;
  bool sync() override;                             // ::fsync → FatFs f_sync: the FIL buffer and the directory entry (size, chain) hit the card
};
class SdFs : public IFileSystem {
 public:
  static constexpr int kMaxOpen = kMaxOpenFiles;    // = the VFS max_files (config.h, 7): 4 stream files (two vocal, two loop slots) + 2 pending stream switches + one kit / index / set file. Mounted before the boot pools so the 29 KB preallocation fits one free block. Each slot is a preallocated 4.2 KB FIL (see sd_fs.cpp), so this is the whole open-file budget of the device; Generator::streamOpenAllowed keeps the last handle free for the job's own file (SPEC §9.19g)
  static constexpr const char* kMount = "/sd";       // the Arduino SD library's mount point; our paths start with /lp
  bool begin();                                     // SPI.begin + SD.begin with the Cardputer ADV pins, max_files = kMaxOpen + 2
  bool exists(const char* path) override;
  bool isDir(const char* path) override;
  IFile* open(const char* path, const char* mode = "r") override;
  bool mkdir(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;   // VFS rename; FatFs refuses to overwrite (FR_EXIST → EEXIST), so `to` is unlinked first then — the new file is already complete at that point (Sets' .tmp rewrites)
  bool walk(const char* path, DirEntryFn fn, void* user) override;
  int openCount() const override { int n = 0; for (const SdFile& s : pool_) n += s.inUse ? 1 : 0; return n; }   // handles in use (stat / [diag]; the generator's handle budget, SPEC §9.19g)
 private:
  SdFile pool_[kMaxOpen];
  static const char* full(const char* path, char* buf, int cap);   // "/lp/..." → "/sd/lp/..."
};
}
#endif
