#include "./font.hpp"

#include <freetype2/ft2build.h>
#include FT_FREETYPE_H

namespace wayland {
  namespace {
    struct font_context {
      font_context(std::filesystem::path font_file);
      ~font_context();

      void get_glyph(std::uint32_t codepoint) const;

      FT_Library library;
      FT_Face face;
    };

    font_context::font_context(std::filesystem::path font_file) {
      FT_Init_FreeType(&library);
      FT_Error rc = FT_New_Face(library, font_file.c_str(), 0, &face);
      if (rc) {
        FT_Done_FreeType(library);
        throw std::runtime_error("Failed to load font");
      }
      FT_Set_Char_Size(face, 0, 4 * 64, 300, 300);
    }

    font_context::~font_context() {
      FT_Done_Face(face);
      FT_Done_FreeType(library);
    }

    void font_context::get_glyph(std::uint32_t codepoint) const {
      FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
      FT_Error rc = FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER);
      if (rc) {
        throw std::runtime_error(FT_Error_String(rc));
      }
      rc = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
      if (rc) {
        throw std::runtime_error(FT_Error_String(rc));
      }
    }
  } // namespace

  font::font(std::filesystem::path font_file)
    : handle_(new font_context(std::move(font_file))) {
  }

  font::~font() {
    delete static_cast<font_context*>(handle_);
  }

  font::font(font&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {
  }

  font& font::operator=(font&& other) noexcept {
    delete static_cast<font_context*>(handle_);
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  glyph font::get_glyph(std::uint32_t codepoint) const {
    auto context = static_cast<font_context*>(handle_);
    context->get_glyph(codepoint);
    glyph g{};
    g.handle_ = static_cast<void*>(context->face->glyph);
    return g;
  }

  mdspan<std::uint8_t, 2> glyph::bitmap() const noexcept {
    auto* glyph = static_cast<FT_GlyphSlot>(handle_);
    return mdspan<std::uint8_t, 2>(glyph->bitmap.buffer, glyph->bitmap.width, glyph->bitmap.rows);
  }

  vector glyph::offset() const noexcept {
    auto* glyph = static_cast<FT_GlyphSlot>(handle_);
    return vector{glyph->bitmap_left, glyph->bitmap_top};
  }

  vector glyph::advance_by() const noexcept {
    auto* glyph = static_cast<FT_GlyphSlot>(handle_);
    return vector{
      static_cast<std::int32_t>(glyph->advance.x), static_cast<std::int32_t>(glyph->advance.y)};
  }

} // namespace wayland