// Decrypted .dtb read/edit/write, plus a small DTA text parser for patches.
// Format (little-endian): u8 1, then the root array. An array is u16 node
// count, u16 line, u16 0, then its nodes; a node is u32 type, then a value:
// u32 for ints (stored as type 0) and #else/#endif, f32 for floats, u32
// length + chars for symbols, variables, strings and directives, an array
// for (), {} and [].

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rb3pc {

struct DtbArray;

struct DtbNode {
  enum Type : uint32_t {
    kInt = 0,  // DataNode::Save writes kDataInt as 0
    kFloat = 1,
    kVar = 2,
    kFunc = 3,
    kObject = 4,
    kSymbol = 5,
    kIntAlt = 6,
    kIfdef = 7,
    kElse = 8,
    kEndif = 9,
    kArray = 16,
    kCommand = 17,
    kString = 18,
    kProperty = 19,
    kGlob = 20,
    kDefine = 32,
    kInclude = 33,
    kMerge = 34,
    kIfndef = 35,
    kAutorun = 36,
    kUndef = 37,
  };

  uint32_t type = kInt;
  uint32_t integer = 0;
  float real = 0;
  std::string text;                 // symbol, variable, string, directive
  std::shared_ptr<DtbArray> array;  // (), {}, []

  bool IsSymbol(std::string_view s) const { return type == kSymbol && text == s; }
  bool IsArray() const { return array != nullptr; }
};

struct DtbArray {
  uint16_t line = 0;
  uint16_t reserved = 0;
  std::vector<DtbNode> nodes;
};

std::optional<DtbArray> ParseDtb(const std::vector<uint8_t>& plain);
std::vector<uint8_t> SerializeDtb(const DtbArray& root);

// "{set $x 1} {foo bar}"; ';' comments.
std::optional<std::vector<DtbNode>> ParseDta(std::string_view text);

// Depth-first, by leading symbols, e.g. {"set", "$first_screen"}.
DtbArray* FindArray(DtbArray& root, const std::vector<std::string>& prefix);

}  // namespace rb3pc
