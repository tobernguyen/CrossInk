#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

struct HalFile {
  FILE* fp = nullptr;
  int read(uint8_t* buf, int n) { return fp ? static_cast<int>(fread(buf, 1, n, fp)) : -1; }
  void close() {
    if (fp) fclose(fp);
    fp = nullptr;
  }
  ~HalFile() { close(); }
};

struct HalStorageStub {
  bool openFileForRead(const char*, const std::string& path, HalFile& f) {
    f.close();
    f.fp = fopen(path.c_str(), "rb");
    return f.fp != nullptr;
  }
};
inline HalStorageStub Storage;
