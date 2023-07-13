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

#include <sio/sequence/any_sequence_of.hpp>

namespace wayland {

template <class... Ts>
using any_receiver = exec::any_receiver_ref<stdexec::completion_signatures<
  stdexec::set_value_t(Ts...),
  stdexec::set_error_t(std::exception_ptr),
  stdexec::set_error_t(std::error_code),
  stdexec::set_stopped_t()>>;

template <class... Ts>
using any_sender_of = typename any_receiver<Ts...>::template any_sender<>;

template <class... Ts>
using any_sequence_receiver = sio::any_sequence_receiver_ref<stdexec::completion_signatures<
  stdexec::set_value_t(Ts...),
  stdexec::set_error_t(std::exception_ptr),
  stdexec::set_error_t(std::error_code),
  stdexec::set_stopped_t()>>;

template <class... Ts>
using any_sequence_of = typename any_sequence_receiver<Ts...>::template any_sender<>;

template <class Tp>
using any_resource = any_sequence_of<Tp>;

}