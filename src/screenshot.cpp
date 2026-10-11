#include "screenshot.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_INT32(screenshot_interval, 0, "rb3pc",
                     "Take a screenshot every N seconds (0 = off). For debugging.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  out.insert(out.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
}

void PutChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  PutU32(out, uint32_t(data.size()));
  size_t type_pos = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutU32(out, Crc32(out.data() + type_pos, data.size() + 4));
}

// RGB8 PNG with a zlib stream of uncompressed (stored) deflate blocks. Large,
// but needs no compression library and is only used for debug captures.
std::vector<uint8_t> EncodePng(const rex::ui::RawImage& image) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(image.height) * (1 + image.width * 3));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);  // filter: none
    const uint8_t* row = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);  // drop X8
    }
  }

  std::vector<uint8_t> zlib = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    size_t len = std::min<size_t>(raw.size() - pos, 65535);
    bool last = pos + len == raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.insert(zlib.end(), {uint8_t(len), uint8_t(len >> 8), uint8_t(~len), uint8_t(~len >> 8)});
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + len);
    pos += len;
    if (last) {
      break;
    }
  }
  PutU32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutU32(ihdr, image.width);
  PutU32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
  PutChunk(png, "IHDR", ihdr);
  PutChunk(png, "IDAT", zlib);
  PutChunk(png, "IEND", {});
  return png;
}

}  // namespace

std::filesystem::path SaveScreenshot(rex::ui::Presenter& presenter,
                                     const std::filesystem::path& dir) {
  rex::ui::RawImage image;
  if (!presenter.CaptureGuestOutput(image) || !image.width || !image.height) {
    REXLOG_WARN("Screenshot: no guest output to capture");
    return {};
  }

  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count() %
            1000;
  auto path = dir / fmt::format("rb3pc_{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}.png", tm.tm_year + 1900,
                                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto png = EncodePng(image);
  std::ofstream file(path, std::ios::binary);
  if (!file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()))) {
    REXLOG_ERROR("Screenshot: failed to write {}", path.string());
    return {};
  }
  REXLOG_INFO("Screenshot: saved {} ({}x{})", path.string(), image.width, image.height);
  return path;
}

void ScreenshotService::Start(rex::ui::WindowedAppContext& app_context,
                              std::function<rex::ui::Presenter*()> get_presenter,
                              std::filesystem::path dir) {
  app_context_ = &app_context;
  get_presenter_ = std::move(get_presenter);
  dir_ = std::move(dir);
  rex::ui::RegisterBind("bind_screenshot", "F12", "Save a screenshot of the game",
                        [this] { Capture(); });

  int32_t interval = REXCVAR_GET(screenshot_interval);
  if (interval <= 0) {
    return;
  }
  interval_thread_ = std::thread([this, interval] {
    std::unique_lock lock(mutex_);
    while (!stop_cv_.wait_for(lock, std::chrono::seconds(interval), [this] { return stopping_; })) {
      app_context_->CallInUIThread([this] { Capture(); });
    }
  });
}

void ScreenshotService::Stop() {
  if (!app_context_) {
    return;
  }
  rex::ui::UnregisterBind("bind_screenshot");
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  stop_cv_.notify_all();
  if (interval_thread_.joinable()) {
    interval_thread_.join();
  }
  app_context_ = nullptr;
}

void ScreenshotService::Capture() {
  if (auto* presenter = get_presenter_()) {
    SaveScreenshot(*presenter, dir_);
  }
}

}  // namespace rb3pc
