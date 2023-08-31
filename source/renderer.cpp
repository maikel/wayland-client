#include "./renderer.hpp"
#include "./font.hpp"
#include "./mdspan.hpp"

#include <sio/sequence/then_each.hpp>
#include <sio/sequence/merge_each.hpp>

#include <iostream>
#include <vector>

#include <fmt/format.h>

namespace wayland {
  struct render_context {
    using image_type = stdx::mdarray<
      color,
      stdx::extents<std::size_t, std::dynamic_extent, std::dynamic_extent>,
      stdx::layout_left>;
    image_type image;
    // font font{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    wayland::font font{"/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf"};

    render_context(int width, int height, color col) noexcept;


    void draw_shape(mdspan<std::uint8_t, 2> bitmap, position pos, color base) noexcept;
    void draw_text(std::string_view text, position pos, color base) noexcept;
    void draw_rectangle(position lower, position upper, color fill) noexcept;

    void draw_image() const noexcept;
  };

  render_context::render_context(int width, int height, color col) noexcept
  : image{width, height} {
    std::fill(image.container().begin(), image.container().end(), col);
  }

  static uint8_t apply_opacity(uint8_t base, uint8_t fill, uint8_t alpha) noexcept {
    return (base * (0xFF - alpha) + fill * alpha) / 0xFF;
  }

  static color apply_opacity(color base, color fill, uint8_t alpha) noexcept {
    base.red = apply_opacity(base.red, fill.red, alpha);
    base.green = apply_opacity(base.green, fill.green, alpha);
    base.blue = apply_opacity(base.blue, fill.blue, alpha);
    base.alpha = base.alpha;
    return base;
  }

  void render_context::draw_rectangle(position lower, position upper, color fill) noexcept {
    int nx = std::min<int>(upper.x, image.extent(0));
    int ny = std::min<int>(upper.y, image.extent(1));
    for (int y = lower.y; y < ny; ++y) {
      for (int x = lower.x; x < nx; ++x) {
        image(x, y) = fill;
      }
    }
  }

  void render_context::draw_shape(mdspan<std::uint8_t, 2> bitmap, position pos, color fill) noexcept {
    for (std::size_t j = 0; j < bitmap.extent(1); ++j) {
      const int iy = pos.y + j;
      if (iy >= image.extent(1)) {
        continue;
      }
      for (std::size_t i = 0; i < bitmap.extent(0); ++i) {
        const int ix = pos.x + i;
        if (ix >= image.extent(0)) {
          continue;
        }
        auto value = bitmap(i, j);
        if (value) {
          color& x = image(ix, iy);
          x = apply_opacity(x, fill, value);
        }
      }
    }
  }

  void render_context::draw_text(std::string_view text, position pos, color fill) noexcept {
    for (char c: text) {
      auto glyph = font.get_glyph(c);
      auto bitmap = glyph.bitmap();
      auto offset = glyph.offset();
      int start_y = pos.y - offset.y;
      int start_x = pos.x + offset.x;
      draw_shape(bitmap, {start_x, start_y}, fill);
      auto escapement = glyph.advance_by();
      pos.x += (escapement.x >> 6);
      pos.y += (escapement.y >> 6);
    }
  }

  struct close_file {
    void operator()(FILE* file) const noexcept {
      std::fclose(file);
    }
  };

  void render_context::draw_image() const noexcept {
    std::unique_ptr<FILE, close_file> file{std::fopen("image.ppm", "wb")};
    std::string header = fmt::format(
      "P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
      image.extent(0),
      image.extent(1));
    std::fwrite(header.data(), 1, header.size(), file.get());
    std::fwrite(image.data(), 1, image.size() * 4, file.get());
  }

  renderer::renderer(exec::io_uring_context& ctx, renderer_options opts) noexcept
    : context_(&ctx) 
    , opts_{opts} {
  }

  any_sequence_of<renderer_handle> renderer::use(sio::async::use_t) noexcept {
    auto context = sio::make_deferred<render_context>(opts_.width, opts_.height, opts_.background);
    return stdexec::just(context) //
         | stdexec::let_value([](auto& ctx) {
             render_context& context = ctx();
             return stdexec::just(renderer_handle{context});
           });
  }

  renderer_handle::renderer_handle(render_context& context) noexcept
    : context_(&context) {
  }

  void renderer_handle::draw_rectangle(position lower, position upper, color fill) const noexcept {
    context_->draw_rectangle(lower, upper, fill);
  }

  void renderer_handle::draw_text(std::string_view text, position pos, color base) const noexcept {
    context_->draw_text(text, pos, base);
  }

  void renderer_handle::render() const noexcept {
    context_->draw_image();
  }

} // namespace wayland