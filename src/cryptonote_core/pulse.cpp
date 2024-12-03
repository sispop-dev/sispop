#include "pulse.h"
#include "crypto/crypto.h"
#include "serialization/binary_utils.h"
#include "common/util.h"
#include "validator.h"
#include <stdexcept>
#include <mutex>

namespace pulse {

Pulse::Pulse()
    : validators(validator::init_validators()), current_validator_index(0) {
    if (sodium_init() < 0) {
        LOG_ERROR("Libsodium initialization failed.");
        throw std::runtime_error("Libsodium initialization failed.");
    }
}

cryptonote::block Pulse::generate_block(const cryptonote::block& previous_block, const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);  // Ensure thread safety
    const validator::Validator& current_validator = get_current_validator(current_validator_index);

    if (std::find_if(validators.begin(), validators.end(), [&current_validator](const validator::Validator& v) {
            return v.public_key == current_validator.public_key;
        }) == validators.end()) {
        LOG_ERROR("Validator not recognized: " << current_validator.id);
        throw std::runtime_error("Validator not recognized.");
    }

    cryptonote::block new_block;
    new_block.prev_id = previous_block.hash;
    new_block.timestamp = time(nullptr);
    new_block.height = previous_block.height + 1;
    new_block.miner_tx.extra = std::vector<uint8_t>(data.begin(), data.end());

    try {
        const crypto::hash block_hash = cryptonote::get_block_hash(new_block);
        const crypto::signature block_signature = sign_block(new_block, current_validator);
        new_block.signature = block_signature;
    } catch (const std::exception& e) {
        LOG_ERROR("Block generation failed: " << e.what());
        throw;
    }

    current_validator_index = (current_validator_index + 1) % validators.size();
    return new_block;
}

bool Pulse::validate_block(const cryptonote::block& block) const {
    const validator::Validator& validator = get_current_validator(block.height);

    if (std::find_if(validators.begin(), validators.end(), [&validator](const validator::Validator& v) {
            return v.public_key == validator.public_key;
        }) == validators.end()) {
        LOG_ERROR("Validator not recognized: " << validator.id);
        return false;
    }

    return verify_signature(block, validator);
}

validator::Validator Pulse::get_current_validator(uint64_t height) const {
    return validators[height % validators.size()];
}

crypto::signature Pulse::sign_block(const cryptonote::block& block, const validator::Validator& validator) const {
    const crypto::hash block_hash = cryptonote::get_block_hash(block);
    crypto::signature signature;
    try {
        crypto::generate_signature(block_hash, validator.public_key, validator.secret_key, signature);
    } catch (const std::exception& e) {
        LOG_ERROR("Block signing failed: " << e.what());
        throw;
    }
    return signature;
}

bool Pulse::verify_signature(const cryptonote::block& block, const validator::Validator& validator) const {
    const crypto::hash block_hash = cryptonote::get_block_hash(block);
    try {
        bool valid = crypto::check_signature(block_hash, validator.public_key, block.signature);
        if (!valid) {
            LOG_ERROR("Invalid signature for validator: " << validator.id);
        }
        return valid;
    } catch (const std::exception& e) {
        LOG_ERROR("Signature verification failed: " << e.what());
        return false;
    }
}

} // namespace pulse
