#pragma once

#include <stdexec/execution.hpp>

namespace wayland {

template <class Tp>
struct construct_t {
  template <class Sender>
  auto operator()(Sender&& sndr) const {
    return stdexec::then(std::forward<Sender>(sndr), []<class... Args>(Args&&... args) {
      return Tp{std::forward<Args>(args)...};
    });
  }

  template <class Sender>
  stdexec::__binder_back<construct_t> operator()() const {
    return {{}, {}, {}};
  }
};

template <class Tp>
inline constexpr construct_t<Tp> construct{};

}