#include "formats.h"
#include <objidl.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace model {
namespace {
using std::string_view;
constexpr uint64_t MaxInflated = 1024ull * 1024 * 1024;   // largest 3MF part we will unpack

[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }

std::wstring Widen(string_view utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), length);
    return wide;
}

std::string Lower(string_view text) {
    std::string out(text);
    for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void AddTriangle(stl::Mesh& mesh, stl::Point a, stl::Point b, stl::Point c) {
    if (mesh.triangles.size() >= stl::MaxFaces) Fail("模型超過 200 萬個三角面。");
    mesh.triangles.push_back({a, b, c});
}

// ---- Deflate (RFC 1951), table driven -------------------------------------------------------

class Inflater {
public:
    Inflater(const uint8_t* data, size_t size, std::vector<uint8_t>& output, size_t limit)
        : input(data), length(size), out(output), limit(limit) {}

    void Run() {
        for (bool last = false; !last;) {
            last = Bits(1) != 0;
            const int type = int(Bits(2));
            if (type == 0) Stored();
            else if (type == 1) Block(FixedLiterals(), FixedDistances());
            else if (type == 2) Dynamic();
            else Fail("3MF 壓縮資料損毀。");
        }
    }

private:
    static constexpr int FastBits = 10;
    struct Huffman {
        uint16_t fast[1 << FastBits]{};   // (length << 9) | symbol, 0 when the code is longer
        uint16_t firstCode[17]{}, firstSymbol[17]{};
        uint32_t maxCode[18]{};
        uint8_t size[288]{};
        uint16_t value[288]{};
    };

    static int Reverse(int code, int bits) {
        int result = 0;
        for (int i = 0; i < bits; ++i) result |= ((code >> i) & 1) << (bits - 1 - i);
        return result;
    }

    static void Build(Huffman& h, const uint8_t* lengths, int count) {
        h = {};
        int sizes[17]{};
        for (int i = 0; i < count; ++i) ++sizes[lengths[i]];
        sizes[0] = 0;
        int nextCode[16]{};
        int code = 0, k = 0;
        for (int i = 1; i < 16; ++i) {
            nextCode[i] = code;
            h.firstCode[i] = uint16_t(code);
            h.firstSymbol[i] = uint16_t(k);
            code += sizes[i];
            if (code > (1 << i)) Fail("3MF 壓縮資料損毀。");   // over-subscribed code
            h.maxCode[i] = uint32_t(code) << (16 - i);
            code <<= 1;
            k += sizes[i];
        }
        h.maxCode[16] = 0x10000;
        for (int i = 0; i < count; ++i) {
            const int s = lengths[i];
            if (!s) continue;
            const int c = nextCode[s] - h.firstCode[s] + h.firstSymbol[s];
            h.size[c] = uint8_t(s);
            h.value[c] = uint16_t(i);
            if (s <= FastBits) {
                for (int j = Reverse(nextCode[s], s); j < (1 << FastBits); j += 1 << s)
                    h.fast[j] = uint16_t((s << 9) | i);
            }
            ++nextCode[s];
        }
    }

    void Fill() {
        while (count <= 56) {
            const uint64_t byte = position < length ? input[position] : 0;
            if (position >= length) overrun += 8;
            if (overrun > 64) Fail("3MF 壓縮資料不完整。");
            ++position;
            buffer |= byte << count;
            count += 8;
        }
    }

    uint32_t Bits(int n) {
        if (count < n) Fill();
        if (count - overrun < n) Fail("3MF 壓縮資料不完整。");
        const uint32_t value = uint32_t(buffer & ((1ull << n) - 1));
        buffer >>= n;
        count -= n;
        return value;
    }

    int Decode(const Huffman& h) {
        if (count < 16) Fill();
        const int fast = h.fast[buffer & ((1 << FastBits) - 1)];
        if (fast) {
            const int s = fast >> 9;
            Bits(s);
            return fast & 511;
        }
        const int k = Reverse(int(buffer & 0xFFFF), 16);
        int s = FastBits + 1;
        while (s < 16 && uint32_t(k) >= h.maxCode[s]) ++s;
        if (s >= 16) Fail("3MF 壓縮資料損毀。");
        const int index = (k >> (16 - s)) - h.firstCode[s] + h.firstSymbol[s];
        if (index < 0 || index >= 288 || h.size[index] != s) Fail("3MF 壓縮資料損毀。");
        Bits(s);
        return h.value[index];
    }

    void Stored() {
        Bits(count & 7);   // align to a byte boundary
        const uint32_t len = Bits(16), nlen = Bits(16);
        if ((len ^ 0xFFFF) != nlen) Fail("3MF 壓縮資料損毀。");
        Grow(len);
        for (uint32_t i = 0; i < len; ++i) out.push_back(uint8_t(Bits(8)));
    }

    static const Huffman& FixedLiterals() {
        static const Huffman table = [] {
            uint8_t lengths[288];
            for (int i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            Huffman h;
            Build(h, lengths, 288);
            return h;
        }();
        return table;
    }

    static const Huffman& FixedDistances() {
        static const Huffman table = [] {
            uint8_t lengths[32];
            std::fill(std::begin(lengths), std::end(lengths), uint8_t(5));
            Huffman h;
            Build(h, lengths, 32);
            return h;
        }();
        return table;
    }

    void Dynamic() {
        static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int literals = int(Bits(5)) + 257, distances = int(Bits(5)) + 1, codes = int(Bits(4)) + 4;
        if (literals > 286) Fail("3MF 壓縮資料損毀。");
        uint8_t codeLengths[19]{};
        for (int i = 0; i < codes; ++i) codeLengths[order[i]] = uint8_t(Bits(3));
        Huffman lengthsTable;
        Build(lengthsTable, codeLengths, 19);
        uint8_t lengths[286 + 32]{};
        for (int n = 0; n < literals + distances;) {
            const int symbol = Decode(lengthsTable);
            if (symbol < 16) {
                lengths[n++] = uint8_t(symbol);
                continue;
            }
            int repeat = 0;
            uint8_t fill = 0;
            if (symbol == 16) {
                if (n == 0) Fail("3MF 壓縮資料損毀。");
                fill = lengths[n - 1];
                repeat = 3 + int(Bits(2));
            } else if (symbol == 17) {
                repeat = 3 + int(Bits(3));
            } else {
                repeat = 11 + int(Bits(7));
            }
            if (n + repeat > literals + distances) Fail("3MF 壓縮資料損毀。");
            std::fill(lengths + n, lengths + n + repeat, fill);
            n += repeat;
        }
        Huffman literalTable, distanceTable;
        Build(literalTable, lengths, literals);
        Build(distanceTable, lengths + literals, distances);
        Block(literalTable, distanceTable);
    }

    void Block(const Huffman& literals, const Huffman& distances) {
        static constexpr uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t distanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                                      6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            const int symbol = Decode(literals);
            if (symbol < 256) {
                Grow(1);
                out.push_back(uint8_t(symbol));
                continue;
            }
            if (symbol == 256) return;
            const int li = symbol - 257;
            if (li >= 29) Fail("3MF 壓縮資料損毀。");
            const size_t len = lengthBase[li] + Bits(lengthExtra[li]);
            const int di = Decode(distances);
            if (di >= 30) Fail("3MF 壓縮資料損毀。");
            const size_t distance = distanceBase[di] + Bits(distanceExtra[di]);
            if (distance > out.size()) Fail("3MF 壓縮資料損毀。");
            Grow(len);
            const size_t from = out.size() - distance;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }

    void Grow(size_t extra) {
        if (extra > limit - out.size()) Fail("3MF 壓縮資料大小不符。");
    }

    const uint8_t* input;
    size_t length, position = 0;
    uint64_t buffer = 0;
    int count = 0, overrun = 0;
    std::vector<uint8_t>& out;
    size_t limit;
};

// ---- ZIP container --------------------------------------------------------------------------

constexpr string_view PlatePictures[] = {
    "metadata/plate_1.png", "metadata/plate_1_small.png", "metadata/top_1.png"
};
constexpr size_t MaxPictureBytes = 2 * 1024 * 1024, MaxPictureDirectory = 1024 * 1024;

class Zip {
public:
    explicit Zip(std::span<const char> bytes)
        : data(reinterpret_cast<const uint8_t*>(bytes.data())), size(bytes.size()) { Index(); }

    explicit Zip(IStream& source) : stream(&source) {
        STATSTG stat{};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) Fail("無法讀取 3MF 檔案大小。");
        size = stat.cbSize.QuadPart;
        Index();
    }

    bool Has(string_view name) const { return entries_.count(Key(name)) != 0; }

    std::vector<std::string> Names() const {
        std::vector<std::string> names;
        for (const auto& [name, entry] : entries_) names.push_back(name);
        return names;
    }

    std::string Read(string_view name) const {
        const auto found = entries_.find(Key(name));
        if (found == entries_.end()) Fail("3MF 缺少模型資料。");
        const Entry& entry = found->second;
        const uint64_t limit = stream ? MaxPictureBytes : MaxInflated;
        if (entry.unpacked > limit || (stream && entry.packed > limit))
            Fail("3MF 項目超過大小限制。");
        if (entry.flags & 1) Fail("3MF 使用了加密的 ZIP 項目。");
        std::vector<uint8_t> buffer;
        const auto* header = Range(entry.offset, 30, buffer);
        if (U32(header) != 0x04034b50) Fail("3MF ZIP 項目損毀。");
        const uint64_t headerSize = 30ull + U16(header + 26) + U16(header + 28);
        if (headerSize > size - entry.offset) Fail("3MF ZIP 項目不完整。");
        const auto* packed = Range(entry.offset + headerSize, entry.packed, buffer);
        if (entry.method == 0 && entry.packed == entry.unpacked)
            return std::string(reinterpret_cast<const char*>(packed), size_t(entry.packed));
        if (entry.method != 8) Fail("3MF 使用了不支援的壓縮方式。");
        std::vector<uint8_t> out;
        out.reserve(size_t(entry.unpacked));
        Inflater(packed, size_t(entry.packed), out, size_t(entry.unpacked)).Run();
        if (out.size() != entry.unpacked) Fail("3MF 壓縮資料大小不符。");
        return std::string(out.begin(), out.end());
    }

private:
    void Index() {
        if (size < 22) Fail("3MF 檔案不完整。");
        std::vector<uint8_t> tailBuffer, directoryBuffer;
        constexpr size_t FullTail = 22 + 65535 + 20;
        size_t tailSize = size_t(std::min<uint64_t>(size, stream ? 4096 : FullTail));
        const uint8_t* closing = nullptr;
        size_t end = 0;
        for (;;) {
            const auto* tail = Range(size - tailSize, tailSize, tailBuffer);
            for (end = tailSize - 22;; --end) {
                if (U32(tail + end) == 0x06054b50 && end + 22 + U16(tail + end + 20) == tailSize) {
                    closing = tail + end;
                    break;
                }
                if (end == 0) break;
            }
            if (closing) break;
            if (tailSize >= std::min<uint64_t>(size, FullTail))
                Fail("3MF 檔案不完整（找不到 ZIP 目錄）。");
            tailSize = size_t(std::min<uint64_t>(size, FullTail));
        }
        uint64_t entries = U16(closing + 10), directorySize = U32(closing + 12), directory = U32(closing + 16);
        // ZIP64 end-of-directory record, if the classic fields overflowed.
        if (entries == 0xFFFF || directory == 0xFFFFFFFF || directorySize == 0xFFFFFFFF) {
            const auto closingOffset = size - tailSize + end;
            if (closingOffset < 20) Fail("3MF ZIP64 目錄損毀。");
            const auto* locator = end >= 20 ? closing - 20 : Range(closingOffset - 20, 20, directoryBuffer);
            if (U32(locator) != 0x07064b50) Fail("3MF ZIP64 目錄損毀。");
            const auto* record = Range(U64(locator + 8), 56, directoryBuffer);
            if (U32(record) != 0x06064b50) Fail("3MF ZIP64 目錄損毀。");
            entries = U64(record + 32);
            directorySize = U64(record + 40);
            directory = U64(record + 48);
        }
        if (entries > directorySize / 46 || (stream && directorySize > MaxPictureDirectory))
            Fail("3MF ZIP 目錄超過限制。");
        if (entries == 0) return;
        const auto* listing = Range(directory, directorySize, directoryBuffer);
        size_t at = 0;
        for (uint64_t i = 0; i < entries; ++i) {
            if (directorySize - at < 46 || U32(listing + at) != 0x02014b50) Fail("3MF ZIP 目錄損毀。");
            const auto* row = listing + at;
            const size_t nameLength = U16(row + 28), extraLength = U16(row + 30), commentLength = U16(row + 32);
            const size_t rowSize = 46 + nameLength + extraLength + commentLength;
            if (rowSize > directorySize - at) Fail("3MF ZIP 目錄損毀。");
            at += rowSize;
            if (stream && (nameLength < 18 || nameLength > 32)) continue;
            const auto name = Key(string_view(reinterpret_cast<const char*>(row + 46), nameLength));
            if (stream && std::find(std::begin(PlatePictures), std::end(PlatePictures), name) == std::end(PlatePictures))
                continue;
            Entry entry;
            entry.flags = U16(row + 8);
            entry.method = U16(row + 10);
            entry.packed = U32(row + 20);
            entry.unpacked = U32(row + 24);
            entry.offset = U32(row + 42);
            // ZIP64 extra field: the 64-bit values follow in this order, each only if needed.
            const size_t extraEnd = 46 + nameLength + extraLength;
            for (size_t e = 46 + nameLength; e + 4 <= extraEnd;) {
                const uint16_t id = U16(row + e), length = U16(row + e + 2);
                if (length > extraEnd - e - 4) Fail("3MF ZIP 附加欄位損毀。");
                if (id == 1) {
                    size_t f = e + 4;
                    if (entry.unpacked == 0xFFFFFFFF && f + 8 <= e + 4 + length) { entry.unpacked = U64(row + f); f += 8; }
                    if (entry.packed == 0xFFFFFFFF && f + 8 <= e + 4 + length) { entry.packed = U64(row + f); f += 8; }
                    if (entry.offset == 0xFFFFFFFF && f + 8 <= e + 4 + length) entry.offset = U64(row + f);
                }
                e += 4 + length;
            }
            entries_[name] = entry;
        }
    }

    const uint8_t* Range(uint64_t offset, uint64_t count, std::vector<uint8_t>& buffer) const {
        if (offset > size || count > size - offset || offset > INT64_MAX || count > UINT32_MAX)
            Fail("3MF ZIP 項目不完整。");
        if (!stream) return data + size_t(offset);
        if (count == 0) return reinterpret_cast<const uint8_t*>("");
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr))) Fail("無法定位 3MF ZIP 項目。");
        buffer.resize(size_t(count));
        for (ULONG at = 0; at < count;) {
            const ULONG requested = static_cast<ULONG>(count - at);
            ULONG received{};
            if (FAILED(stream->Read(buffer.data() + at, requested, &received)) || !received || received > requested)
                Fail("無法讀取 3MF ZIP 項目。");
            at += received;
        }
        return buffer.data();
    }

    struct Entry {
        uint16_t method{}, flags{};
        uint64_t packed{}, unpacked{}, offset{};
    };
    static std::string Key(string_view name) {
        std::string key = Lower(name);
        std::replace(key.begin(), key.end(), '\\', '/');
        key.erase(0, key.find_first_not_of('/'));
        return key;
    }
    static uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    static uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    static uint64_t U64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

    const uint8_t* data{};
    IStream* stream{};
    uint64_t size{};
    std::map<std::string, Entry> entries_;
};

// ---- Minimal XML scanner (tags and attributes only) -----------------------------------------

struct Tag {
    string_view name;       // local name, namespace prefix removed
    string_view attributes;
    bool closing{}, selfClosing{};
};

class XmlScanner {
public:
    explicit XmlScanner(string_view source) : text(source) {}

    bool Next(Tag& tag) {
        for (;;) {
            const size_t open = text.find('<', position);
            if (open == string_view::npos) return false;
            position = open;
            const auto rest = text.substr(open);
            if (rest.starts_with("<?")) { Skip("?>"); continue; }
            if (rest.starts_with("<!--")) { Skip("-->"); continue; }
            if (rest.starts_with("<![CDATA[")) { Skip("]]>"); continue; }
            if (rest.starts_with("<!")) { Skip(">"); continue; }
            size_t at = open + 1;
            tag = {};
            if (at < text.size() && text[at] == '/') { tag.closing = true; ++at; }
            const size_t nameStart = at;
            while (at < text.size() && !IsSpace(text[at]) && text[at] != '>' && text[at] != '/') ++at;
            tag.name = text.substr(nameStart, at - nameStart);
            if (const size_t colon = tag.name.find(':'); colon != string_view::npos) tag.name.remove_prefix(colon + 1);
            const size_t attributesStart = at;
            char quote = 0;
            while (at < text.size() && (quote || text[at] != '>')) {
                if (quote) { if (text[at] == quote) quote = 0; }
                else if (text[at] == '"' || text[at] == '\'') quote = text[at];
                ++at;
            }
            if (at >= text.size()) Fail("3MF 模型 XML 不完整。");
            size_t attributesEnd = at;
            if (attributesEnd > attributesStart && text[attributesEnd - 1] == '/') { tag.selfClosing = true; --attributesEnd; }
            tag.attributes = text.substr(attributesStart, attributesEnd - attributesStart);
            position = at + 1;
            return true;
        }
    }

    static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

private:
    void Skip(string_view terminator) {
        const size_t end = text.find(terminator, position);
        position = end == string_view::npos ? text.size() : end + terminator.size();
    }
    string_view text;
    size_t position = 0;
};

// Calls f(localName, rawValue) for each attribute.
template <class F> void ForEachAttribute(string_view attributes, F&& f) {
    size_t at = 0;
    while (at < attributes.size()) {
        while (at < attributes.size() && XmlScanner::IsSpace(attributes[at])) ++at;
        const size_t nameStart = at;
        while (at < attributes.size() && attributes[at] != '=' && !XmlScanner::IsSpace(attributes[at])) ++at;
        string_view name = attributes.substr(nameStart, at - nameStart);
        while (at < attributes.size() && attributes[at] != '"' && attributes[at] != '\'') ++at;
        if (at >= attributes.size()) return;
        const char quote = attributes[at++];
        const size_t valueStart = at;
        while (at < attributes.size() && attributes[at] != quote) ++at;
        const string_view value = attributes.substr(valueStart, at - valueStart);
        ++at;
        if (const size_t colon = name.find(':'); colon != string_view::npos) name.remove_prefix(colon + 1);
        if (!name.empty()) f(name, value);
    }
}

std::string Unescape(string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') { out += raw[i]; continue; }
        const size_t end = raw.find(';', i);
        if (end == string_view::npos) { out += raw[i]; continue; }
        const string_view entity = raw.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity.starts_with("#")) {
            uint32_t code = 0;
            const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            const auto digits = entity.substr(hex ? 2 : 1);
            std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
            if (code < 0x80) out += char(code);
            else if (code < 0x800) { out += char(0xC0 | (code >> 6)); out += char(0x80 | (code & 0x3F)); }
            else if (code < 0x10000) { out += char(0xE0 | (code >> 12)); out += char(0x80 | ((code >> 6) & 0x3F)); out += char(0x80 | (code & 0x3F)); }
            else { out += char(0xF0 | (code >> 18)); out += char(0x80 | ((code >> 12) & 0x3F)); out += char(0x80 | ((code >> 6) & 0x3F)); out += char(0x80 | (code & 0x3F)); }
        } else {
            out.append(raw.substr(i, end - i + 1));
        }
        i = end;
    }
    return out;
}

template <class T> bool Number(string_view text, T& value) {
    while (!text.empty() && (XmlScanner::IsSpace(text.front()) || text.front() == '+')) text.remove_prefix(1);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end != text.data();
}

// ---- 3MF ------------------------------------------------------------------------------------

// Affine transform for row vectors: p' = p * M, stored as m00 m01 m02 m10 ... m32 (3MF order).
struct Affine {
    double m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    stl::Point Apply(stl::Point p) const {
        return {float(p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9]),
                float(p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10]),
                float(p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11])};
    }
    // First this, then `next`.
    Affine Then(const Affine& next) const {
        Affine r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 3; ++col) {
                double sum = row == 3 ? next.m[9 + col] : 0;
                for (int k = 0; k < 3; ++k) sum += m[row * 3 + k] * next.m[k * 3 + col];
                r.m[row * 3 + col] = sum;
            }
        }
        return r;
    }
    static Affine Parse(string_view text) {
        Affine a;
        size_t at = 0;
        for (double& value : a.m) {
            while (at < text.size() && XmlScanner::IsSpace(text[at])) ++at;
            const auto [end, error] = std::from_chars(text.data() + at, text.data() + text.size(), value);
            if (error != std::errc{}) return Affine{};
            at = size_t(end - text.data());
        }
        return a;
    }
};

struct Component {
    std::string path;
    uint32_t id{};
    Affine transform;
};

struct Object {
    std::vector<stl::Point> vertices;
    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<Component> components;
    std::string name;
    bool support{};
};

struct BuildItem {
    std::string path;
    uint32_t id{};
    Affine transform;
};

struct ModelFile {
    std::unordered_map<uint32_t, Object> objects;
    std::vector<BuildItem> items;
    std::string unit;
    bool color{};
};

bool IsColorElement(string_view name) {
    return name == "basematerials" || name == "colorgroup" || name == "texture2dgroup" ||
           name == "texture2d" || name == "multiproperties" || name == "compositematerials" ||
           name == "pbmetallicdisplayproperties" || name == "pbspeculardisplayproperties";
}

ModelFile ParseModelFile(const std::string& xml, const std::string& selfPath) {
    ModelFile file;
    XmlScanner scanner(xml);
    Tag tag;
    Object* current = nullptr;
    uint32_t currentId = 0;
    bool inBuild = false;
    while (scanner.Next(tag)) {
        const auto name = tag.name;
        if (tag.closing) {
            if (name == "object") current = nullptr;
            if (name == "build") inBuild = false;
            continue;
        }
        if (name == "vertex" && current) {
            stl::Point p{};
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "x") Number(value, p.x);
                else if (key == "y") Number(value, p.y);
                else if (key == "z") Number(value, p.z);
            });
            current->vertices.push_back(p);
        } else if (name == "triangle" && current) {
            std::array<uint32_t, 3> t{};
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "v1") Number(value, t[0]);
                else if (key == "v2") Number(value, t[1]);
                else if (key == "v3") Number(value, t[2]);
                else if (key == "pid" || key == "paint_color" || key == "mmu_segmentation") file.color = true;
            });
            current->triangles.push_back(t);
        } else if (name == "component" && current) {
            Component c{selfPath};
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "objectid") Number(value, c.id);
                else if (key == "transform") c.transform = Affine::Parse(value);
                else if (key == "path") c.path = Unescape(value);
            });
            current->components.push_back(std::move(c));
        } else if (name == "object") {
            Object object;
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "id") Number(value, currentId);
                else if (key == "name") object.name = Unescape(value);
                else if (key == "type") object.support = value == "support" || value == "other";
                else if (key == "pid") file.color = true;
            });
            current = &(file.objects[currentId] = std::move(object));
            if (tag.selfClosing) current = nullptr;
        } else if (name == "build") {
            inBuild = !tag.selfClosing;
        } else if (name == "item" && inBuild) {
            BuildItem item{selfPath};
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "objectid") Number(value, item.id);
                else if (key == "transform") item.transform = Affine::Parse(value);
                else if (key == "path") item.path = Unescape(value);
            });
            file.items.push_back(std::move(item));
        } else if (name == "model") {
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "unit") file.unit = std::string(value);
            });
        } else if (IsColorElement(name)) {
            file.color = true;
        }
    }
    return file;
}

std::wstring UnitLabel(string_view unit) {
    if (unit == "micron") return L" µm";
    if (unit == "centimeter") return L" cm";
    if (unit == "inch") return L" in";
    if (unit == "foot") return L" ft";
    if (unit == "meter") return L" m";
    return L" mm";   // 3MF's default unit is the millimetre
}

// Bambu Studio keeps object names and filament colours in Metadata/*.config files.
std::map<uint32_t, std::string> BambuNames(const Zip& zip) {
    std::map<uint32_t, std::string> names;
    if (!zip.Has("Metadata/model_settings.config")) return names;
    const auto xml = zip.Read("Metadata/model_settings.config");
    XmlScanner scanner(xml);
    Tag tag;
    int depth = 0;
    uint32_t object = 0;
    while (scanner.Next(tag)) {
        if (tag.name == "object" && !tag.closing) {
            ForEachAttribute(tag.attributes, [&](string_view key, string_view value) {
                if (key == "id") Number(value, object);
            });
            depth = tag.selfClosing ? 0 : 1;
        } else if (tag.name == "object" && tag.closing) {
            depth = 0;
        } else if (tag.name == "part" && !tag.closing && !tag.selfClosing) {
            ++depth;
        } else if (tag.name == "part" && tag.closing) {
            --depth;
        } else if (tag.name == "metadata" && depth == 1) {
            std::string key, value;
            ForEachAttribute(tag.attributes, [&](string_view k, string_view v) {
                if (k == "key") key = std::string(v);
                else if (k == "value") value = Unescape(v);
            });
            if (key == "name" && !names.count(object)) names[object] = value;
        }
    }
    return names;
}

bool BambuMultiColor(const Zip& zip) {
    if (!zip.Has("Metadata/project_settings.config")) return false;
    const auto json = zip.Read("Metadata/project_settings.config");
    const size_t key = json.find("\"filament_colour\"");
    if (key == std::string::npos) return false;
    const size_t open = json.find('[', key), close = json.find(']', key);
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    std::set<std::string> colors;
    for (size_t at = open; (at = json.find('"', at + 1)) < close;) {
        const size_t end = json.find('"', at + 1);
        if (end == std::string::npos || end > close) break;
        colors.insert(Lower(string_view(json).substr(at + 1, end - at - 1)));
        at = end;
    }
    return colors.size() > 1;
}

Model Load3mf(std::span<const char> bytes) {
    const Zip zip(bytes);
    Model result;
    result.format = L"3MF";
    // The package relationships name the root model part.
    std::string root = "/3D/3dmodel.model";
    if (zip.Has("_rels/.rels")) {
        const auto rels = zip.Read("_rels/.rels");
        XmlScanner scanner(rels);
        Tag tag;
        while (scanner.Next(tag)) {
            if (tag.name != "Relationship" || tag.closing) continue;
            std::string type, target;
            ForEachAttribute(tag.attributes, [&](string_view k, string_view v) {
                if (k == "Type") type = std::string(v);
                else if (k == "Target") target = Unescape(v);
            });
            if (type.ends_with("/3dmodel") && !target.empty()) root = target;
        }
    }
    if (!root.starts_with('/')) root.insert(root.begin(), '/');

    std::map<std::string, ModelFile> files;
    const auto file = [&](const std::string& path) -> ModelFile& {
        const std::string key = Lower(path);
        auto found = files.find(key);
        if (found == files.end()) {
            found = files.emplace(key, ParseModelFile(zip.Read(path), path)).first;
            result.hasColor = result.hasColor || found->second.color;
        }
        return found->second;
    };
    const ModelFile& main = file(root);
    result.unit = UnitLabel(main.unit);
    const auto bambuNames = BambuNames(zip);
    result.hasColor = result.hasColor || BambuMultiColor(zip);
    // Texture or colour resources anywhere in the package also count.
    for (const auto& name : zip.Names()) {
        if (name.starts_with("3d/textures/") || name.starts_with("3d/texture/")) result.hasColor = true;
    }

    std::function<void(const std::string&, uint32_t, const Affine&, int)> emit =
        [&](const std::string& path, uint32_t id, const Affine& transform, int depth) {
            if (depth > 32) Fail("3MF 元件巢狀層數過深。");
            ModelFile& owner = file(path);
            const auto found = owner.objects.find(id);
            if (found == owner.objects.end()) Fail("3MF 參照了不存在的物件。");
            const Object& object = found->second;
            if (object.support) return;
            for (const auto& t : object.triangles) {
                if (t[0] >= object.vertices.size() || t[1] >= object.vertices.size() || t[2] >= object.vertices.size())
                    Fail("3MF 三角面參照了不存在的頂點。");
                AddTriangle(result.mesh, transform.Apply(object.vertices[t[0]]), transform.Apply(object.vertices[t[1]]),
                            transform.Apply(object.vertices[t[2]]));
            }
            for (const auto& c : object.components)
                emit(c.path.empty() ? path : c.path, c.id, c.transform.Then(transform), depth + 1);
        };

    std::map<std::string, int> seen;
    for (const auto& item : main.items) {
        const size_t first = result.mesh.triangles.size();
        const std::string path = item.path.empty() ? root : item.path;
        emit(path, item.id, item.transform, 0);
        const size_t count = result.mesh.triangles.size() - first;
        if (!count) continue;
        std::string name;
        if (const auto found = file(path).objects.find(item.id); found != file(path).objects.end()) name = found->second.name;
        if (name.empty()) if (const auto b = bambuNames.find(item.id); b != bambuNames.end()) name = b->second;
        std::wstring label = name.empty() ? L"物件 " + std::to_wstring(result.parts.size() + 1) : Widen(name);
        if (const int n = ++seen[name]; n > 1 && !name.empty()) label += L" (" + std::to_wstring(n) + L")";
        result.parts.push_back({label, first, count});
    }
    if (result.mesh.triangles.empty()) Fail("這個 3MF 沒有可顯示的模型。");
    return result;
}

// ---- OBJ ------------------------------------------------------------------------------------

Model LoadObj(std::span<const char> bytes) {
    Model result;
    result.format = L"OBJ";
    std::vector<stl::Point> vertices;
    std::wstring partName;
    size_t partStart = 0;
    const auto closePart = [&] {
        const size_t count = result.mesh.triangles.size() - partStart;
        if (count) {
            result.parts.push_back({partName.empty() ? L"物件 " + std::to_wstring(result.parts.size() + 1) : partName,
                                    partStart, count});
        }
        partStart = result.mesh.triangles.size();
    };
    string_view text(bytes.data(), bytes.size());
    std::vector<uint32_t> face;
    while (!text.empty()) {
        const size_t end = std::min(text.find('\n'), text.size());
        string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        while (!line.empty() && (line.back() == '\r' || XmlScanner::IsSpace(line.back()))) line.remove_suffix(1);
        while (!line.empty() && XmlScanner::IsSpace(line.front())) line.remove_prefix(1);
        if (line.size() < 2 || line[0] == '#') continue;
        const auto next = [&](string_view& rest) {
            while (!rest.empty() && XmlScanner::IsSpace(rest.front())) rest.remove_prefix(1);
            size_t n = 0;
            while (n < rest.size() && !XmlScanner::IsSpace(rest[n])) ++n;
            const auto token = rest.substr(0, n);
            rest.remove_prefix(n);
            return token;
        };
        string_view rest = line;
        const auto keyword = next(rest);
        if (keyword == "v") {
            double values[7]{};
            int count = 0;
            for (string_view token; count < 7 && !(token = next(rest)).empty(); ++count)
                if (!Number(token, values[count])) Fail("OBJ 頂點座標無效。");
            if (count < 3) Fail("OBJ 頂點座標不完整。");
            if (count >= 6) result.hasColor = true;   // "v x y z r g b" vertex colours
            // OBJ is Y-up; the viewer (like STL and 3MF) is Z-up.
            vertices.push_back({float(values[0]), float(-values[2]), float(values[1])});
        } else if (keyword == "f") {
            face.clear();
            for (string_view token; !(token = next(rest)).empty();) {
                long long index = 0;
                const auto slash = token.find('/');
                if (!Number(token.substr(0, slash), index) || index == 0) Fail("OBJ 面的頂點索引無效。");
                index = index < 0 ? static_cast<long long>(vertices.size()) + index : index - 1;
                if (index < 0 || size_t(index) >= vertices.size()) Fail("OBJ 面參照了不存在的頂點。");
                face.push_back(uint32_t(index));
            }
            for (size_t i = 2; i < face.size(); ++i)   // fan-triangulate polygons
                AddTriangle(result.mesh, vertices[face[0]], vertices[face[i - 1]], vertices[face[i]]);
        } else if (keyword == "o" || keyword == "g") {
            closePart();
            while (!rest.empty() && XmlScanner::IsSpace(rest.front())) rest.remove_prefix(1);
            partName = Widen(rest);
        } else if (keyword == "usemtl" || keyword == "mtllib") {
            result.hasColor = true;
        }
    }
    closePart();
    if (result.mesh.triangles.empty()) Fail("這個 OBJ 沒有三角面。");
    return result;
}

// ---- PLY ------------------------------------------------------------------------------------

Model LoadPly(std::span<const char> bytes) {
    Model result;
    result.format = L"PLY";
    string_view text(bytes.data(), bytes.size());
    const size_t headerEnd = text.find("end_header");
    if (!text.starts_with("ply") || headerEnd == string_view::npos) Fail("PLY 標頭不完整。");
    size_t bodyStart = text.find('\n', headerEnd);
    if (bodyStart == string_view::npos) Fail("PLY 標頭不完整。");
    ++bodyStart;

    enum class Encoding { Ascii, Little, Big } encoding = Encoding::Ascii;
    struct Property { std::string name; int type = 0, countType = 0; bool list{}; };
    struct Element { std::string name; size_t count{}; std::vector<Property> properties; };
    std::vector<Element> elements;
    const auto typeSize = [](string_view t) -> int {
        if (t == "char" || t == "int8" || t == "uchar" || t == "uint8") return t[0] == 'u' ? 2 : 1;   // 1/2 = s8/u8
        if (t == "short" || t == "int16") return 3;
        if (t == "ushort" || t == "uint16") return 4;
        if (t == "int" || t == "int32") return 5;
        if (t == "uint" || t == "uint32") return 6;
        if (t == "float" || t == "float32") return 7;
        if (t == "double" || t == "float64") return 8;
        Fail("PLY 屬性型別不支援。");
    };
    string_view header = text.substr(0, headerEnd);
    while (!header.empty()) {
        const size_t end = std::min(header.find('\n'), header.size());
        string_view line = header.substr(0, end);
        header.remove_prefix(std::min(end + 1, header.size()));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        std::vector<string_view> words;
        for (size_t at = 0; at < line.size();) {
            while (at < line.size() && line[at] == ' ') ++at;
            size_t n = at;
            while (n < line.size() && line[n] != ' ') ++n;
            if (n > at) words.push_back(line.substr(at, n - at));
            at = n;
        }
        if (words.empty()) continue;
        if (words[0] == "format" && words.size() >= 2) {
            encoding = words[1] == "binary_little_endian" ? Encoding::Little
                     : words[1] == "binary_big_endian"    ? Encoding::Big : Encoding::Ascii;
        } else if (words[0] == "element" && words.size() >= 3) {
            Element e{std::string(words[1])};
            Number(words[2], e.count);
            elements.push_back(std::move(e));
        } else if (words[0] == "property" && !elements.empty()) {
            Property p;
            if (words.size() >= 5 && words[1] == "list") {
                p.list = true;
                p.countType = typeSize(words[2]);
                p.type = typeSize(words[3]);
                p.name = std::string(words[4]);
            } else if (words.size() >= 3) {
                p.type = typeSize(words[1]);
                p.name = std::string(words[2]);
            }
            const auto lower = Lower(p.name);
            if (elements.back().name == "vertex" &&
                (lower == "red" || lower == "green" || lower == "blue" || lower == "r" || lower == "diffuse_red"))
                result.hasColor = true;
            if (elements.back().name == "face" && (lower == "red" || lower == "texcoord" || lower == "texnumber"))
                result.hasColor = true;
            elements.back().properties.push_back(std::move(p));
        } else if (words[0] == "comment" && words.size() >= 2 && words[1] == "TextureFile") {
            result.hasColor = true;
        }
    }

    const char* at = text.data() + bodyStart;
    const char* const end = text.data() + text.size();
    const auto readBinary = [&](int type) -> double {
        static constexpr int sizes[] = {0, 1, 1, 2, 2, 4, 4, 4, 8};
        const int n = sizes[type];
        if (end - at < n) Fail("PLY 資料不完整。");
        unsigned char raw[8];
        std::memcpy(raw, at, size_t(n));
        at += n;
        if (encoding == Encoding::Big) std::reverse(raw, raw + n);
        switch (type) {
        case 1: { int8_t v; std::memcpy(&v, raw, 1); return v; }
        case 2: return raw[0];
        case 3: { int16_t v; std::memcpy(&v, raw, 2); return v; }
        case 4: { uint16_t v; std::memcpy(&v, raw, 2); return v; }
        case 5: { int32_t v; std::memcpy(&v, raw, 4); return v; }
        case 6: { uint32_t v; std::memcpy(&v, raw, 4); return v; }
        case 7: { float v; std::memcpy(&v, raw, 4); return v; }
        default: { double v; std::memcpy(&v, raw, 8); return v; }
        }
    };
    const auto readAscii = [&]() -> double {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')) ++at;
        double v{};
        const auto [next, error] = std::from_chars(at, end, v);
        if (error != std::errc{}) Fail("PLY 資料無效。");
        at = next;
        return v;
    };
    const auto read = [&](int type) { return encoding == Encoding::Ascii ? readAscii() : readBinary(type); };

    std::vector<stl::Point> vertices;
    std::vector<uint32_t> face;
    for (const auto& element : elements) {
        const bool isVertex = element.name == "vertex", isFace = element.name == "face";
        if (isVertex) vertices.reserve(std::min<size_t>(element.count, 8'000'000));
        for (size_t i = 0; i < element.count; ++i) {
            stl::Point p{};
            face.clear();
            for (const auto& property : element.properties) {
                if (property.list) {
                    const auto n = size_t(read(property.countType));
                    const bool indices = isFace && (property.name == "vertex_indices" || property.name == "vertex_index");
                    for (size_t k = 0; k < n; ++k) {
                        const double v = read(property.type);
                        if (indices) face.push_back(uint32_t(v));
                    }
                } else {
                    const double v = read(property.type);
                    if (isVertex) {
                        if (property.name == "x") p.x = float(v);
                        else if (property.name == "y") p.y = float(v);
                        else if (property.name == "z") p.z = float(v);
                    }
                }
            }
            if (isVertex) vertices.push_back(p);
            if (isFace) {
                for (const auto index : face)
                    if (index >= vertices.size()) Fail("PLY 面參照了不存在的頂點。");
                for (size_t k = 2; k < face.size(); ++k)
                    AddTriangle(result.mesh, vertices[face[0]], vertices[face[k - 1]], vertices[face[k]]);
            }
        }
    }
    if (result.mesh.triangles.empty()) Fail("這個 PLY 沒有三角面。");
    return result;
}

// ---- STL ------------------------------------------------------------------------------------

Model LoadStl(std::span<const char> bytes) {
    Model result;
    result.format = L"STL";
    stl::StlInfo info;
    result.mesh = stl::Parse(bytes, &info);   // already normalised
    result.hasColor = info.color;
    if (info.solids.size() > 1) {
        for (size_t i = 0; i < info.solids.size(); ++i) {
            const size_t first = info.solids[i].first;
            const size_t last = i + 1 < info.solids.size() ? info.solids[i + 1].first : result.mesh.triangles.size();
            if (last <= first) continue;
            const auto& name = info.solids[i].name;
            result.parts.push_back({name.empty() ? L"物件 " + std::to_wstring(result.parts.size() + 1) : Widen(name),
                                    first, last - first});
        }
    }
    return result;
}

std::string Extension(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::string ext;
    for (const wchar_t c : path.substr(dot + 1)) ext += char(c < 128 ? std::tolower(int(c)) : '?');
    return ext;
}
}

std::string ReadFirstPlateImage(IStream& stream) {
    const Zip zip(stream);
    for (const auto name : PlatePictures) {
        if (!zip.Has(name)) continue;
        try {
            auto bytes = zip.Read(name);
            if (string_view(bytes).starts_with("\x89PNG\r\n\x1a\n")) return bytes;
        } catch (const std::runtime_error&) {
            // An unusable picture can still have a smaller first-plate alternative.
        }
    }
    return {};
}

Model Load(const std::wstring& path, std::span<const char> bytes) {
    if (bytes.empty()) throw std::runtime_error("這個檔案是空的。");
    const string_view head(bytes.data(), std::min<size_t>(bytes.size(), 64));
    std::string ext = Extension(path);
    if (ext != "stl" && ext != "3mf" && ext != "obj" && ext != "ply") {
        ext = head.starts_with("PK\x03\x04") ? "3mf" : head.starts_with("ply") ? "ply" : "stl";
    }
    Model result = ext == "3mf" ? Load3mf(bytes) : ext == "obj" ? LoadObj(bytes) : ext == "ply" ? LoadPly(bytes)
                                                                                     : LoadStl(bytes);
    if (result.format != L"STL") stl::Normalize(result.mesh);
    if (result.parts.empty()) result.parts.push_back({L"模型", 0, result.mesh.triangles.size()});
    const auto& mesh = result.mesh;
    for (auto& part : result.parts) {
        const float inf = std::numeric_limits<float>::infinity();
        stl::Point lo{inf, inf, inf}, hi{-inf, -inf, -inf};
        for (size_t i = part.first; i < part.first + part.count; ++i) {
            for (const auto& p : {mesh.triangles[i].a, mesh.triangles[i].b, mesh.triangles[i].c}) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        }
        part.min = lo;
        part.max = hi;
        part.size[0] = (hi.x - lo.x) * mesh.diameter;
        part.size[1] = (hi.y - lo.y) * mesh.diameter;
        part.size[2] = (hi.z - lo.z) * mesh.diameter;
    }
    return result;
}
}
