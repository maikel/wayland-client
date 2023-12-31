/*
 * Copyright (c) 2023 Maikel Nadolski
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <stdexec/execution.hpp>

#include <sio/deferred.hpp>

namespace wayland {

template <class Tp>
struct construct_t {
  template <class Sender>
  auto operator()(Sender&& sndr) const {
    return stdexec::then(std::forward<Sender>(sndr), []<class... Args>(Args&&... args) {
      return Tp{std::forward<Args>(args)...};
    });
  }

  stdexec::__binder_back<construct_t> operator()() const {
    return {{}, {}, {}};
  }
};

template <class Tp>
inline constexpr construct_t<Tp> construct{};

}