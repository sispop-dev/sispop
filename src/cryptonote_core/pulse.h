#ifndef PULSE_H
#define PULSE_H

#include <vector>
#include <string>
#include <sodium.h>
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
    Pulse(const std::vector<Validator>& validators);

    cryptonote::block generate_block(const cryptonote::block& previous_block, const std::string& data);
    bool validate_block(const cryptonote::block& block);
    Validator get_current_validator(uint64_t height);

private:
    std::vector<Validator> validators;
    uint64_t current_validator_index;
    crypto::signature sign_block(const cryptonote::block& block, const Validator& validator);
    bool verify_signature(const cryptonote::block& block, const Validator& validator);
};

} // namespace pulse

#endif // PULSE_H
