// Copyright (c) 2018-2021, The Loki Project
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

#include <array>

#include "hardfork.h"

namespace cryptonote {

// version 7 from the start of the blockchain, inhereted from Monero mainnet
static constexpr std::array mainnet_hard_forks =
{
  hard_fork{7,  0,        0, 1503046577 }, 
  hard_fork{8,  0,    100, 1692426487 }, 
  hard_fork{9,  0,   102, 1692426900 }, 
  hard_fork{10, 0,   110, 1692427080 }, 
  hard_fork{11, 0,   118, 1692427200 }, 
  hard_fork{12, 0,   120, 1692427260 },
  hard_fork{13, 0,   125, 1692427500 }, 
  hard_fork{14, 0,   128, 1692427620 }, 
  hard_fork{15, 0,   130, 1692427740 }, 
};

static constexpr std::array testnet_hard_forks =
{
  hard_fork{7,  0,        0, 1533631121 }, // Testnet was rebooted during Loki 3 development
  hard_fork{8,  0,        2, 1533631122 },
  hard_fork{9,  0,        3, 1533631123 },
  hard_fork{10, 0,        4, 1542681077 },
  hard_fork{11, 0,        5, 1551223964 },
  hard_fork{12, 0,    75471, 1561608000 }, // 2019-06-28 14:00 AEDT
  hard_fork{13, 0,   127028, 1568440800 }, // 2019-09-13 16:00 AEDT
  hard_fork{14, 0,   174630, 1575075600 }, // 2019-11-30 07:00 UTC
  hard_fork{15, 0,   244777, 1583940000 }, // 2020-03-11 15:20 UTC
  hard_fork{16, 0,   382222, 1600468200 }, // 2020-09-18 22:30 UTC
  hard_fork{17, 0,   447275, 1608276840 }, // 2020-12-18 05:34 UTC
  hard_fork{18, 0,   501750, 1616631051 }, // 2021-03-25 12:10 UTC
  hard_fork{18, 1,   578637, 1624040400 }, // 2021-06-18 18:20 UTC
};

static constexpr std::array devnet_hard_forks =
{
  hard_fork{ 7, 0,      0,  1599848400 },
  hard_fork{ 11, 0,     2,  1599848400 },
  hard_fork{ 12, 0,     3,  1599848400 },
  hard_fork{ 13, 0,     4,  1599848400 }, 
  hard_fork{ 15, 0,     5,  1599848400 },
  hard_fork{ 16, 0,    99,  1599848400 },
};


template <size_t N>
static constexpr bool is_ordered(const std::array<hard_fork, N>& forks) {
  if (N == 0 || forks[0].version < 7)
    return false;
  for (size_t i = 1; i < N; i++) {
    auto& hf = forks[i];
    auto& prev = forks[i-1];
    if ( // [major,snoderevision] pair must be strictly increasing (lexicographically)
        std::make_pair(hf.version, hf.snode_revision) <= std::make_pair(prev.version, prev.snode_revision)
        // height must be strictly increasing; time must be weakly increasing
        || hf.height <= prev.height || hf.time < prev.time)
      return false;
  }
  return true;
}

static_assert(is_ordered(mainnet_hard_forks),
    "Invalid mainnet hard forks: version must start at 7, major versions and heights must be strictly increasing, and timestamps must be non-decreasing");
static_assert(is_ordered(testnet_hard_forks),
    "Invalid testnet hard forks: version must start at 7, versions and heights must be strictly increasing, and timestamps must be non-decreasing");
static_assert(is_ordered(devnet_hard_forks),
    "Invalid devnet hard forks: version must start at 7, versions and heights must be strictly increasing, and timestamps must be non-decreasing");

std::vector<hard_fork> fakechain_hardforks;

std::pair<const hard_fork*, const hard_fork*> get_hard_forks(network_type type)
{
  if (type == network_type::MAINNET) return {&mainnet_hard_forks[0], &mainnet_hard_forks[mainnet_hard_forks.size()]};
  if (type == network_type::TESTNET) return {&testnet_hard_forks[0], &testnet_hard_forks[testnet_hard_forks.size()]};
  if (type == network_type::DEVNET) return {&devnet_hard_forks[0], &devnet_hard_forks[devnet_hard_forks.size()]};
  if (type == network_type::FAKECHAIN) return {fakechain_hardforks.data(), fakechain_hardforks.data() + fakechain_hardforks.size()};
  return {nullptr, nullptr};
}


std::pair<std::optional<uint64_t>, std::optional<uint64_t>>
get_hard_fork_heights(network_type nettype, uint8_t version) {
  std::pair<std::optional<uint64_t>, std::optional<uint64_t>> found;
  for (auto [it, end] = get_hard_forks(nettype); it != end; it++) {
    if (it->version > version) { // This (and anything else) are in the future
      if (found.first) // Found something suitable in the previous iteration, so one before this hf is the max
        found.second = it->height - 1;
      break;
    } else if (it->version == version) {
      found.first = it->height;
    }
  }
  return found;
}

uint8_t hard_fork_ceil(network_type nettype, uint8_t version) {
  auto [it, end] = get_hard_forks(nettype);
  for (; it != end; it++)
    if (it->version >= version)
      return it->version;

  return version;
}

std::pair<uint8_t, uint8_t>
get_network_version_revision(network_type nettype, uint64_t height) {
  std::pair<uint8_t, uint8_t> result;
  for (auto [it, end] = get_hard_forks(nettype); it != end; it++) {
    if (it->height <= height)
      result = {it->version, it->snode_revision};
    else
      break;
  }
  return result;
}

bool is_hard_fork_at_least(network_type type, uint8_t version, uint64_t height) {
  return get_network_version(type, height) >= version;
}

std::pair<uint8_t, uint8_t>
get_ideal_block_version(network_type nettype, uint64_t height)
{
  std::pair<uint8_t, uint8_t> result;
  for (auto [it, end] = get_hard_forks(nettype); it != end; it++) {
    if (it->height <= height)
      result.first = it->version;
    result.second = it->version;
  }
  return result;
}


}

