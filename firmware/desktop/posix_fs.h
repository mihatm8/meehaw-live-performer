// meehaw live performer — IFileSystem over POSIX for the desktop build and host tests (SPEC §7).
// Card-root paths ("/lp/midi") map to root + path, e.g. PosixFs("../../sdcard"); an empty or null root is
// the host root "/". A null path is refused by every method. Fixed pool of kMaxOpenFiles files (config.h, 7): the
// device's FatFs slot count (SdFs::kMaxOpen), so the desktop build and the host tests run out of handles exactly where
// the device would (SPEC §9.19g).
#pragma once
#include <stdio.h>
#include "../liveplayer/src/io/fs.h"
#include "../liveplayer/src/core/config.h"

namespace lp {

class PosixFile : public IFile {
 public:
  bool valid() const override { return fp_ != nullptr; }
  size_t read(void* dst, size_t n) override;
  size_t write(const void* src, size_t n) override;
  bool seek(uint32_t pos) override;
  uint32_t position() const override;
  uint32_t size() const override;
  void close() override;
  bool sync() override;                              // fflush + fsync: the bytes and the size reach the file as a second handle sees it
 private:
  friend class PosixFs;
  FILE* fp_ = nullptr;
  bool writable_ = false;
};

class PosixFs : public IFileSystem {
 public:
  static constexpr int kMaxOpen = kMaxOpenFiles;
  static constexpr int kRootMax = 256;
  static constexpr int kFullMax = 512;
  explicit PosixFs(const char* root);
  ~PosixFs() override;
  bool exists(const char* path) override;
  bool isDir(const char* path) override;
  IFile* open(const char* path, const char* mode = "r") override;
  bool mkdir(const char* path) override;             // 0755; true if it exists as a directory afterwards
  bool remove(const char* path) override;            // file or empty directory
  bool rename(const char* from, const char* to) override;   // ::rename on the mapped paths: atomic, replaces an existing `to`
  bool walk(const char* path, DirEntryFn fn, void* user) override;   // skips ".", ".." and dotfiles
  const char* root() const { return root_; }
  int openCount() const override;
 private:
  char root_[kRootMax];
  PosixFile pool_[kMaxOpen];
  void full(char* out, const char* path) const;      // root + "/" + path into out[kFullMax]
};

} // namespace lp
