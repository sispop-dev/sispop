#ifndef NEW_MINING_MECHANISM_H
#define NEW_MINING_MECHANISM_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <atomic>
#include <thread>

typedef struct {
    uint64_t nonce;
    uint64_t difficulty;
    // Additional fields as needed
} Block;

class NewMiningMechanism {
public:
    static const uint64_t FIXED_DIFFICULTY = 90000;
    static void mineBlock(Block& block, const void* data, size_t length, char* hash);
    static bool verifyBlock(const Block& block, const void* data, size_t length, const char* hash);
    static void startMining(const std::string& wallet_address, size_t threads_count, bool do_background_mining, bool ignore_battery);
    static void stopMining();
private:
    static std::atomic<bool> mining;
    static std::thread mining_thread;
};

#endif // NEW_MINING_MECHANISM_H
