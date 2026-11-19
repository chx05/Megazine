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

#include <sqlite3.h>

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 100'000;
constexpr size_t NUM_QUERIES = 1'000;

static_assert(NUM_USERS >= 2, "Ensures magic_number fits in signed sqlite3_int64");

struct User
{
    char username[USERNAME_LEN];
    uint64_t magic_number;
};

static_assert(sizeof(User) == USERNAME_LEN + sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<User>);

inline void check_sqlite(int rc, sqlite3* db, char const* op)
{
    if (rc != SQLITE_OK)
    {
        throw std::runtime_error(
            std::string(op) + " failed: (" + std::to_string(rc) + ") " +
            (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
    }
}

inline void exec_sql(sqlite3* db, char const* sql)
{
    char* errmsg = nullptr;
    int const rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        std::string msg = std::string("SQL failed [") + sql + "]: " + (errmsg ? errmsg : sqlite3_errmsg(db));
        if (errmsg)
        {
            sqlite3_free(errmsg);
        }
        throw std::runtime_error(msg);
    }
}

struct SqliteDbGuard
{
    sqlite3* db = nullptr;
    ~SqliteDbGuard()
    {
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
    }
};

struct SqliteStmtGuard
{
    sqlite3_stmt* stmt = nullptr;
    ~SqliteStmtGuard()
    {
        if (stmt != nullptr)
        {
            sqlite3_finalize(stmt);
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
        0, std::numeric_limits<uint32_t>::max() / NUM_USERS);

    std::vector<User> users{};
    for (size_t i = 0; i < NUM_USERS; ++i)
    {
        users.push_back(User());
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
    constexpr char const* DB_PATH = "test2_sqlite.db";

    std::error_code ec;
    std::filesystem::remove(DB_PATH, ec);
    std::filesystem::remove(std::string(DB_PATH) + "-wal", ec);
    std::filesystem::remove(std::string(DB_PATH) + "-shm", ec);
    std::filesystem::remove(std::string(DB_PATH) + "-journal", ec);

    auto const users = generate_users();
    uint64_t expected_sum = 0;
    for (auto const& u : users)
    {
        expected_sum += u.magic_number;
    }
    uint64_t const expected_avg = expected_sum / NUM_USERS;

    SqliteDbGuard db_guard;
    check_sqlite(sqlite3_open(DB_PATH, &db_guard.db), db_guard.db, "sqlite3_open");

    exec_sql(db_guard.db, "PRAGMA journal_mode = WAL;");
    // Optional tuning for single-process / relaxed-durability comparison:
    exec_sql(db_guard.db, "PRAGMA locking_mode = EXCLUSIVE;");
    // exec_sql(db_guard.db, "PRAGMA synchronous = OFF;");
    // exec_sql(db_guard.db, "PRAGMA mmap_size = 268435456;");

    exec_sql(
        db_guard.db,
        "CREATE TABLE users ("
        "  username TEXT PRIMARY KEY,"
        "  magic_number INTEGER NOT NULL"
        ") WITHOUT ROWID;");

    // 1) Write phase: register 100 users in a single write transaction
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        SqliteStmtGuard insert_stmt;
        check_sqlite(
            sqlite3_prepare_v2(
                db_guard.db,
                "INSERT INTO users (username, magic_number) VALUES (?, ?);",
                -1,
                &insert_stmt.stmt,
                nullptr),
            db_guard.db,
            "sqlite3_prepare_v2(INSERT)");

        exec_sql(db_guard.db, "BEGIN IMMEDIATE;");
        for (size_t i = 0; i < NUM_USERS; ++i)
        {
            check_sqlite(
                sqlite3_bind_text(
                    insert_stmt.stmt,
                    1,
                    users[i].username,
                    static_cast<int>(USERNAME_LEN), // exact byte length, no null-terminator needed
                    SQLITE_STATIC),
                db_guard.db,
                "sqlite3_bind_text");
            check_sqlite(
                sqlite3_bind_int64(
                    insert_stmt.stmt,
                    2,
                    static_cast<sqlite3_int64>(users[i].magic_number)),
                db_guard.db,
                "sqlite3_bind_int64");

            int const rc = sqlite3_step(insert_stmt.stmt);
            if (rc != SQLITE_DONE)
            {
                throw std::runtime_error(
                    std::string("sqlite3_step(INSERT) failed: ") + sqlite3_errmsg(db_guard.db));
            }
            check_sqlite(sqlite3_reset(insert_stmt.stmt), db_guard.db, "sqlite3_reset(INSERT)");
        }
        exec_sql(db_guard.db, "COMMIT;");
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Read phase: run NUM_QUERIES separate read transactions, summing all records each time
    SqliteStmtGuard select_stmt;
    check_sqlite(
        sqlite3_prepare_v2(
            db_guard.db,
            "SELECT SUM(magic_number) FROM users;",
            -1,
            &select_stmt.stmt,
            nullptr),
        db_guard.db,
        "sqlite3_prepare_v2(SELECT)");

    uint64_t last_avg = 0;
    uint64_t checksum = 0;

    auto const t_read_start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < NUM_QUERIES; ++q)
    {
        uint64_t counter = 0;

        while (true)
        {
            int const rc = sqlite3_step(select_stmt.stmt);
            if (rc == SQLITE_ROW)
            {
                if (sqlite3_column_type(select_stmt.stmt, 0) != SQLITE_NULL)
                {
                    counter = static_cast<uint64_t>(sqlite3_column_int64(select_stmt.stmt, 0));
                }
            }
            else if (rc == SQLITE_DONE)
            {
                break;
            }
            else
            {
                throw std::runtime_error(
                    std::string("sqlite3_step(SELECT) failed: ") + sqlite3_errmsg(db_guard.db));
            }
        }
        check_sqlite(sqlite3_reset(select_stmt.stmt), db_guard.db, "sqlite3_reset(SELECT)");

        last_avg = counter / NUM_USERS;
        checksum += last_avg;
    }
    auto const t_read_end = std::chrono::steady_clock::now();

    if (last_avg != expected_avg)
    {
        throw std::runtime_error("Verification failed!");
    }

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const read_us = std::chrono::duration<double, std::micro>(t_read_end - t_read_start).count();

    std::cout << "[sqlite]\n"
              << "  records:        " << NUM_USERS << '\n'
              << "  iterations:     " << NUM_QUERIES << '\n'
              << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
              << "  write (" << NUM_USERS << "):    " << write_us / 1000.0 << " ms (" << write_us << " us)\n"
              << "  read (" << NUM_QUERIES << "x):   " << read_us / 1000.0 << " ms (" << read_us / NUM_QUERIES << " us/query)\n"
              << "  total:          " << (write_us + read_us) / 1000.0 << " ms\n";

    return 0;
}