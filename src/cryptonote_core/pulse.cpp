#include "pulse.h"
#include "crypto/crypto.h"
#include "serialization/binary_utils.h"
#include <stdexcept>

namespace pulse {

Pulse::Pulse(const std::vector<Validator>& validators)
    : validators(validators), current_validator_index(0) {
    if (sodium_init() < 0) {
        throw std::runtime_error("Libsodium initialization failed.");
    }
}

cryptonote::block Pulse::generate_block(const cryptonote::block& previous_block, const std::string& data) {
    Validator current_validator = get_current_validator(current_validator_index);

    cryptonote::block new_block;
    new_block.prev_id = previous_block.hash;
    new_block.timestamp = time(nullptr);
    new_block.height = previous_block.height + 1;
    new_block.miner_tx.extra = std::vector<uint8_t>(data.begin(), data.end());

    crypto::hash block_hash = cryptonote::get_block_hash(new_block);
    crypto::signature block_signature = sign_block(new_block, current_validator);

    new_block.signature = block_signature;

    current_validator_index = (current_validator_index + 1) % validators.size();
    return new_block;
}

bool Pulse::validate_block(const cryptonote::block& block) {
    Validator validator = get_current_validator(block.height);
    return verify_signature(block, validator);
}

Validator Pulse::get_current_validator(uint64_t height) {
    return validators[height % validators.size()];
}

crypto::signature Pulse::sign_block(const cryptonote::block& block, const Validator& validator) {
    crypto::hash block_hash = cryptonote::get_block_hash(block);
    crypto::signature signature;
    crypto::generate_signature(block_hash, validator.public_key, validator.secret_key, signature);
    return signature;
}

bool Pulse::verify_signature(const cryptonote::block& block, const Validator& validator) {
    crypto::hash block_hash = cryptonote::get_block_hash(block);
    return crypto::check_signature(block_hash, validator.public_key, block.signature);
}

} // namespace pulse
