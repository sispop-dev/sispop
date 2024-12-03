#ifndef VALIDATOR_H
#define VALIDATOR_H

#include <vector>
#include <string>
#include "crypto/crypto.h"

namespace validator {

struct Validator {
    std::string id;
    crypto::public_key public_key;
    // crypto::secret_key secret_key; // Uncomment only for testing, remove in production
};

std::vector<Validator> init_validators();

} // namespace validator

#endif // VALIDATOR_H
