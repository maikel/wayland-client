#pragma once

#include "./mdspan.hpp"

#include <filesystem>

namespace wayland {

struct vector {
  std::int32_t x;
  std::int32_t y;
};

class font;

class glyph {
public:
  mdspan<std::uint8_t, 2> bitmap() const noexcept;

  vector offset() const noexcept;

  vector advance_by() const noexcept;

private:
  friend class font;
  void* handle_;
};

class font {
public:
  explicit font(std::filesystem::path font_file);
  ~font();

  font(const font&) = delete;
  font& operator=(const font&) = delete;

  font(font&&) noexcept;
  font& operator=(font&&) noexcept;

  glyph get_glyph(std::uint32_t codepoint) const;

private:
  void* handle_;
};

}