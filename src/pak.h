// pak.h - minimal asset archive: a ZIP file (stored or deflated) read straight from disk.
//
// The application's entire asset surface goes through this. Adding an image, an audio clip or a
// replacement SPIR-V module to the archive requires no rebuild.
#ifndef PAK_H
#define PAK_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>

struct PakEntry {
    std::string name;
    uint16_t method = 0;      // 0 = stored, 8 = deflate
    uint64_t offset = 0;      // offset of local header
    uint64_t compSize = 0;
    uint64_t uncompSize = 0;
    uint32_t crc = 0;
};

class Pak {
public:
    // Opens `path`, indexes the central directory. Returns false (and sets error()) if the file
    // is missing or is not a readable ZIP. An empty-but-valid archive is not an error.
    bool open(const std::string& path, std::string* error = nullptr);
    void close();

    bool has(const std::string& name) const;
    // Names are matched case-insensitively with '/' separators, as produced by every zip tool.
    bool read(const std::string& name, std::vector<uint8_t>* out) const;
    std::vector<std::string> names() const;

    const std::string& path() const { return path_; }
    size_t entryCount() const { return entries_.size(); }

    static uint32_t crc32(const void* data, size_t len);

private:
    std::string path_;
    std::vector<uint8_t> blob_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<PakEntry> entries_;
};

// Raw DEFLATE (RFC1951) inflate. Returns false on malformed input.
bool inflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);

#endif  // PAK_H
