#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <mdbx.h>

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 100'000;
constexpr size_t NUM_QUERIES = 1'000;

struct User
{
    char username[USERNAME_LEN];
    uint64_t magic_number;
};

static_assert(sizeof(User) == USERNAME_LEN + sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<User>);

inline void check_mdbx(int rc, char const* op)
{
    if (rc != MDBX_SUCCESS)
    {
        throw std::runtime_error(
            std::string(op) + " failed: (" + std::to_string(rc) + ") " + mdbx_strerror(rc));
    }
}

struct MdbxEnvGuard
{
    MDBX_env* env = nullptr;
    ~MdbxEnvGuard()
    {
        if (env != nullptr)
        {
            mdbx_env_close(env);
        }
    }
};

struct MdbxTxnGuard
{
    MDBX_txn* txn = nullptr;
    ~MdbxTxnGuard()
    {
        if (txn != nullptr)
        {
            mdbx_txn_abort(txn);
        }
    }
};

struct MdbxCursorGuard
{
    MDBX_cursor* cursor = nullptr;
    ~MdbxCursorGuard()
    {
        if (cursor != nullptr)
        {
            mdbx_cursor_close(cursor);
        }
    }
};

std::vector<User> generate_users()
{
    static constexpr char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static constexpr size_t alphabet_len = sizeof(alphabet) - 1;

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> char_dist(0, alphabet_len - 1);
    std::uniform_int_distribution<uint64_t> magic_dist(
        0, std::numeric_limits<uint64_t>::max() / NUM_USERS);

    std::vector<User> users(NUM_USERS);
    std::unordered_set<uint64_t> seen;
    seen.reserve(NUM_USERS * 2);

    for (size_t i = 0; i < NUM_USERS; ++i)
    {
        while (true)
        {
            for (size_t c = 0; c < USERNAME_LEN; ++c)
            {
                users[i].username[c] = alphabet[char_dist(rng)];
            }
            uint64_t packed = 0;
            std::memcpy(&packed, users[i].username, USERNAME_LEN);
            if (seen.insert(packed).second)
            {
                break;
            }
        }
        users[i].magic_number = magic_dist(rng);
    }
    return users;
}

int main()
{
    constexpr char const* DB_PATH = "test2_mdbx";

    std::error_code ec;
    std::filesystem::remove(DB_PATH, ec);
    std::filesystem::remove(std::string(DB_PATH) + "-lck", ec);

    auto const users = generate_users();
    uint64_t expected_sum = 0;
    for (auto const& u : users)
    {
        expected_sum += u.magic_number;
    }
    uint64_t const expected_avg = expected_sum / NUM_USERS;

    MdbxEnvGuard env_guard;
    check_mdbx(mdbx_env_create(&env_guard.env), "mdbx_env_create");
    check_mdbx(
        mdbx_env_set_geometry(
            env_guard.env,
            -1,
            -1,
            intptr_t{ 512 } * 1024 * 1024,
            -1,
            -1,
            -1),
        "mdbx_env_set_geometry");
    // Optional for write-durability parity with un-synced megazine writes: add | MDBX_SAFE_NOSYNC
    check_mdbx(
        mdbx_env_open(env_guard.env, DB_PATH, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0664),
        "mdbx_env_open");

    MDBX_dbi dbi = 0;

    // 1) Write phase: register 1,000,000 users in a single write transaction
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        MdbxTxnGuard wtxn;
        check_mdbx(mdbx_txn_begin(env_guard.env, nullptr, MDBX_TXN_READWRITE, &wtxn.txn), "mdbx_txn_begin(RW)");
        check_mdbx(mdbx_dbi_open(wtxn.txn, nullptr, MDBX_CREATE, &dbi), "mdbx_dbi_open");

        for (auto const& u : users)
        {
            MDBX_val key{};
            key.iov_base = const_cast<char*>(u.username);
            key.iov_len = USERNAME_LEN;

            MDBX_val val{};
            val.iov_base = const_cast<uint64_t*>(&u.magic_number);
            val.iov_len = sizeof(u.magic_number);

            check_mdbx(mdbx_put(wtxn.txn, dbi, &key, &val, MDBX_NOOVERWRITE), "mdbx_put");
        }

        MDBX_txn* txn = wtxn.txn;
        wtxn.txn = nullptr;
        check_mdbx(mdbx_txn_commit(txn), "mdbx_txn_commit");
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Full-scan phase: run NUM_QUERIES separate read transactions
    uint64_t last_avg = 0;
    uint64_t last_count = 0;
    uint64_t checksum = 0;

    std::chrono::steady_clock::time_point t_scan_start{};
    std::chrono::steady_clock::time_point t_scan_end{};
    {
        MdbxTxnGuard rtxn;
        check_mdbx(mdbx_txn_begin(env_guard.env, nullptr, MDBX_TXN_RDONLY, &rtxn.txn), "mdbx_txn_begin(SCAN)");
        MdbxCursorGuard cursor;
        check_mdbx(mdbx_cursor_open(rtxn.txn, dbi, &cursor.cursor), "mdbx_cursor_open");
        check_mdbx(mdbx_txn_reset(rtxn.txn), "mdbx_txn_reset(SCAN)");

        t_scan_start = std::chrono::steady_clock::now();
        for (size_t q = 0; q < NUM_QUERIES; ++q)
        {
            check_mdbx(mdbx_txn_renew(rtxn.txn), "mdbx_txn_renew(SCAN)");
            check_mdbx(mdbx_cursor_renew(rtxn.txn, cursor.cursor), "mdbx_cursor_renew");

            uint64_t counter = 0;
            uint64_t record_count = 0;
            MDBX_val key{}, val{};
            int rc = mdbx_cursor_get(cursor.cursor, &key, &val, MDBX_FIRST);
            while (rc == MDBX_SUCCESS)
            {
                uint64_t magic = 0;
                std::memcpy(&magic, val.iov_base, sizeof(magic));
                counter += magic;
                ++record_count;
                rc = mdbx_cursor_get(cursor.cursor, &key, &val, MDBX_NEXT);
            }
            if (rc != MDBX_NOTFOUND)
            {
                check_mdbx(rc, "mdbx_cursor_get");
            }

            check_mdbx(mdbx_txn_reset(rtxn.txn), "mdbx_txn_reset(SCAN)");
            last_count = record_count;
            last_avg = (record_count > 0) ? (counter / record_count) : 0;
            checksum += last_avg;
        }
        t_scan_end = std::chrono::steady_clock::now();
    }

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Scan verification failed!");
    }

    // 3) Point-lookup phase: 1,000,000 O(log N) lookups, separate read trx per lookup
    uint64_t lookup_checksum = 0;
    std::chrono::steady_clock::time_point t_lookup_start{};
    std::chrono::steady_clock::time_point t_lookup_end{};
    {
        MdbxTxnGuard rtxn;
        check_mdbx(mdbx_txn_begin(env_guard.env, nullptr, MDBX_TXN_RDONLY, &rtxn.txn), "mdbx_txn_begin(LOOKUP)");
        check_mdbx(mdbx_txn_reset(rtxn.txn), "mdbx_txn_reset(LOOKUP)");

        t_lookup_start = std::chrono::steady_clock::now();
        for (auto const& u : users)
        {
            check_mdbx(mdbx_txn_renew(rtxn.txn), "mdbx_txn_renew(LOOKUP)");

            MDBX_val key{};
            key.iov_base = const_cast<char*>(u.username);
            key.iov_len = USERNAME_LEN;

            MDBX_val val{};
            check_mdbx(mdbx_get(rtxn.txn, dbi, &key, &val), "mdbx_get");

            uint64_t magic = 0;
            if (val.iov_len != sizeof(magic))
            {
                throw std::runtime_error("Lookup verification failed: invalid value size!");
            }
            std::memcpy(&magic, val.iov_base, sizeof(magic));
            if (magic != u.magic_number)
            {
                throw std::runtime_error("Lookup verification failed!");
            }
            lookup_checksum += magic;

            check_mdbx(mdbx_txn_reset(rtxn.txn), "mdbx_txn_reset(LOOKUP)");
        }
        t_lookup_end = std::chrono::steady_clock::now();
    }

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const scan_us = std::chrono::duration<double, std::micro>(t_scan_end - t_scan_start).count();
    auto const lookup_us = std::chrono::duration<double, std::micro>(t_lookup_end - t_lookup_start).count();

    std::cout << "[libmdbx]\n"
              << "  records:        " << last_count << '\n'
              << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
              << "  write (" << NUM_USERS << "):  " << write_us / 1000.0 << " ms\n"
              << "  scan (" << NUM_QUERIES << "x):    " << scan_us / 1000.0 << " ms (" << scan_us / NUM_QUERIES << " us/query)\n"
              << "  lookup (" << NUM_USERS << "x): " << lookup_us / 1000.0 << " ms (" << (lookup_us * 1000.0) / NUM_USERS << " ns/lookup, check=" << lookup_checksum << ")\n"
              << '\n'
              << "mdbx options: " << (mdbx_build.options ? mdbx_build.options : "(null)") << '\n'
              << "mdbx flags:   " << (mdbx_build.flags   ? mdbx_build.flags   : "(null)") << '\n';

    return 0;
}