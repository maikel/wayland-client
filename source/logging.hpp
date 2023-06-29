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

#include <source_location>
#include <string_view>
#include <fmt/core.h>

namespace wayland {

  void vlog(
    std::source_location location,
    std::string_view channel,
    fmt::string_view format,
    fmt::format_args args);

  template <class... Args>
  struct log {
    log(
      std::string_view channel,
      fmt::format_string<Args...> format,
      Args&&... args,
      std::source_location location = std::source_location::current()) {
      vlog(location, channel, format, fmt::make_format_args(args...));
    }
  };

  template <class... Args>
  log(std::string_view, fmt::format_string<Args...>, Args&&...) -> log<Args...>;

} // namespace wayland