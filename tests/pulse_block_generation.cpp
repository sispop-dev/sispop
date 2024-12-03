#include "gtest/gtest.h"
#include "cryptonote_core/blockchain.h"
#include "cryptonote_core/pulse.h"
#include "cryptonote_core/validator.h"

TEST(PulseConsensus, BlockGenerationAndValidation) {
    cryptonote::block genesis_block; // Initialize a basic genesis block
    std::vector<validator::Validator> validators = validator::init_validators();
    pulse::Pulse pulse(validators);

    cryptonote::block new_block = pulse.generate_block(genesis_block, "Test Data");
    EXPECT_TRUE(pulse.validate_block(new_block));
}
