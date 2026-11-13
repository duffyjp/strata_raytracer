// pak.cpp - implementation of the ZIP asset reader plus a self-contained DEFLATE decoder.
#include "pak.h"

#include <cstdio>
#include <cstring>

namespace {

std::string normalizeName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        out.push_back(c);
    }
    // Strip a leading "./" and any drive-ish prefix so authors can zip a folder directly.
    while (out.compare(0, 2, "./") == 0) out.erase(0, 2);
    return out;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1] << 8) | uint32_t(p[2] << 16) | uint32_t(p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) | (p[1] << 8)); }

}  // namespace

uint32_t Pak::crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool Pak::open(const std::string& path, std::string* error) {
    close();
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        close();
        return false;
    };

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return fail("cannot open file");
    fseek(f, 0, SEEK_END);
    long long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long long)2ull * 1024 * 1024 * 1024) {
        fclose(f);
        return fail("archive size out of range");
    }
    blob_.resize(size_t(sz));
    if (fread(blob_.data(), 1, size_t(sz), f) != size_t(sz)) {
        fclose(f);
        return fail("short read");
    }
    fclose(f);

    // Find the End Of Central Directory record near the tail (a ZIP comment may follow it).
    const size_t kMinEocd = 22, kMaxComment = 65535;
    if (blob_.size() < kMinEocd) return fail("file too small to be a zip archive");
    long long eocd = -1;
    long long scanStart = long long(blob_.size()) - long long(kMinEocd);
    long long scanEnd = (blob_.size() > kMaxComment + kMinEocd)
                            ? long long(blob_.size()) - long long(kMaxComment + kMinEocd)
                            : 0;
    for (long long i = scanStart; i >= scanEnd; --i) {
        if (rd32(&blob_[size_t(i)]) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd < 0) return fail("no end-of-central-directory record (not a .zip/.pak)");

    const uint8_t* e = &blob_[size_t(eocd)];
    uint16_t totalEntries = rd16(e + 10);
    uint32_t cdSize = rd32(e + 12);
    uint32_t cdOfs = rd32(e + 16);
    if (totalEntries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOfs == 0xFFFFFFFFu)
        return fail("zip64 archives are not supported; re-save without them");
    if (uint64_t(cdOfs) + cdSize > blob_.size()) return fail("central directory outside file");

    const uint8_t* p = &blob_[cdOfs];
    const uint8_t* cdEnd = &blob_[cdOfs + cdSize];
    for (uint16_t i = 0; i < totalEntries; ++i) {
        if (p + 46 > cdEnd || rd32(p) != 0x02014b50u) break;  // tolerate trailing padding
        uint16_t method = rd16(p + 10);
        uint32_t crc = rd32(p + 16);
        uint32_t csize = rd32(p + 20);
        uint32_t usize = rd32(p + 24);
        uint16_t nameLen = rd16(p + 28);
        uint16_t extraLen = rd16(p + 30);
        uint16_t cmtLen = rd16(p + 32);
        uint32_t localOfs = rd32(p + 42);
        std::string name = normalizeName(std::string(reinterpret_cast<const char*>(p + 46), nameLen));
        p += 46 + nameLen + extraLen + cmtLen;
        if (name.empty() || name.back() == '/') continue;  // directory entries
        PakEntry ent;
        ent.name = name;
        ent.method = method;
        ent.compSize = csize;
        ent.uncompSize = usize;
        ent.crc = crc;
        ent.offset = localOfs;
        if (index_.count(name)) continue;  // first entry wins
        index_[name] = entries_.size();
        entries_.push_back(std::move(ent));
    }

    path_ = path;
    if (error) error->clear();
    return true;
}

void Pak::close() {
    path_.clear();
    blob_.clear();
    entries_.clear();
    index_.clear();
}

bool Pak::has(const std::string& name) const { return index_.count(normalizeName(name)) != 0; }

std::vector<std::string> Pak::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.name);
    return out;
}

bool Pak::read(const std::string& name, std::vector<uint8_t>* out) const {
    auto it = index_.find(normalizeName(name));
    if (it == index_.end() || !out) return false;
    const PakEntry& ent = entries_[it->second];
    if (ent.offset + 30 > blob_.size()) return false;
    const uint8_t* lh = &blob_[size_t(ent.offset)];
    if (rd32(lh) != 0x04034b50u) return false;
    uint16_t nameLen = rd16(lh + 26);
    uint16_t extraLen = rd16(lh + 28);
    // Prefer the local header's sizes when the central directory left them as zero (streamed zip).
    uint32_t csize = rd32(lh + 18) ? rd32(lh + 18) : uint32_t(ent.compSize);
    uint32_t usize = rd32(lh + 22) ? rd32(lh + 22) : uint32_t(ent.uncompSize);
    uint64_t dataOfs = ent.offset + 30 + nameLen + extraLen;
    if (dataOfs + csize > blob_.size()) return false;
    const uint8_t* src = &blob_[size_t(dataOfs)];

    out->assign(size_t(usize), uint8_t(0));
    if (ent.method == 0) {
        if (usize != csize) return false;
        memcpy(out->data(), src, size_t(usize));
    } else if (ent.method == 8) {
        if (!inflateRaw(src, size_t(csize), out->data(), size_t(usize))) return false;
    } else {
        return false;  // unsupported compression method
    }

    if (ent.crc && Pak::crc32(out->data(), out->size()) != ent.crc) return false;
    return true;
}

// -----------------------------------------------------------------------------------------------
// DEFLATE (RFC 1951) - a compact puff.c-style decoder. No external zlib, keeping the exe
// self-contained while still accepting normally compressed pak files.
namespace {

struct Huffman {
    uint16_t counts[16];
    uint16_t symbols[288];
};

struct BitReader {
    const uint8_t* src;
    size_t len, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool overrun = false;

    int bit() {
        if (bitcnt == 0) {
            if (pos >= len) { overrun = true; return 0; }
            bitbuf = src[pos++];
            bitcnt = 8;
        }
        int b = int(bitbuf & 1);
        bitbuf >>= 1;
        --bitcnt;
        return b;
    }
    int bits(int need) {
        int v = 0;
        for (int i = 0; i < need; ++i) v |= bit() << i;
        return v;
    }
};

int decodeSymbol(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= br.bit();
        int count = h.counts[len];
        if (code - count < first) return h.symbols[index + (code - first)];
        index += count;
        first += count;
        code <<= 1;
        ++code;
    }
    return -1;
}

int buildHuffman(Huffman* h, const uint16_t* lengths, int count) {
    for (int i = 0; i <= 15; ++i) h->counts[i] = 0;
    for (int i = 0; i < count; ++i) ++h->counts[lengths[i]];
    if (h->counts[0] == count) return 0;  // empty code, legal for distance tree with no data
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return -1;  // over-subscribed
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h->counts[len];
    for (int sym = 0; sym < count; ++sym)
        if (lengths[sym]) h->symbols[offs[lengths[sym]]++] = uint16_t(sym);
    return left;
}

constexpr int kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30]   = {1, 2, 3, 4, 5, 7, 9,    13,    17,   25,    33,   49,    65,    97,   129,
                                 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
                                 12289, 16385, 24577};
constexpr int kDistExtra[30]  = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist, uint8_t* dst, size_t dstLen, size_t* w) {
    for (;;) {
        int sym = decodeSymbol(br, lit);
        if (sym < 0 || br.overrun) return false;
        if (sym < 256) {
            if (*w >= dstLen) return false;
            dst[(*w)++] = uint8_t(sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t length = size_t(kLengthBase[sym] + br.bits(kLengthExtra[sym]));
            int dsym = decodeSymbol(br, dist);
            if (dsym < 0 || dsym >= 30) return false;
            size_t offset = size_t(kDistBase[dsym] + br.bits(kDistExtra[dsym]));
            if (offset > *w || *w + length > dstLen) return false;
            const uint8_t* from = dst + *w - offset;
            for (size_t i = 0; i < length; ++i) dst[*w + i] = from[i];
            *w += length;
        }
    }
}

}  // namespace

bool inflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    BitReader br{src, srcLen, 0, 0, 0, false};
    size_t w = 0;
    int last = 0;

    while (!last) {
        last = br.bit();
        int type = br.bits(2);
        if (type == 0) {
            br.bitcnt = 0;  // discard the partial byte
            if (br.pos + 4 > srcLen) return false;
            uint16_t l = uint16_t(src[br.pos] | (src[br.pos + 1] << 8));
            uint16_t n = uint16_t(src[br.pos + 2] | (src[br.pos + 3] << 8));
            (void)n;
            br.pos += 4;
            if (br.pos + l > srcLen || w + l > dstLen) return false;
            memcpy(dst + w, src + br.pos, l);
            br.pos += l;
            w += l;
        } else if (type == 1) {
            uint16_t lengths[288];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < 288; ++i) lengths[i] = 8;
            Huffman lit{}, dist{};
            if (buildHuffman(&lit, lengths, 288) != 0) return false;
            uint16_t dlengths[30];
            for (int d = 0; d < 30; ++d) dlengths[d] = 5;
            if (buildHuffman(&dist, dlengths, 30) != 0) return false;
            if (!inflateBlock(br, lit, dist, dst, dstLen, &w)) return false;
        } else if (type == 2) {
            int nlen = br.bits(5) + 257;
            int ndist = br.bits(5) + 1;
            int ncode = br.bits(4) + 4;
            if (nlen > 288 || ndist > 30) return false;
            static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint16_t codeLens[19] = {0};
            for (int i = 0; i < ncode; ++i) codeLens[kOrder[i]] = uint16_t(br.bits(3));
            Huffman codeHuff{};
            if (buildHuffman(&codeHuff, codeLens, 19) != 0) return false;
            uint16_t lengths[288 + 30];
            memset(lengths, 0, sizeof(lengths));
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decodeSymbol(br, codeHuff);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[idx++] = uint16_t(sym);
                } else {
                    int repeat = sym == 16 ? lengths[idx - 1] : 0;
                    int n = sym == 16 ? 3 + br.bits(2) : (sym == 17 ? 3 + br.bits(3) : 11 + br.bits(7));
                    while (n-- && idx < nlen + ndist) lengths[idx++] = uint16_t(repeat);
                }
            }
            if (idx > nlen && lengths[nlen] == 0) return false;
            Huffman lit{}, dist{};
            if (buildHuffman(&lit, lengths, nlen) != 0) return false;
            if (buildHuffman(&dist, lengths + nlen, ndist) != 0) return false;
            if (!inflateBlock(br, lit, dist, dst, dstLen, &w)) return false;
        } else {
            return false;
        }
        if (br.overrun) return false;
    }
    return w == dstLen;
}
