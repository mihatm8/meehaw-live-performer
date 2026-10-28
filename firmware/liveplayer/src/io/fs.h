// meehaw live performer — minimal filesystem interface implemented by SD (device) and POSIX (desktop).
// All paths are absolute from the card root, e.g. "/lp/midi/Chord/Amin". No heap in callers.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace lp {

class IFile {
 public:
  virtual ~IFile() {}
  virtual bool valid() const = 0;
  virtual size_t read(void* dst, size_t n) = 0;
  virtual size_t write(const void* src, size_t n) = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual uint32_t position() const = 0;
  virtual uint32_t size() const = 0;
  virtual void close() = 0;
  // Pushes buffered data AND the directory entry (size, cluster chain) to the medium, so a power loss right after leaves the
  // file readable up to here by another handle. FatFs in this core (CONFIG_FATFS_IMMEDIATE_FSYNC unset) parks writes in
  // the FIL's 4 KB buffer and updates the entry only at f_sync / f_close: the Recorder (SPEC §9.23) syncs after every
  // flush of its event buffer. SdFile: ::fsync(fd) (one or two sector writes, a few ms); PosixFile: fflush + fsync. A
  // file system without it answers false (the default, so decorators and stubs keep compiling); a caller treats that as
  // "no durability promise", not as an error.
  virtual bool sync() { return false; }
};

// Directory entry callback: return false to stop the walk.
typedef bool (*DirEntryFn)(const char* name, bool isDir, uint32_t size, void* user);

class IFileSystem {
 public:
  virtual ~IFileSystem() {}
  virtual bool exists(const char* path) = 0;
  virtual bool isDir(const char* path) = 0;
  // open returns a file object owned by the filesystem (fixed small pool); close() releases it.
  // At most kMaxOpenFiles (config.h) files open at once on both implementations. mode: "r", "w", "a".
  virtual IFile* open(const char* path, const char* mode = "r") = 0;
  virtual bool mkdir(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  // Renames (moves) a file inside the card, REPLACING an existing `to` (POSIX rename semantics). Sets' rewrites rely on it
  // (SPEC §9.22: <name>.tmp is written whole, then renamed over the original, so the card is never without a valid copy).
  // A file system that cannot rename answers false (the default, so decorators and stubs keep compiling); PosixFs maps to
  // ::rename, SdFs to the VFS rename with an unlink of `to` first when FatFs refuses to overwrite (FR_EXIST → EEXIST).
  virtual bool rename(const char* from, const char* to) { (void)from; (void)to; return false; }
  // Walks a directory in the storage order (stable between calls while the folder is unchanged).
  virtual bool walk(const char* path, DirEntryFn fn, void* user) = 0;
  // Files open now (handles of the fixed pool in use). The generator keeps a new-file stream open from taking the last of
  // kMaxOpenFiles handles (SPEC §9.19g); a file system that cannot tell answers 0 (no handle budget is enforced then).
  virtual int openCount() const { return 0; }
};

// Helpers shared by both implementations.
int countEntries(IFileSystem& fs, const char* path, const char* ext /* ".mid" or nullptr */);
// Finds the n-th entry (0-based) with the given extension; copies its name into out (cap bytes). Returns false
// if absent, and also when the name does not fit in cap (out is then "" rather than a truncated name).
bool entryAt(IFileSystem& fs, const char* path, const char* ext, int n, char* out, int cap);
bool hasExt(const char* name, const char* ext);       // case-insensitive
void joinPath(char* out, int cap, const char* a, const char* b);   // a + "/" + b; empty a keeps b verbatim

} // namespace lp
