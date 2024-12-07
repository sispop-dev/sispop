#ifndef PULSE_H
#define PULSE_H

#include "cryptonote_basic/cryptonote_basic_impl.h"  // Adjust this to the correct header file
#include "crypto/crypto.h"
#include "validator.h"
#include <vector>
#include <mutex>

namespace pulse {

class Pulse {
public:
    // Constructor with validators
    Pulse(const std::vector<validator::Validator>& validators);

    // Block generation function
    cryptonote::block generate_block(const cryptonote::block& previous_block, const std::string& data);

    // Block validation function
    bool validate_block(const cryptonote::block& block) const;

private:
    // Validators
    std::vector<validator::Validator> validators;

    // Current validator index
    uint64_t current_validator_index;

    // Mutex for thread safety
    mutable std::mutex mutex_;

    // Utility functions
    validator::Validator get_current_validator(uint64_t height) const;
    crypto::signature sign_block(const cryptonote::block& block, const validator::Validator& validator) const;
    bool verify_signature(const cryptonote::block& block, const validator::Validator& validator) const;
};

} // namespace pulse

#endif // PULSE_H
