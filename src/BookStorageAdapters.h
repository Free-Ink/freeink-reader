#pragma once

// freeink-reader — SD-card adapters binding FreeInkBook's storage interfaces
// to SDCardManager/SdFat. Everything else in the reader is SDK code.

#include <BookStorage.h>
#include <SDCardManager.h>

// Random access over one EPUB file on the card.
class SdBookSource : public freeink::book::BookSource {
 public:
  bool open(const char* path) {
    file_ = SdMan.open(path, O_RDONLY);
    return file_ && (size_ = file_.fileSize()) > 0;
  }
  void close() {
    if (file_) file_.close();
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_ || !file_.seekSet(offset)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return size_; }

 private:
  FsFile file_;
  uint64_t size_ = 0;
};

// Layout-cache files under a per-book directory, torn-write safe via a temp
// name + rename commit.
class SdCacheStorage : public freeink::book::CacheStorage {
 public:
  void setDir(const char* dir) {
    snprintf(dir_, sizeof(dir_), "%s", dir);
    SdMan.ensureDirectoryExists(dir_);
  }
  bool exists(const char* name) override { return SdMan.exists(path(name)); }
  bool remove(const char* name) override { return SdMan.remove(path(name)); }
  int64_t fileSize(const char* name) override {
    FsFile f = SdMan.open(path(name), O_RDONLY);
    if (!f) return -1;
    const int64_t size = f.fileSize();
    f.close();
    return size;
  }
  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override {
    FsFile f = SdMan.open(path(name), O_RDONLY);
    if (!f || !f.seekSet(offset)) return -1;
    const int32_t n = f.read(dst, len);
    f.close();
    return n;
  }
  bool beginWrite(const char* name) override {
    snprintf(commitPath_, sizeof(commitPath_), "%s/%s", dir_, name);
    write_ = SdMan.open(path("_tmp.fibp"), O_WRONLY | O_CREAT | O_TRUNC);
    return static_cast<bool>(write_);
  }
  bool write(const void* data, uint32_t len) override {
    return write_ && write_.write(data, len) == len;
  }
  bool endWrite() override {
    if (!write_) return false;
    write_.close();
    SdMan.remove(commitPath_);
    return SdMan.rename(path("_tmp.fibp"), commitPath_);
  }

 private:
  const char* path(const char* name) {
    snprintf(pathBuf_, sizeof(pathBuf_), "%s/%s", dir_, name);
    return pathBuf_;
  }
  char dir_[96] = "/BookCache";
  char pathBuf_[192];
  char commitPath_[192];
  FsFile write_;
};
