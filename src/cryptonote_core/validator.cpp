#include "validator.h"
#include "cryptonote_config.h" // Include the configuration header

namespace validator {

std::vector<Validator> init_validators() {
    std::vector<Validator> validators;

    // Add predefined validators
    validators.push_back({"Validator_1", VALIDATOR_PK1, {}});
    validators.push_back({"Validator_2", VALIDATOR_PK2, {}});
    validators.push_back({"Validator_3", VALIDATOR_PK3, {}});
    validators.push_back({"Validator_4", VALIDATOR_PK4, {}});
    validators.push_back({"Validator_5", VALIDATOR_PK5, {}});

    return validators;
}

} // namespace validator
