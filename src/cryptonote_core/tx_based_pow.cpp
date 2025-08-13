#include "tx_based_pow.h"
#include "crypto/crypto.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "ringct/rctSigs.h"

namespace cryptonote {


crypto::hash get_transaction_hashs(const cryptonote::transaction& tx) {
    crypto::hash h = crypto::null_hash;
    crypto::cn_fast_hash(&tx, sizeof(tx), h);
    return h;
}

crypto::hash get_block_hashs(const cryptonote::block& b) {
    crypto::hash h = crypto::null_hash;
    crypto::cn_fast_hash(&b, sizeof(b), h);
    return h;
}

// Generate transaction digest from privacy-preserving components
crypto::hash generate_transaction_digest(const transaction& tx) {
    std::vector<uint8_t> tx_data;

    // Extract transaction ID
    crypto::hash txid = cryptonote::get_transaction_hashs(tx);
    tx_data.insert(tx_data.end(), txid.data, txid.data + sizeof(txid.data));

    // Include ring signature data (hashed)
    if (tx.rct_signatures.type != rct::RCTTypeNull) {
        // Hash rct_signatures struct data
        crypto::hash rct_hash;
        crypto::cn_fast_hash(&tx.rct_signatures, sizeof(tx.rct_signatures), rct_hash);
        tx_data.insert(tx_data.end(), rct_hash.data, rct_hash.data + sizeof(rct_hash.data));
    }

    // Include stealth address (hashed one-time public key)
    for (const auto& out : tx.vout) {
        if (out.target.type() == typeid(txout_to_key)) {
            const auto& key = boost::get<txout_to_key>(out.target);
            crypto::hash key_hash;
            crypto::cn_fast_hash(key.key.data, sizeof(key.key.data), key_hash);
            tx_data.insert(tx_data.end(), key_hash.data, key_hash.data + sizeof(key_hash.data));
        }
    }

    // Compute combined hash
    crypto::hash digest;
    crypto::cn_fast_hash(tx_data.data(), tx_data.size(), digest);
    return digest;
}

// Generate combined transaction digest for a block
crypto::hash generate_block_tx_digest(const std::vector<transaction>& txs) {
    std::vector<uint8_t> combined_data;
    for (const auto& tx : txs) {
        crypto::hash tx_digest = generate_transaction_digest(tx);
        combined_data.insert(combined_data.end(), tx_digest.data, tx_digest.data + sizeof(tx_digest.data));
    }

    crypto::hash block_tx_digest;
    if (combined_data.empty()) {
        std::memset(block_tx_digest.data, 0, sizeof(block_tx_digest.data));
    } else {
        crypto::cn_fast_hash(combined_data.data(), combined_data.size(), block_tx_digest);
    }
    return block_tx_digest;
}

// Validate transactions as part of mining work
bool validate_transactions(const std::vector<transaction>& txs) {
    for (const auto& tx : txs) {
        if (!rct::verRct(tx.rct_signatures, true)) {
            return false;
        }
        if (tx.rct_signatures.type == rct::RCTTypeBulletproof2) {
            // In newer Monero code, bulletproof verification is included inside verRct, so this check can be omitted.
            // If needed, do:
            // if (!rct::verBulletproof(tx.rct_signatures.p.bulletproofs)) return false;
            // But here we'll just assume verRct covers it.
        }
    }
    return true;
}



bool mine_tx_based_block(const std::vector<transaction>& txs,
                         const crypto::hash& prev_block_hash,
                         uint64_t timestamp,
                         const uint256_t& difficulty_target,
                         tx_based_block_header& header,
                         uint64_t& winning_nonce) {
    header.tx_digest = generate_block_tx_digest(txs);
    header.prev_id = prev_block_hash;
    header.timestamp = timestamp;
    header.nonce = 0;

    if (!validate_transactions(txs)) {
        return false;
    }

    while (true) {
        std::vector<uint8_t> header_data;
        header_data.insert(header_data.end(), header.prev_id.data, header.prev_id.data + sizeof(header.prev_id.data));
        header_data.insert(header_data.end(), header.tx_digest.data, header.tx_digest.data + sizeof(header.tx_digest.data));
        header_data.insert(header_data.end(), reinterpret_cast<uint8_t*>(&header.timestamp),
                          reinterpret_cast<uint8_t*>(&header.timestamp) + sizeof(header.timestamp));
        header_data.insert(header_data.end(), reinterpret_cast<uint8_t*>(&header.nonce),
                          reinterpret_cast<uint8_t*>(&header.nonce) + sizeof(header.nonce));

        crypto::hash hash;
        crypto::cn_fast_hash(header_data.data(), header_data.size(), hash);

        uint256_t hash_val = 0;
        for (uint8_t i = 0; i < 32; ++i) {
            hash_val <<= 8;
            hash_val += static_cast<uint8_t>(hash.data[i]);
        }

        if (hash_val < difficulty_target) {
            winning_nonce = header.nonce;
            return true;
        }

        if (header.nonce == UINT64_MAX) {
            return false; // Exhausted all nonce values
        }
        ++header.nonce;
    }
}

// Create and mine a block
bool create_and_mine_block(std::vector<transaction>& txs,
                          const block& prev_block,
                          uint64_t timestamp,
                          const crypto::hash& difficulty_target,
                          block& new_block) {
    tx_based_block_header header;
    header.major_version = prev_block.major_version;
    header.minor_version = prev_block.minor_version;
    header.prev_id = cryptonote::get_block_hashs(prev_block);
    header.timestamp = timestamp;
    header.nonce = 0;

    uint256_t diff_target_val = 0;
    for (int i = 0; i < 32; ++i) {
        diff_target_val <<= 8;
        diff_target_val += static_cast<uint8_t>(difficulty_target.data[i]);
    }

    uint64_t winning_nonce;
    if (mine_tx_based_block(txs, header.prev_id, timestamp, diff_target_val, header, winning_nonce)) {
        new_block.major_version = header.major_version;
        new_block.minor_version = header.minor_version;
        new_block.prev_id = header.prev_id;
        new_block.timestamp = header.timestamp;
        new_block.nonce = winning_nonce;
        new_block.miner_tx = txs[0]; // Coinbase transaction
        new_block.tx_hashes.clear();
        for (size_t i = 1; i < txs.size(); ++i) {
            new_block.tx_hashes.push_back(cryptonote::get_transaction_hashs(txs[i]));
        }
        return true;
    }
    return false;
}


} // namespace cryptonote
