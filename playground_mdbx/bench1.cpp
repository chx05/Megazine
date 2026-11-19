#include <array>
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

#include <mdbx.h>

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 100'000;
constexpr size_t NUM_QUERIES = 1'000;

struct User
{
    char username[USERNAME_LEN];
    uint64_t magic_number;
};

static_assert(sizeof(User) == 16);
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

std::array<User, NUM_USERS> generate_users()
{
    static constexpr char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static constexpr size_t alphabet_len = sizeof(alphabet) - 1;

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> char_dist(0, alphabet_len - 1);
    std::uniform_int_distribution<uint64_t> magic_dist(
        0, std::numeric_limits<uint64_t>::max() / NUM_USERS);

    std::array<User, NUM_USERS> users{};
    for (size_t i = 0; i < NUM_USERS; ++i)
    {
        bool unique = false;
        while (!unique)
        {
            for (size_t c = 0; c < USERNAME_LEN; ++c)
            {
                users[i].username[c] = alphabet[char_dist(rng)];
            }
            unique = true;
            for (size_t j = 0; j < i; ++j)
            {
                if (std::memcmp(users[i].username, users[j].username, USERNAME_LEN) == 0)
                {
                    unique = false;
                    break;
                }
            }
        }
        users[i].magic_number = magic_dist(rng);
    }
    return users;
}

int main()
{
    constexpr char const* DB_PATH = "test2_mdbx.db";

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
        mdbx_env_open(env_guard.env, DB_PATH, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0664),
        "mdbx_env_open");

    MDBX_dbi dbi = 0;

    // 1) Write phase: register 100 users in a single write transaction
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        MDBX_txn* txn = nullptr;
        check_mdbx(mdbx_txn_begin(env_guard.env, nullptr, MDBX_TXN_READWRITE, &txn), "mdbx_txn_begin(RW)");
        check_mdbx(mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &dbi), "mdbx_dbi_open");

        for (size_t i = 0; i < NUM_USERS; ++i)
        {
            MDBX_val key{};
            key.iov_base = const_cast<char*>(users[i].username);
            key.iov_len = USERNAME_LEN;

            MDBX_val val{};
            val.iov_base = const_cast<uint64_t*>(&users[i].magic_number);
            val.iov_len = sizeof(users[i].magic_number);

            check_mdbx(mdbx_put(txn, dbi, &key, &val, MDBX_NOOVERWRITE), "mdbx_put");
        }

        check_mdbx(mdbx_txn_commit(txn), "mdbx_txn_commit");
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Read phase: run 1000 separate read transactions, scanning all records each time
    uint64_t last_avg = 0;
    uint64_t last_count = 0;
    uint64_t checksum = 0;

    MDBX_txn* txn = nullptr;
    check_mdbx(mdbx_txn_begin(env_guard.env, nullptr, MDBX_TXN_RDONLY, &txn), "mdbx_txn_begin");
    MDBX_cursor* cursor = nullptr;
    check_mdbx(mdbx_cursor_open(txn, dbi, &cursor), "mdbx_cursor_open");
    check_mdbx(mdbx_txn_reset(txn), "mdbx_txn_reset");

    auto const t_read_start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < NUM_QUERIES; ++q)
    {
        check_mdbx(mdbx_txn_renew(txn), "mdbx_txn_renew");
        check_mdbx(mdbx_cursor_renew(txn, cursor), "mdbx_cursor_renew");

        uint64_t counter = 0;
        uint64_t record_count = 0;
        MDBX_val key{}, val{};
        int rc = mdbx_cursor_get(cursor, &key, &val, MDBX_FIRST);
        while (rc == MDBX_SUCCESS)
        {
            uint64_t magic = 0;
            std::memcpy(&magic, val.iov_base, sizeof(magic));
            counter += magic;
            ++record_count;
            rc = mdbx_cursor_get(cursor, &key, &val, MDBX_NEXT);
        }
        if (rc != MDBX_NOTFOUND)
        {
            check_mdbx(rc, "mdbx_cursor_get");
        }

        check_mdbx(mdbx_txn_reset(txn), "mdbx_txn_reset");
        last_count = record_count;
        last_avg = (record_count > 0) ? (counter / record_count) : 0;
        checksum += last_avg;
    }
    auto const t_read_end = std::chrono::steady_clock::now();

    mdbx_cursor_close(cursor);
    mdbx_txn_abort(txn);

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Verification failed!");
    }

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const read_us = std::chrono::duration<double, std::micro>(t_read_end - t_read_start).count();

    std::cout << "[libmdbx]\n"
              << "  records:        " << last_count << '\n'
              << "  iterations:     " << NUM_QUERIES << '\n'
              << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
              << "  write (100):    " << write_us / 1000.0 << " ms (" << write_us << " us)\n"
              << "  read (1000x):   " << read_us / 1000.0 << " ms (" << read_us / NUM_QUERIES << " us/query)\n"
              << "  total:          " << (write_us + read_us) / 1000.0 << " ms\n"
              << '\n'
              << "mdbx options: " << (mdbx_build.options ? mdbx_build.options : "(null)") << '\n'
              << "mdbx flags:   " << (mdbx_build.flags   ? mdbx_build.flags   : "(null)") << '\n';

    return 0;
}