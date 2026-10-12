#include "dtb.h"

#include <cctype>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace rb3pc {

namespace {

bool HasText(uint32_t type) {
  switch (type) {
    case DtbNode::kVar:
    case DtbNode::kFunc:
    case DtbNode::kObject:
    case DtbNode::kSymbol:
    case DtbNode::kIfdef:
    case DtbNode::kString:
    case DtbNode::kGlob:
    case DtbNode::kDefine:
    case DtbNode::kInclude:
    case DtbNode::kMerge:
    case DtbNode::kIfndef:
    case DtbNode::kUndef:
      return true;
    default:
      return false;
  }
}

bool HasArray(uint32_t type) {
  return type == DtbNode::kArray || type == DtbNode::kCommand || type == DtbNode::kProperty;
}

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
  void Need(size_t n) const {
    if (pos_ + n > d_.size()) throw std::out_of_range("dtb");
  }
  uint16_t U16() {
    Need(2);
    uint16_t v = uint16_t(d_[pos_] | d_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
  }
  uint32_t U32() {
    Need(4);
    uint32_t v = d_[pos_] | d_[pos_ + 1] << 8 | d_[pos_ + 2] << 16 | uint32_t(d_[pos_ + 3]) << 24;
    pos_ += 4;
    return v;
  }
  std::string Text() {
    uint32_t n = U32();
    Need(n);
    std::string s(reinterpret_cast<const char*>(&d_[pos_]), n);
    pos_ += n;
    return s;
  }
  DtbArray Array(int depth = 0) {
    if (depth > 256) throw std::out_of_range("dtb nesting");
    DtbArray a;
    uint16_t count = U16();
    a.line = U16();
    a.reserved = U16();
    a.nodes.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
      DtbNode n;
      n.type = U32();
      if (HasText(n.type)) {
        n.text = Text();
      } else if (HasArray(n.type)) {
        n.array = std::make_shared<DtbArray>(Array(depth + 1));
      } else if (n.type == DtbNode::kFloat) {
        uint32_t bits = U32();
        std::memcpy(&n.real, &bits, 4);
      } else {
        n.integer = U32();
      }
      a.nodes.push_back(std::move(n));
    }
    return a;
  }
  size_t pos() const { return pos_; }
  void Skip(size_t n) { pos_ += n; }

 private:
  const std::vector<uint8_t>& d_;
  size_t pos_ = 0;
};

void WriteU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

void WriteArray(std::vector<uint8_t>& out, const DtbArray& a) {
  WriteU16(out, uint16_t(a.nodes.size()));
  WriteU16(out, a.line);
  WriteU16(out, a.reserved);
  for (const auto& n : a.nodes) {
    WriteU32(out, n.type);
    if (HasText(n.type)) {
      WriteU32(out, uint32_t(n.text.size()));
      out.insert(out.end(), n.text.begin(), n.text.end());
    } else if (HasArray(n.type)) {
      WriteArray(out, n.array ? *n.array : DtbArray{});
    } else if (n.type == DtbNode::kFloat) {
      uint32_t bits;
      std::memcpy(&bits, &n.real, 4);
      WriteU32(out, bits);
    } else {
      WriteU32(out, n.integer);
    }
  }
}

}  // namespace

std::optional<DtbArray> ParseDtb(const std::vector<uint8_t>& plain) {
  if (plain.empty() || plain[0] != 1) return std::nullopt;
  try {
    Reader r(plain);
    r.Skip(1);
    DtbArray root = r.Array();
    if (r.pos() != plain.size()) return std::nullopt;
    return root;
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
}

std::vector<uint8_t> SerializeDtb(const DtbArray& root) {
  std::vector<uint8_t> out{1};
  WriteArray(out, root);
  return out;
}

std::optional<std::vector<DtbNode>> ParseDta(std::string_view text) {
  size_t pos = 0;
  // Parses nodes until `close` (or the end, for the top level).
  auto parse = [&](auto& self, char close) -> std::optional<std::vector<DtbNode>> {
    std::vector<DtbNode> nodes;
    while (true) {
      while (pos < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[pos]))) {
          ++pos;
        } else if (text[pos] == ';') {
          while (pos < text.size() && text[pos] != '\n') ++pos;
        } else {
          break;
        }
      }
      if (pos >= text.size()) {
        if (close) return std::nullopt;
        return nodes;
      }
      char c = text[pos];
      if (c == ')' || c == '}' || c == ']') {
        if (c != close) return std::nullopt;
        ++pos;
        return nodes;
      }
      DtbNode n;
      if (c == '(' || c == '{' || c == '[') {
        ++pos;
        char match = c == '(' ? ')' : c == '{' ? '}' : ']';
        auto inner = self(self, match);
        if (!inner) return std::nullopt;
        n.type = c == '(' ? DtbNode::kArray : c == '{' ? DtbNode::kCommand : DtbNode::kProperty;
        n.array = std::make_shared<DtbArray>();
        n.array->nodes = std::move(*inner);
      } else if (c == '"') {
        size_t end = text.find('"', pos + 1);
        if (end == std::string_view::npos) return std::nullopt;
        n.type = DtbNode::kString;
        n.text = std::string(text.substr(pos + 1, end - pos - 1));
        pos = end + 1;
      } else {
        size_t end = pos;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
               !std::strchr("(){}[];\"", text[end])) {
          ++end;
        }
        std::string_view token = text.substr(pos, end - pos);
        pos = end;
        int32_t i;
        float f;
        if (token[0] == '$') {
          n.type = DtbNode::kVar;
          n.text = std::string(token.substr(1));
        } else if (auto r = std::from_chars(token.data(), token.data() + token.size(), i);
                   r.ec == std::errc() && r.ptr == token.data() + token.size()) {
          n.type = DtbNode::kInt;
          n.integer = uint32_t(i);
        } else if (auto r = std::from_chars(token.data(), token.data() + token.size(), f);
                   r.ec == std::errc() && r.ptr == token.data() + token.size()) {
          n.type = DtbNode::kFloat;
          n.real = f;
        } else {
          n.type = DtbNode::kSymbol;
          n.text = std::string(token);
        }
      }
      nodes.push_back(std::move(n));
    }
  };
  return parse(parse, 0);
}

DtbArray* FindArray(DtbArray& root, const std::vector<std::string>& prefix) {
  if (root.nodes.size() >= prefix.size()) {
    bool match = true;
    for (size_t i = 0; i < prefix.size() && match; ++i) {
      const DtbNode& n = root.nodes[i];
      const std::string& want = prefix[i];
      if (!want.empty() && want[0] == '$') {
        match = n.type == DtbNode::kVar && n.text == want.substr(1);
      } else {
        match = n.IsSymbol(want);
      }
    }
    if (match) return &root;
  }
  for (auto& n : root.nodes) {
    if (n.array) {
      if (DtbArray* found = FindArray(*n.array, prefix)) return found;
    }
  }
  return nullptr;
}

}  // namespace rb3pc
