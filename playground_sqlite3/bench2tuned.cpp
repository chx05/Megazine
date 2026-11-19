// sqlite btree username lookup (optimized, aligned with the megazine btree benchmark)
//
// build (example):
// g++ bench2_sqlite.cpp -x c sqlite3.c -I. -static -lntdll -lwinmm -pthread -o bench2_sqlite.exe -O3 -DNDEBUG -march=native
//     -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK
//     -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_DECLTYPE

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

#include <sqlite3.h>

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 1'000'000;
constexpr size_t NUM_QUERIES = 1'000;

// Same page size as the megazine benchmark (SQLite default is 4096)
constexpr int PAGE_SIZE = 8192;

// false = username TEXT PRIMARY KEY ... WITHOUT ROWID  (general SQL, variable-length key)
// true  = username INTEGER PRIMARY KEY                  (8 chars packed into a big-endian int64 = rowid,
//                                                         the same key representation megazine uses)
constexpr bool KEY_AS_INTEGER = true;

// true  = every lookup is its own implicit read transaction (matches trx_r() per lookup in megazine)
// false = one read transaction around all lookups (isolates pure B-tree cost; mirror it in megazine too)
constexpr bool LOOKUP_TRX_PER_LOOKUP = true;

// true = synchronous=OFF, for parity with a megazine that does not fsync on close
constexpr bool UNSYNCED = false;

static_assert(NUM_USERS >= 2);

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

inline void prepare(sqlite3* db, SqliteStmtGuard& g, char const* sql, size_t sql_size_with_nul)
{
    // SQLITE_PREPARE_PERSISTENT: statement is reused many times; passing size incl. '\0' avoids a copy
    check_sqlite(
        sqlite3_prepare_v3(db, sql, static_cast<int>(sql_size_with_nul), SQLITE_PREPARE_PERSISTENT, &g.stmt, nullptr),
        db,
        sql);
}

int64_t pragma_int(sqlite3* db, char const* sql)
{
    SqliteStmtGuard s;
    check_sqlite(sqlite3_prepare_v2(db, sql, -1, &s.stmt, nullptr), db, sql);
    if (sqlite3_step(s.stmt) != SQLITE_ROW)
    {
        throw std::runtime_error(std::string("pragma returned no row: ") + sql);
    }
    return sqlite3_column_int64(s.stmt, 0);
}

// 8 ASCII chars -> big-endian uint64 (same as MegazineBTree::pack_key). ASCII < 0x80, so it always fits in int64.
inline int64_t pack_key(char const* s)
{
    uint64_t k = 0;
    std::memcpy(&k, s, USERNAME_LEN);
    return static_cast<int64_t>(__builtin_bswap64(k)); // little-endian host (x86)
}

inline void bind_key(sqlite3* db, sqlite3_stmt* stmt, User const& u)
{
    if constexpr (KEY_AS_INTEGER)
    {
        check_sqlite(sqlite3_bind_int64(stmt, 1, pack_key(u.username)), db, "sqlite3_bind_int64(key)");
    }
    else
    {
        check_sqlite(
            sqlite3_bind_text(stmt, 1, u.username, static_cast<int>(USERNAME_LEN), SQLITE_STATIC),
            db,
            "sqlite3_bind_text(key)");
    }
}

std::vector<User> generate_users()
{
    static constexpr char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static constexpr size_t alphabet_len = sizeof(alphabet) - 1; // 62
    static_assert(alphabet_len == 62 && USERNAME_LEN == 8);

    // 62^8 = 2^8 * 31^8
    constexpr uint64_t KEYSPACE = 218'340'105'584'896ULL;
    // Both multipliers are coprime to 2 and 31, and safe against uint64_t overflow
    constexpr uint64_t K1 = 180'000'000'001ULL;
    constexpr uint64_t K2 = 81'919ULL;

    static_assert(NUM_USERS <= KEYSPACE, "More users than unique 8-char usernames");
    static_assert(NUM_USERS <= (std::numeric_limits<uint64_t>::max() - KEYSPACE) / K1);
    static_assert((KEYSPACE - 1) <= (std::numeric_limits<uint64_t>::max() - (KEYSPACE - 1)) / K2);

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> key_dist(0, KEYSPACE - 1);
    // Use int64_t::max() / NUM_USERS if you want larger values without overflowing SQLite SUM() to REAL
    std::uniform_int_distribution<uint64_t> magic_dist(
        0, std::numeric_limits<uint32_t>::max() / NUM_USERS);

    uint64_t const offset1 = key_dist(rng);
    uint64_t const offset2 = key_dist(rng);

    std::vector<User> users;
    users.reserve(NUM_USERS);

    for (size_t i = 0; i < NUM_USERS; ++i)
    {
        uint64_t const x = (static_cast<uint64_t>(i) * K1 + offset1) % KEYSPACE;
        uint64_t code = (x * K2 + offset2) % KEYSPACE;

        User u{};
        for (size_t c = USERNAME_LEN; c-- > 0;)
        {
            u.username[c] = alphabet[code % alphabet_len];
            code /= alphabet_len;
        }
        u.magic_number = magic_dist(rng);
        users.push_back(u);
    }

    return users;
}

int main()
{
    constexpr char const* DB_PATH = "bench2_sqlite.db";

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
    sqlite3* const db = [&]
    {
        check_sqlite(
            sqlite3_open_v2(
                DB_PATH,
                &db_guard.db,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                nullptr),
            db_guard.db,
            "sqlite3_open_v2");
        return db_guard.db;
    }();

    // page_size must be set before the WAL switch / first table; locking_mode before WAL keeps the wal-index in RAM
    exec_sql(db, ("PRAGMA page_size = " + std::to_string(PAGE_SIZE) + ";").c_str());
    exec_sql(db, "PRAGMA locking_mode = EXCLUSIVE;");
    exec_sql(db, "PRAGMA journal_mode = WAL;");
    exec_sql(db, UNSYNCED ? "PRAGMA synchronous = OFF;" : "PRAGMA synchronous = NORMAL;");
    exec_sql(db, "PRAGMA cache_size = -65536;"); // 64 MB, the table is only a few MB
    exec_sql(db, "PRAGMA temp_store = MEMORY;");

    if constexpr (KEY_AS_INTEGER)
    {
        exec_sql(
            db,
            "CREATE TABLE users ("
            "  username INTEGER PRIMARY KEY,"
            "  magic_number INTEGER NOT NULL"
            ");");
    }
    else
    {
        exec_sql(
            db,
            "CREATE TABLE users ("
            "  username TEXT PRIMARY KEY,"
            "  magic_number INTEGER NOT NULL"
            ") WITHOUT ROWID;");
    }

    int64_t const actual_page_size = pragma_int(db, "PRAGMA page_size;");

    // statements are prepared outside the timed regions (megazine has no parse/plan step to time either)
    SqliteStmtGuard insert_stmt;
    static constexpr char insert_sql[] = "INSERT INTO users (username, magic_number) VALUES (?, ?);";
    prepare(db, insert_stmt, insert_sql, sizeof(insert_sql));

    SqliteStmtGuard scan_stmt;
    static constexpr char scan_sql[] = "SELECT SUM(magic_number), COUNT(*) FROM users;";
    prepare(db, scan_stmt, scan_sql, sizeof(scan_sql));

    SqliteStmtGuard lookup_stmt;
    static constexpr char lookup_sql[] = "SELECT magic_number FROM users WHERE username = ?;";
    prepare(db, lookup_stmt, lookup_sql, sizeof(lookup_sql));

    // 1) Write phase: single write transaction
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        exec_sql(db, "BEGIN IMMEDIATE;");
        for (auto const& u : users)
        {
            bind_key(db, insert_stmt.stmt, u);
            check_sqlite(
                sqlite3_bind_int64(insert_stmt.stmt, 2, static_cast<sqlite3_int64>(u.magic_number)),
                db,
                "sqlite3_bind_int64");

            int const rc = sqlite3_step(insert_stmt.stmt);
            if (rc != SQLITE_DONE)
            {
                throw std::runtime_error(std::string("sqlite3_step(INSERT) failed: ") + sqlite3_errmsg(db));
            }
            check_sqlite(sqlite3_reset(insert_stmt.stmt), db, "sqlite3_reset(INSERT)");
        }
        exec_sql(db, "COMMIT;");
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Full-scan phase: aggregate runs inside the engine, one row comes back per query
    uint64_t last_count = 0;
    uint64_t last_avg = 0;
    uint64_t checksum = 0;

    auto const t_scan_start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < NUM_QUERIES; ++q)
    {
        int const rc = sqlite3_step(scan_stmt.stmt);
        if (rc != SQLITE_ROW)
        {
            throw std::runtime_error(std::string("sqlite3_step(SCAN) failed: ") + sqlite3_errmsg(db));
        }
        uint64_t const sum = static_cast<uint64_t>(sqlite3_column_int64(scan_stmt.stmt, 0));
        uint64_t const cnt = static_cast<uint64_t>(sqlite3_column_int64(scan_stmt.stmt, 1));
        check_sqlite(sqlite3_reset(scan_stmt.stmt), db, "sqlite3_reset(SCAN)");

        last_count = cnt;
        last_avg = (cnt > 0) ? (sum / cnt) : 0;
        checksum += last_avg;
    }
    auto const t_scan_end = std::chrono::steady_clock::now();

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Scan verification failed!");
    }

    // 3) Point-lookup phase: one lookup per user, each verified
    uint64_t lookup_checksum = 0;

    auto const t_lookup_start = std::chrono::steady_clock::now();
    if constexpr (!LOOKUP_TRX_PER_LOOKUP)
    {
        exec_sql(db, "BEGIN;");
    }
    for (auto const& u : users)
    {
        bind_key(db, lookup_stmt.stmt, u);

        int const rc = sqlite3_step(lookup_stmt.stmt);
        if (rc != SQLITE_ROW)
        {
            throw std::runtime_error(std::string("sqlite3_step(LOOKUP) failed: ") + sqlite3_errmsg(db));
        }

        uint64_t const val = static_cast<uint64_t>(sqlite3_column_int64(lookup_stmt.stmt, 0));
        if (val != u.magic_number)
        {
            throw std::runtime_error("Lookup verification failed!");
        }
        lookup_checksum += val;

        // reset ends the implicit read transaction (autocommit) when LOOKUP_TRX_PER_LOOKUP
        check_sqlite(sqlite3_reset(lookup_stmt.stmt), db, "sqlite3_reset(LOOKUP)");
    }
    if constexpr (!LOOKUP_TRX_PER_LOOKUP)
    {
        exec_sql(db, "COMMIT;");
    }
    auto const t_lookup_end = std::chrono::steady_clock::now();

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const scan_us = std::chrono::duration<double, std::micro>(t_scan_end - t_scan_start).count();
    auto const lookup_us = std::chrono::duration<double, std::micro>(t_lookup_end - t_lookup_start).count();

    std::cout << "[sqlite]\n"
              << "  config:         page_size=" << actual_page_size
              << " key=" << (KEY_AS_INTEGER ? "INTEGER" : "TEXT/WITHOUT ROWID")
              << " lookup_trx=" << (LOOKUP_TRX_PER_LOOKUP ? "per-lookup" : "single")
              << " sync=" << (UNSYNCED ? "OFF" : "NORMAL") << '\n'
              << "  records:        " << last_count << '\n'
              << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
              << "  write (" << NUM_USERS << "):  " << write_us / 1000.0 << " ms\n"
              << "  scan (" << NUM_QUERIES << "x):    " << scan_us / 1000.0 << " ms (" << scan_us / NUM_QUERIES << " us/query)\n"
              << "  lookup (" << NUM_USERS << "x): " << lookup_us / 1000.0 << " ms (" << (lookup_us * 1000.0) / NUM_USERS
              << " ns/lookup, check=" << lookup_checksum << ")\n";

    return 0;
}