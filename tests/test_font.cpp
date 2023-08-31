#include "../source/font.hpp"
#include <iostream>

int main()
{
  std::string str = "Hello, World!";
  wayland::font font("/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf");

  for (char c : str) {
    auto glyph = font.get_glyph(c);
    auto bitmap = glyph.bitmap();
    for (std::size_t i = 0; i < bitmap.extent(0); ++i) {
      for (std::size_t j = 0; j < bitmap.extent(1); ++j) {
        if (bitmap(i, j)) {
          std::cout << (bitmap(i, j) < 128 ? '+' : '*');
        } else {
          std::cout << ' ';
        }
      }
      std::cout << '\n';
    }
  }
}