// rb3pc-dx: stand-in for RB3Enhanced's script side. DX gates on #ifdef RB3E
// and calls rb3e_* functions (song metadata, DELETE SONG CACHE, REBOOT GAME).
// Defines RB3E/RB3E_HAS_VERSION with OptionStr("define") in PreInitSystem and
// registers only the functions DX calls in gDataFuncs after DataInitFuncs
// (cf. RB3Enhanced DTAFunctions.c). Addresses are DX's TU5 executable's.

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "deluxe.h"
#include "exe.h"

REXCVAR_DECLARE(bool, deluxe_rb3e_functions);  // src/deluxe.cpp

REX_EXTERN(__imp__sub_827C0EC8);  // OptionStr(const char* option, const char* default)
REX_EXTERN(__imp__sub_82763A00);  // DataInitFuncs()
REX_EXTERN(sub_827C0728);         // Symbol::Symbol(Symbol* this, const char*)
REX_EXTERN(sub_82359F28);         // gDataFuncs' operator[](Symbol*) -> DataFunc* slot
REX_EXTERN(sub_8274AE98);         // DataNode::Evaluate(DataNode* this) -> DataNode*
REX_EXTERN(sub_827A8E30);         // BandSongMgr::Data(int song_id) -> BandSongMetadata*

namespace {

constexpr uint32_t kDataFuncs = 0x82E05D30;  // gDataFuncs
constexpr uint32_t kTheSongMgr = 0x82DFE7B4;

// DataNode types (Xbox).
constexpr uint32_t kInt = 0, kFloat = 1, kSymbol = 5, kString = 18;

// BandSongMetadata: String is {vtable, length, char* text}.
constexpr uint32_t kMetaOrigin = 0x38, kMetaTitle = 0x4C, kMetaArtist = 0x58, kMetaAlbum = 0x64,
                   kMetaGenre = 0x80;
constexpr uint32_t kStringText = 8;

constexpr const char* kDefines[] = {"RB3E", "RB3E_HAS_VERSION"};

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + addr, &value, 4);
}
std::string_view GuestString(const uint8_t* base, uint32_t addr) {
  if (!addr) return {};
  return reinterpret_cast<const char*>(base + addr);
}

bool Enabled() {
  return rb3pc::DeluxeEnabled() && REXCVAR_GET(deluxe_rb3e_functions);
}

struct GuestCall {
  GuestCall(PPCContext& ctx, uint8_t* base) : ctx(ctx), base(base), saved(ctx) {
    ctx.r1.u64 = (saved.r1.u32 - 0x400) & ~0xFu;
    scratch = ctx.r1.u32 + 0x100;  // above the callee's linkage area
  }
  ~GuestCall() { ctx = saved; }

  uint32_t Symbol(std::string_view text) {
    text = text.substr(0, 0x200);
    std::memcpy(base + scratch + 8, text.data(), text.size());
    base[scratch + 8 + text.size()] = 0;
    ctx.r3.u64 = scratch;
    ctx.r4.u64 = scratch + 8;
    sub_827C0728(ctx, base);
    return Load32(base, scratch);
  }

  // {value, type}.
  std::pair<uint32_t, uint32_t> Arg(uint32_t args, uint32_t index) {
    uint32_t nodes = Load32(base, args);
    ctx.r3.u64 = nodes + 8 * index;
    sub_8274AE98(ctx, base);
    uint32_t node = ctx.r3.u32;
    return {Load32(base, node), Load32(base, node + 4)};
  }

  uint32_t SongMetadata(int song_id) {
    ctx.r3.u64 = kTheSongMgr;
    ctx.r4.u64 = uint32_t(song_id);
    sub_827A8E30(ctx, base);
    return ctx.r3.u32;
  }

  PPCContext& ctx;
  uint8_t* base;
  PPCContext saved;
  uint32_t scratch;
};

// rbdxcache (later DX) or songcache.
std::string SongCacheName(const uint8_t* base) {
  std::string_view rdata(reinterpret_cast<const char*>(base + rb3pc::exe::kRdata),
                         rb3pc::exe::kRdataSize);
  return rdata.find(std::string_view("rbdxcache", 10)) != std::string_view::npos ? "rbdxcache"
                                                                                 : "songcache";
}

// Deferred to the restart: the game holds the cache open.
bool g_delete_song_cache = false;

void DeleteSongCache(const uint8_t* base) {
  std::string name = SongCacheName(base);
  std::filesystem::path root = rex::filesystem::GetUserFolder() / "rb3pc";
  std::error_code ec;
  for (const auto& profile : std::filesystem::directory_iterator(root, ec)) {
    std::filesystem::path title = profile.path() / "45410914";
    std::filesystem::path cache = title / "00000001" / name;
    if (!std::filesystem::exists(cache, ec)) continue;
    std::filesystem::remove_all(cache, ec);
    std::filesystem::remove(title / "Headers" / "00000001" / (name + ".header"), ec);
    REXLOG_INFO("RB3E: deleted song cache {}", rex::path_to_utf8(cache));
  }
}

constexpr std::string_view kNames[] = {
    "print_debug",      "rb3e_api_version",   "rb3e_build_tag",  "rb3e_commit",
    "rb3e_is_emulator", "rb3e_get_song_name", "rb3e_get_artist", "rb3e_get_album",
    "rb3e_get_origin",  "rb3e_get_genre",     "rb3e_delete_songcache", "rb3e_relaunch_game",
    "rb3e_local_ip",
};

// DataNode* f(DataNode* ret, DataArray* args).
void RunFunction(PPCContext& ctx, uint8_t* base, std::string_view name) {
  uint32_t ret = ctx.r3.u32, args = ctx.r4.u32;
  uint32_t value = 0, type = kInt;
  {
    GuestCall call(ctx, base);
    auto symbol = [&](std::string_view text) {
      value = call.Symbol(text);
      type = kSymbol;
    };
    if (name == "rb3e_api_version") {
      value = 1;
    } else if (name == "rb3e_build_tag" || name == "rb3e_commit") {
      symbol("rb3pc");
    } else if (name == "rb3e_is_emulator") {
      value = 0;
    } else if (name == "print_debug") {
      auto [v, t] = call.Arg(args, 1);
      float f;
      std::memcpy(&f, &v, 4);
      if (t == kInt) REXLOG_INFO("DX: {}", int32_t(v));
      else if (t == kFloat) REXLOG_INFO("DX: {}", f);
      else if (t == kString) REXLOG_INFO("DX: {}", GuestString(base, Load32(base, v)));
      else if (t == kSymbol) REXLOG_INFO("DX: {}", GuestString(base, v));
      value = 1;
    } else if (name == "rb3e_get_song_name" || name == "rb3e_get_artist" ||
               name == "rb3e_get_album" || name == "rb3e_get_origin" || name == "rb3e_get_genre") {
      std::string_view field = std::string_view(name).substr(name.rfind('_') + 1);
      symbol("rb3e_no_" + std::string(field == "name" ? "song_name" : field));
      auto [id, t] = call.Arg(args, 1);
      uint32_t meta = t == kInt ? call.SongMetadata(int32_t(id)) : 0;
      if (meta) {
        if (field == "origin") {
          value = Load32(base, meta + kMetaOrigin);
        } else if (field == "genre") {
          value = Load32(base, meta + kMetaGenre);
        } else {
          uint32_t string = meta + (field == "name"     ? kMetaTitle
                                    : field == "artist" ? kMetaArtist
                                                        : kMetaAlbum);
          symbol(GuestString(base, Load32(base, string + kStringText)));
        }
      }
    } else if (name == "rb3e_delete_songcache") {
      g_delete_song_cache = true;
      value = 1;
    } else if (name == "rb3e_relaunch_game") {
      if (g_delete_song_cache) DeleteSongCache(base);
      std::string error = rb3pc::RestartExecutable();
      REXLOG_ERROR("RB3E: {}", error);
    } else if (name == "rb3e_local_ip") {
      // No RB3E web server to reach.
      symbol("(not connected)");
    }
  }
  Store32(base, ret, value);
  Store32(base, ret + 4, type);
  ctx.r3.u64 = ret;
}

// One host function per name: scripts hold the guest address, not the name.
template <size_t I>
void RunNamedFunction(PPCContext& ctx, uint8_t* base) {
  RunFunction(ctx, base, kNames[I]);
}
template <size_t... I>
constexpr auto MakeFunctions(std::index_sequence<I...>) {
  return std::array<PPCFunc*, sizeof...(I)>{&RunNamedFunction<I>...};
}
constexpr auto kFunctions = MakeFunctions(std::make_index_sequence<std::size(kNames)>());

}  // namespace

REX_HOOK_RAW(sub_827C0EC8) {
  std::string option(GuestString(base, ctx.r3.u32));
  __imp__sub_827C0EC8(ctx, base);
  // PreInitSystem loops on -define until null; append ours.
  static size_t next_define = 0;
  if (ctx.r3.u32 || option != "define" || !Enabled() || next_define >= std::size(kDefines)) return;
  static uint32_t strings[std::size(kDefines)] = {};
  uint32_t& string = strings[next_define];
  if (!string) {
    const char* text = kDefines[next_define];
    string = rex::system::kernel_state()->memory()->SystemHeapAlloc(uint32_t(std::strlen(text) + 1));
    std::memcpy(base + string, text, std::strlen(text) + 1);
  }
  REXLOG_INFO("RB3E: defining {}", kDefines[next_define]);
  ++next_define;
  ctx.r3.u64 = string;
}

REX_HOOK_RAW(sub_82763A00) {
  __imp__sub_82763A00(ctx, base);
  if (!Enabled()) return;
  auto* dispatcher = rex::Runtime::instance()->function_dispatcher();
  GuestCall call(ctx, base);
  for (size_t i = 0; i < std::size(kNames); ++i) {
    uint32_t func = dispatcher->AllocateThunk(kFunctions[i], 0);
    if (!func) {
      REXLOG_ERROR("RB3E: no guest address for {}", kNames[i]);
      return;
    }
    Store32(base, call.scratch + 4, call.Symbol(kNames[i]));
    ctx.r3.u64 = kDataFuncs;
    ctx.r4.u64 = call.scratch + 4;
    sub_82359F28(ctx, base);
    Store32(base, ctx.r3.u32, func);
  }
  REXLOG_INFO("RB3E: added {} script functions", std::size(kNames));
}
