// Copyright (c) 2018-2020, The Loki Project
// Copyright (c) 2016-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

// the loose definitions of types in this file are, well, loose.
//
// these helpers aren't here for absolute type certainty at compile-time,
// but rather to help with templated functions telling types apart.

namespace sfinae
{

template <typename T, typename = void>
constexpr bool is_container_like = false;

// Container-like: has a begin(), end(), and a ::value_type.
template <typename T>
constexpr bool is_container_like<T, std::void_t<
  decltype(std::declval<T>().begin()),
  decltype(std::declval<T>().end()),
  typename T::value_type>> = true;

template <typename T, typename = void>
constexpr bool is_map_like = false;

// Map-like: looks like a container plus has a ::key_type and ::mapped_type
template <typename T>
constexpr bool is_map_like<T, std::enable_if_t<is_container_like<T>, std::void_t<
  typename T::key_type,
  typename T::mapped_type>>> = true;

// List-like: looks like a container but *doesn't* look like a map
template <typename T>
constexpr bool is_list_like = is_container_like<T> && !is_map_like<T>;

}  // namespace sfinae
