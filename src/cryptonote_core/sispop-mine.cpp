#include "new_mining_mechanism.h"
#include "randomx.h"
#include <string.h>
#include <iostream>

std::atomic<bool> NewMiningMechanism::mining(false);
std::thread NewMiningMechanism::mining_thread;

void NewMiningMechanism::mineBlock(Block& block, const void* data, size_t length, char* hash) {
    randomx_flags flags = RANDOMX_FLAG_DEFAULT;
    randomx_cache *cache = randomx_alloc_cache(flags);
    randomx_init_cache(cache, data, length);
    randomx_dataset *dataset = randomx_alloc_dataset(flags);
    randomx_vm *vm = randomx_create_vm(flags, cache, dataset);

    if (vm == NULL) {
        std::cerr << "Couldn't allocate RandomX VM" << std::endl;
        return;
    }

    block.nonce = 0;
    block.difficulty = FIXED_DIFFICULTY;

    // Simplified mining loop
    for (uint64_t i = 0; i < UINT64_MAX; ++i) {
        if (!mining) break;
        block.nonce = i;
        randomx_calculate_hash(vm, data, length, hash);
        
        if (memcmp(hash, &block.difficulty, sizeof(block.difficulty)) < 0) {
            break;
        }
    }

    randomx_destroy_vm(vm);
    randomx_release_cache(cache);
    randomx_release_dataset(dataset);
}

bool NewMiningMechanism::verifyBlock(const Block& block, const void* data, size_t length, const char* hash) {
    randomx_flags flags = RANDOMX_FLAG_DEFAULT;
    randomx_cache *cache = randomx_alloc_cache(flags);
    randomx_init_cache(cache, data, length);
    randomx_dataset *dataset = randomx_alloc_dataset(flags);
    randomx_vm *vm = randomx_create_vm(flags, cache, dataset);

    if (vm == NULL) {
        std::cerr << "Couldn't allocate RandomX VM" << std::endl;
        return false;
    }

    char calculated_hash[32];
    randomx_calculate_hash(vm, data, length, calculated_hash);

    bool isValid = (memcmp(calculated_hash, &block.difficulty, sizeof(block.difficulty)) < 0);

    randomx_destroy_vm(vm);
    randomx_release_cache(cache);
    randomx_release_dataset(dataset);

    return isValid;
}

void NewMiningMechanism::startMining(const std::string& wallet_address, size_t threads_count, bool do_background_mining, bool ignore_battery) {
    mining = true;
    mining_thread = std::thread([wallet_address, threads_count, do_background_mining, ignore_battery]() {
        Block block;
        char hash[32];
        NewMiningMechanism::mineBlock(block, wallet_address.c_str(), wallet_address.length(), hash);
        std::cout << "Mined block nonce: " << block.nonce << std::endl;
        std::cout << "Mining stopped." << std::endl;
    });
    mining_thread.detach();
}

void NewMiningMechanism::stopMining() {
    mining = false;
    if (mining_thread.joinable()) {
        mining_thread.join();
    }
}
