
#ifndef TX_BASED_POW_H
#define TX_BASED_POW_H

#include <vector>
#include <cstdint>
#include "crypto/crypto.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/blobdatatype.h"

#include <boost/multiprecision/cpp_int.hpp>

using uint256_t = boost::multiprecision::uint256_t;

namespace cryptonote {

constexpr size_t TX_DIGEST_SIZE = 32; // SHA-256 output size
constexpr size_t MAX_TXS_PER_BLOCK = 1000; // Limit on transactions per block

crypto::hash get_transaction_hashs(const cryptonote::transaction& tx);
crypto::hash get_block_hashs(const cryptonote::block& b);

struct tx_based_block_header {
    uint64_t major_version;
    uint64_t minor_version;
    uint64_t timestamp;
    crypto::hash prev_id; // Previous block hash
    crypto::hash tx_digest; // Combined hash of transactions
    uint64_t nonce;
};

// Function prototypes
crypto::hash generate_transaction_digest(const transaction& tx);
crypto::hash generate_block_tx_digest(const std::vector<transaction>& txs);
bool validate_transactions(const std::vector<transaction>& txs);
bool mine_tx_based_block(const std::vector<transaction>& txs,
                        const crypto::hash& prev_block_hash,
                        uint64_t timestamp,
                        const uint256_t& difficulty_target,
                        tx_based_block_header& header,
                        uint64_t& winning_nonce);
bool create_and_mine_block(std::vector<transaction>& txs,
                          const block& prev_block,
                          uint64_t timestamp,
                          const uint256_t& difficulty_target,
                          block& new_block);

} // namespace cryptonote

#endif // TX_BASED_POW_H
