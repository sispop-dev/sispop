#include "validator.h"
#include <stdexcept>
#include <lmdb++.h>
#include "crypto/crypto.h"
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

// Get the path to the validator.db
std::string get_validator_db_path() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path) != S_OK) {
        throw std::runtime_error("Failed to get AppData path");
    }
    return std::string(path) + "\\Sispop\\validator.db";
#else
    const char* home_dir = getenv("HOME");
    if (home_dir == nullptr) {
        throw std::runtime_error("HOME environment variable not set");
    }
    return std::string(home_dir) + "/.sispop/validator.db";
#endif
}

// Open the LMDB environment for the validator database
expect<environment> open_validator_environment(const char* path) noexcept {
    MONERO_PRECOND(path != nullptr);

    MDB_env* obj = nullptr;
    MONERO_LMDB_CHECK(mdb_env_create(std::addressof(obj)));
    environment out{obj};

    MONERO_LMDB_CHECK(mdb_env_set_maxdbs(out.get(), 1));
    MONERO_LMDB_CHECK(mdb_env_open(out.get(), path, 0, open_flags));
    return {std::move(out)};
}

// Initialize the validator database.
void init_validator_db() {
    std::string db_path = get_validator_db_path();
    auto env = open_validator_environment(db_path.c_str()).value(); // Open environment
    auto txn = lmdb::txn::begin(env.get_handle());
    auto dbi = lmdb::dbi::open(txn, "validators", MDB_CREATE);
    txn.commit();
}

// Insert predefined keys into the validator database
void insert_predefined_keys() {
    auto env = open_validator_environment(get_validator_db_path().c_str()).value();

    auto txn = lmdb::txn::begin(env.get_handle());
    auto dbi = lmdb::dbi::open(txn, "validators");

    const auto& config = cryptonote::get_config(m_nettype, version);
    dbi.put(txn, "Validator_1", lmdb::val(config.VALIDATOR_PK1.data, sizeof(config.VALIDATOR_PK1.data)));
    dbi.put(txn, "Validator_2", lmdb::val(config.VALIDATOR_PK2.data, sizeof(config.VALIDATOR_PK2.data)));
    dbi.put(txn, "Validator_3", lmdb::val(config.VALIDATOR_PK3.data, sizeof(config.VALIDATOR_PK3.data)));
    dbi.put(txn, "Validator_4", lmdb::val(config.VALIDATOR_PK4.data, sizeof(config.VALIDATOR_PK4.data)));
    dbi.put(txn, "Validator_5", lmdb::val(config.VALIDATOR_PK5.data, sizeof(config.VALIDATOR_PK5.data)));

    txn.commit();
}

// Initialize validators by retrieving them from the database
std::vector<Validator> init_validators() {
    std::vector<Validator> validators;
    auto env = open_validator_environment(get_validator_db_path().c_str()).value();

    auto txn = lmdb::txn::begin(env.get_handle(), MDB_RDONLY);
    auto dbi = lmdb::dbi::open(txn, "validators");

    for (const auto& validator_name : {"Validator_1", "Validator_2", "Validator_3", "Validator_4", "Validator_5"}) {
        lmdb::val key(validator_name);
        lmdb::val value;
        if (dbi.get(txn, key, value)) {
            Validator v;
            v.id = validator_name;
            v.public_key = *reinterpret_cast<const crypto::public_key*>(value.data());
            validators.push_back(v);
        } else {
            throw std::runtime_error("Failed to retrieve validator key: " + std::string(validator_name));
        }
    }

    if (validators.size() != 5) {
        throw std::runtime_error("Failed to initialize validators: expected 5 validators.");
    }

    return validators;
}
