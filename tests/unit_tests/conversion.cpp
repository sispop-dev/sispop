// Copyright (c) 2023, SISPOPyr Protocol
// Portions copyright (c) 2016-2019, The Monero Project
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

#include "gtest/gtest.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "oracle/pricing_record.h"
#include "vector"

using tt = cryptonote::transaction_type;

#define INIT_PR(pr)                                                                             \
    std::vector<std::pair<std::string, std::string>> circ_amounts;                              \
    circ_amounts.push_back(std::make_pair("SISPOP", "1000000000000000"));    /* 1000 * 10^12 */ \
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "1000000000000000")); /* 1000 * 10^12 */ \
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "1000000000000000")); /* 1000 * 10^12 */ \
    pr.spot = 20ull * COIN;                                                                     \
    pr.moving_average = 15ull * COIN;                                                           \
    pr.stable = cryptonote::get_stable_coin_price(circ_amounts, pr.spot);                       \
    pr.stable_ma = cryptonote::get_stable_coin_price(circ_amounts, pr.moving_average);          \
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);                     \
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

#define UPDATE_PR(pr)                                                                  \
    pr.stable = cryptonote::get_stable_coin_price(circ_amounts, pr.spot);              \
    pr.stable_ma = cryptonote::get_stable_coin_price(circ_amounts, pr.moving_average); \
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);            \
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

/*
 * STABLE_COIN_PRICE
 */
TEST(get_stable_coin_price, get_stable_coin_price_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);
    EXPECT_EQ(pr.stable, 50000000000);
    EXPECT_EQ(pr.stable_ma, 66666660000);
}

TEST(get_stable_coin_price, get_stable_coin_price_zero_with_zero_rate)
{
    oracle::pricing_record pr;
    INIT_PR(pr);
    pr.spot = 0;
    pr.moving_average = 0;
    UPDATE_PR(pr);
    EXPECT_EQ(pr.stable, 0);
    EXPECT_EQ(pr.stable_ma, 0);
}
TEST(get_stable_coin_price, get_stable_coin_price_zero_on_overflow)
{
    std::vector<std::pair<std::string, std::string>> circ_amounts;
    circ_amounts.push_back(std::make_pair("SISPOP", "1000000000000000")); // 1000
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "0"));
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "1000000000000000")); // 1000
    oracle::pricing_record pr;
    pr.spot = 1;
    pr.moving_average = 1;
    UPDATE_PR(pr);
    EXPECT_EQ(pr.stable, 0);
    EXPECT_EQ(pr.stable_ma, 0);
}
TEST(get_stable_coin_price, get_stable_coin_price_returns_sispop_rsv_over_stable_circ_when_below_100_percent)
{
    oracle::pricing_record pr;
    INIT_PR(pr);
    pr.spot = 80000000000;           // 0.08
    pr.moving_average = 72000000000; // 0.072
    UPDATE_PR(pr);

    EXPECT_EQ(pr.stable, 1000000000000);
    EXPECT_EQ(pr.stable_ma, 1000000000000);
}

/*
 * RESERVE_COIN_PRICE
 */
TEST(get_reserve_coin_price, get_reserve_coin_price_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);
    EXPECT_EQ(pr.reserve, 950000000000);
    EXPECT_EQ(pr.reserve_ma, 933333330000);
}

TEST(get_reserve_coin_price, get_reserve_coin_price_zero_with_zero_rate)
{
    oracle::pricing_record pr;
    INIT_PR(pr);
    pr.spot = 0;
    pr.moving_average = 0;
    UPDATE_PR(pr);
    EXPECT_EQ(pr.reserve, 0);
    EXPECT_EQ(pr.reserve_ma, 0);
}

TEST(get_reserve_coin_price, get_reserve_coin_price_zero_on_overflow)
{
    std::vector<std::pair<std::string, std::string>> circ_amounts;
    oracle::pricing_record pr;
    circ_amounts.push_back(std::make_pair("SISPOP", "1000000000000000"));    // 1000
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "1000000000000000")); // 1000
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "1000000"));          // 0.000001
    pr.spot = 1000000ull * COIN;
    pr.moving_average = 1000000ull * COIN;
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

    EXPECT_EQ(pr.reserve, 0);
    EXPECT_EQ(pr.reserve_ma, 0);
}

TEST(get_reserve_coin_price, get_reserve_coin_price_uses_price_r_min_if_no_reserves_issued)
{
    std::vector<std::pair<std::string, std::string>> circ_amounts;
    oracle::pricing_record pr;
    circ_amounts.push_back(std::make_pair("SISPOP", "1000000000000000"));    // 1000
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "1000000000000000")); // 1000
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "0"));
    pr.spot = 20ull * COIN;
    pr.moving_average = 15ull * COIN;
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

    uint64_t price_r_min = 500000000000;
    EXPECT_EQ(pr.reserve, price_r_min);
    EXPECT_EQ(pr.reserve_ma, price_r_min);
}

TEST(get_reserve_coin_price, get_reserve_coin_price_uses_price_r_min_if_zero_equity)
{
    std::vector<std::pair<std::string, std::string>> circ_amounts;
    oracle::pricing_record pr;
    circ_amounts.push_back(std::make_pair("SISPOP", "500000000000000"));     // 500
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "1000000000000000")); // 1000
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "1000000000000000")); // 1000
    pr.spot = 1 * COIN;
    pr.moving_average = 1 * COIN;
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

    uint64_t price_r_min = 500000000000;
    EXPECT_EQ(pr.reserve, price_r_min);
    EXPECT_EQ(pr.reserve_ma, price_r_min);
}

TEST(get_reserve_coin_price, get_reserve_coin_price_uses_price_r_min_at_lowest)
{
    std::vector<std::pair<std::string, std::string>> circ_amounts;
    oracle::pricing_record pr;

    // $1000 equity | 10000 rsv coins issued creates a rsv coin price of 0.10 (lower than price_r_min)
    circ_amounts.push_back(std::make_pair("SISPOP", "10000000000000000"));    // 10000
    circ_amounts.push_back(std::make_pair("SISPOPUSD", "9000000000000000"));  // 9000
    circ_amounts.push_back(std::make_pair("SISPOPRSV", "10000000000000000")); // 10000
    pr.spot = 1 * COIN;
    pr.moving_average = 1 * COIN;
    pr.reserve = cryptonote::get_reserve_coin_price(circ_amounts, pr.spot);
    pr.reserve_ma = cryptonote::get_reserve_coin_price(circ_amounts, pr.moving_average);

    uint64_t price_r_min = 500000000000;
    EXPECT_EQ(pr.reserve, price_r_min);
    EXPECT_EQ(pr.reserve_ma, price_r_min);
}

/*
 * MINT_STABLE_RATE
 */
TEST(sispop_to_sispopusd, sispop_to_sispopusd_conversion_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 120ull * COIN;
    uint64_t expected_conversion_amount = 1764000176400000;
    EXPECT_EQ(cryptonote::sispop_to_sispopusd(tx_amount, pr), expected_conversion_amount);
}

TEST(sispop_to_sispopusd, sispop_to_sispopusd_uses_lower_of_spot_vs_ma)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 1756ull * COIN;
    uint64_t expected_conversion_amount = 25813202581320000;
    EXPECT_EQ(cryptonote::sispop_to_sispopusd(tx_amount, pr), expected_conversion_amount);

    pr.moving_average = 25ull * COIN;
    expected_conversion_amount = 34417600000000000;
    UPDATE_PR(pr);
    EXPECT_EQ(cryptonote::sispop_to_sispopusd(tx_amount, pr), expected_conversion_amount);
}

TEST(sispop_to_sispopusd, sispop_to_sispopusd_overflow_returns_zero)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = UINT64_MAX;
    EXPECT_EQ(cryptonote::sispop_to_sispopusd(tx_amount, pr), 0);
}

/*
 * REDEEM_STABLE_RATE
 */
TEST(sispopusd_to_sispop, sispopusd_to_sispop_conversion_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 120ull * COIN;
    uint64_t expected_conversion_amount = 5880000000000;
    EXPECT_EQ(cryptonote::sispopusd_to_sispop(tx_amount, pr), expected_conversion_amount);
}

TEST(sispopusd_to_sispop, sispopusd_to_sispop_uses_higher_of_spot_vs_ma)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 1756ull * COIN;
    uint64_t expected_conversion_amount = 86044000000000;
    EXPECT_EQ(cryptonote::sispopusd_to_sispop(tx_amount, pr), expected_conversion_amount);

    pr.moving_average = 25ull * COIN;
    expected_conversion_amount = 68835200000000;
    UPDATE_PR(pr);
    EXPECT_EQ(cryptonote::sispopusd_to_sispop(tx_amount, pr), expected_conversion_amount);
}

TEST(sispopusd_to_sispop, sispopusd_to_sispop_overflow_returns_zero)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    pr.spot = 1000ull * COIN;
    pr.moving_average = 1000ull * COIN;
    pr.stable = 1000ull * COIN;
    pr.stable_ma = 1000ull * COIN;

    uint64_t tx_amount = UINT64_MAX;
    EXPECT_EQ(cryptonote::sispopusd_to_sispop(tx_amount, pr), 0);
}

/*
 * MINT_RESERVE_RATE
 */
TEST(sispop_to_sispoprsv, sispop_to_sispoprsv_conversion_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 120ull * COIN;
    uint64_t expected_conversion_amount = 126315788400000;

    EXPECT_EQ(cryptonote::sispop_to_sispoprsv(tx_amount, pr), expected_conversion_amount);
}

TEST(sispop_to_sispoprsv, sispop_to_sispoprsv_uses_lower_of_spot_vs_ma)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 1756ull * COIN;
    uint64_t expected_conversion_amount = 1848421036920000;
    EXPECT_EQ(cryptonote::sispop_to_sispoprsv(tx_amount, pr), expected_conversion_amount);

    pr.moving_average = 25ull * COIN;
    expected_conversion_amount = 1829166654960000;
    UPDATE_PR(pr);
    EXPECT_EQ(cryptonote::sispop_to_sispoprsv(tx_amount, pr), expected_conversion_amount);
}

TEST(sispop_to_sispoprsv, sispop_to_sispoprsv_overflow_returns_zero)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = UINT64_MAX;
    EXPECT_EQ(cryptonote::sispop_to_sispoprsv(tx_amount, pr), 0);
}

/*
 * REDEEM_RESERVE_RATE
 */
TEST(sispoprsv_to_sispop, sispoprsv_to_sispop_conversion_success)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 120ull * COIN;
    uint64_t expected_conversion_amount = 109759999200000;
    EXPECT_EQ(cryptonote::sispoprsv_to_sispop(tx_amount, pr), expected_conversion_amount);
}

TEST(sispoprsv_to_sispop, sispoprsv_to_sispop_uses_higher_of_spot_vs_ma)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    uint64_t tx_amount = 1756ull * COIN;
    uint64_t expected_conversion_amount = 1606154654960000;
    EXPECT_EQ(cryptonote::sispoprsv_to_sispop(tx_amount, pr), expected_conversion_amount);

    pr.moving_average = 25ull * COIN;
    expected_conversion_amount = 1634836000000000;
    UPDATE_PR(pr);
    EXPECT_EQ(cryptonote::sispoprsv_to_sispop(tx_amount, pr), expected_conversion_amount);
}

TEST(sispoprsv_to_sispop, sispoprsv_to_sispop_overflow_returns_zero)
{
    oracle::pricing_record pr;
    INIT_PR(pr);

    pr.reserve = 1000ull * COIN;
    pr.reserve_ma = 1000ull * COIN;

    uint64_t tx_amount = UINT64_MAX;
    EXPECT_EQ(cryptonote::sispoprsv_to_sispop(tx_amount, pr), 0);
}
