#ifndef PULSE_H
#define PULSE_H

#include <vector>
#include <string>
#include <sodium.h>
#include <mutex> // For thread safety
#include "cryptonote_basic/cryptonote_basic.h" // For block & tx definitions
#include "crypto/crypto.h"

namespace pulse {

struct Validator {
    std::string id;
    crypto::public_key public_key;
    crypto::secret_key secret_key; // Only for testing, remove in production
};

class Pulse {
public:
    // Constructor
    Pulse(const std::vector<Validator>& validators);

    // Generates a new block based on the previous block and provided data
    cryptonote::block generate_block(const cryptonote::block& previous_block, const std::string& data);

    // Validates the provided block
    bool validate_block(const cryptonote::block& block) const;

    // Returns the current validator based on the block height
    Validator get_current_validator(uint64_t height) const;

private:
    std::vector<Validator> validators;
    uint64_t current_validator_index;
    mutable std::mutex mutex_; // For thread safety

    // Signs the provided block with the validator's keys
    crypto::signature sign_block(const cryptonote::block& block, const Validator& validator) const;

    // Verifies the signature of the provided block with the validator's public key
    bool verify_signature(const cryptonote::block& block, const Validator& validator) const;
};

} // namespace pulse

#endif // PULSE_H
