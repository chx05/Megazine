#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sqlite3.h>
#include "..\megazine_engine.hpp"

using Clock = std::chrono::steady_clock;

constexpr uint64_t PAGE_SIZE = 8192;
constexpr uint64_t MISSING_BASE = 1ULL << 40;

struct Cfg
{
    std::string engine;
    std::string db_path;
    uint32_t threads = 4;
    uint64_t users = 1'000'000;
    uint32_t warmup = 2;
    uint32_t duration = 10;
    uint32_t batch = 100;
    uint64_t write_rate = 10'000;
    uint32_t miss_pct = 5;
    double skew = 7.21;
    bool sync = false;
    bool int_key = false;
};

// ---------------- Deterministic data & PRNG ----------------

inline uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() { return mix64(s += 0x9E3779B97F4A7C15ULL); }
    double next_unit() { return static_cast<double>((next() >> 11) + 1) * (1.0 / 9007199254740992.0); }
};

// Bijective 48-bit scrambler -> unique 8-char ASCII username for id in [0, 2^48)
inline void make_username(uint64_t id, char out[8])
{
    static constexpr char alpha[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-";
    constexpr uint64_t M48 = (1ULL << 48) - 1;
    uint64_t x = ((id + 0x123456789ABCULL) * 0x9E3779B97F4A7C15ULL) & M48;
    x ^= (x >> 24);
    x = (x * 0xBF58476D1CE4E5B9ULL) & M48;
    for (int i = 7; i >= 0; --i)
    {
        out[i] = alpha[x & 63];
        x >>= 6;
    }
}

inline uint64_t pack_key(char const* s)
{
    uint64_t k = 0;
    std::memcpy(&k, s, 8);
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(k);
#else
    return ((k & 0xFFULL) << 56) | ((k & 0xFF00ULL) << 40) | ((k & 0xFF0000ULL) << 24) |
           ((k & 0xFF000000ULL) << 8) | ((k & 0xFF00000000ULL) >> 8) | ((k & 0xFF0000000000ULL) >> 24) |
           ((k & 0xFF000000000000ULL) >> 40) | ((k & 0xFF00000000000000ULL) >> 56);
#endif
}

inline uint64_t immutable_val(uint64_t id)
{
    return mix64(id ^ 0xA5A5A5A55A5A5A5AULL) & 0x7FFFFFFFFFFFFFFFULL;
}

inline uint64_t mutable_val(uint64_t id, uint32_t epoch)
{
    uint64_t const e = epoch & 0x7FFFFFFFU;
    uint32_t const tag = static_cast<uint32_t>(mix64(id ^ (e * 0x9E3779B97F4A7C15ULL)));
    return (e << 32) | tag;
}

inline bool verify_mutable_val(uint64_t id, uint64_t val)
{
    return (val >> 63) == 0 && val == mutable_val(id, static_cast<uint32_t>(val >> 32));
}

inline uint64_t pick_hit_id(Rng& rng, uint64_t users, double skew)
{
    if (skew == 1.0 || skew <= 0.0) return rng.next() % users;
    uint64_t const id = static_cast<uint64_t>(static_cast<double>(users) * std::pow(rng.next_unit(), skew));
    return id < users ? id : (users - 1);
}

// ---------------- Log-linear latency histogram (ns) ----------------

struct Hist
{
    uint64_t bins[256]{};

    static int bin_of(uint64_t ns)
    {
        if (ns < 8) return static_cast<int>(ns);
        int const msb = 63 - std::countl_zero(ns);
        if (msb > 33) return 255;
        int const b = ((msb - 2) << 3) | static_cast<int>((ns >> (msb - 3)) & 7);
        return b < 256 ? b : 255;
    }

    static uint64_t ns_of(int b)
    {
        if (b < 8) return static_cast<uint64_t>(b);
        return (uint64_t{8} | static_cast<uint64_t>(b & 7)) << ((b >> 3) - 1);
    }

    void add(uint64_t ns) { ++bins[bin_of(ns)]; }
    void merge(Hist const& o) { for (int i = 0; i < 256; ++i) bins[i] += o.bins[i]; }

    uint64_t pct(double p) const
    {
        uint64_t total = 0;
        for (uint64_t c : bins) total += c;
        if (!total) return 0;
        uint64_t const target = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p * static_cast<double>(total))));
        uint64_t acc = 0;
        for (int i = 0; i < 256; ++i)
        {
            acc += bins[i];
            if (acc >= target) return ns_of(i);
        }
        return ns_of(255);
    }
};

struct ReadRes
{
    uint64_t ops = 0, hits = 0, misses = 0, checksum = 0;
    Hist hist;
};

struct WriteRes
{
    uint64_t ops = 0, commits = 0;
};

// ---------------- Megazine B+Tree ----------------

struct MegazineBTree
{
    struct MetaPage { uint64_t root_page, first_leaf, count; uint32_t height; };

    static constexpr size_t NODE_IS_LEAF_OFF = 0;
    static constexpr size_t NODE_COUNT_OFF = 4;
    static constexpr size_t LEAF_NEXT_OFF = 8;
    static constexpr size_t LEAF_KEYS_OFF = 16;
    static constexpr size_t LEAF_CAP = (PAGE_SIZE - LEAF_KEYS_OFF) / (2 * sizeof(uint64_t));
    static constexpr size_t LEAF_VALS_OFF = LEAF_KEYS_OFF + LEAF_CAP * sizeof(uint64_t);
    static constexpr size_t INTERNAL_KEYS_OFF = 8;
    static constexpr size_t INTERNAL_CAP = (PAGE_SIZE - INTERNAL_KEYS_OFF - sizeof(uint64_t)) / (2 * sizeof(uint64_t));
    static constexpr size_t INTERNAL_CHILDREN_OFF = INTERNAL_KEYS_OFF + INTERNAL_CAP * sizeof(uint64_t);
    static constexpr uint32_t MAX_HEIGHT = 32;

    static uint32_t get_u32(uint8_t const* p, size_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
    static void put_u32(uint8_t* p, size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
    static uint64_t get_u64(uint8_t const* p, size_t off) { uint64_t v; std::memcpy(&v, p + off, 8); return v; }
    static void put_u64(uint8_t* p, size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }
    static uint64_t get_u64_at(uint8_t const* p, size_t i) { return get_u64(p, i * 8); }
    static void put_u64_at(uint8_t* p, size_t i, uint64_t v) { put_u64(p, i * 8, v); }

    static MetaPage read_meta(auto& trx)
    {
        if (trx.pages_count() == 0) return {};
        MetaPage m{};
        std::memcpy(&m, static_cast<uint8_t const*>(trx.get_page(0)), sizeof(m));
        return m;
    }

    static void write_meta(auto& trx, MetaPage const& m)
    {
        std::memcpy(static_cast<uint8_t*>(trx.get_mut_page(0)), &m, sizeof(m));
    }

    static uint64_t alloc_page(auto& trx)
    {
        uint64_t const p = static_cast<uint64_t>(trx.push_page());
        std::memset(static_cast<uint8_t*>(trx.get_mut_page(p)), 0, PAGE_SIZE);
        return p;
    }

    static uint32_t lb(uint8_t const* k, uint32_t n, uint64_t key)
    {
        uint32_t lo = 0, hi = n;
        while (lo < hi) { uint32_t mid = lo + ((hi - lo) >> 1); if (get_u64_at(k, mid) < key) lo = mid + 1; else hi = mid; }
        return lo;
    }

    static uint32_t ub(uint8_t const* k, uint32_t n, uint64_t key)
    {
        uint32_t lo = 0, hi = n;
        while (lo < hi) { uint32_t mid = lo + ((hi - lo) >> 1); if (key < get_u64_at(k, mid)) hi = mid; else lo = mid + 1; }
        return lo;
    }

    static void init_empty(auto& trx)
    {
        if (trx.pages_count() == 0) (void)trx.push_page();
        std::memset(static_cast<uint8_t*>(trx.get_mut_page(0)), 0, PAGE_SIZE);
    }

    static void insert(auto& trx, uint64_t key, uint64_t val)
    {
        MetaPage meta = read_meta(trx);
        if (meta.root_page == 0)
        {
            uint64_t const leaf = alloc_page(trx);
            uint8_t* ml = static_cast<uint8_t*>(trx.get_mut_page(leaf));
            put_u32(ml, NODE_IS_LEAF_OFF, 1);
            put_u32(ml, NODE_COUNT_OFF, 1);
            put_u64_at(ml + LEAF_KEYS_OFF, 0, key);
            put_u64_at(ml + LEAF_VALS_OFF, 0, val);
            meta = { leaf, leaf, 1, 1 };
            write_meta(trx, meta);
            return;
        }

        uint64_t path_pages[MAX_HEIGHT];
        uint32_t path_slots[MAX_HEIGHT];
        uint32_t depth = 0;
        uint64_t page_idx = meta.root_page;

        while (true)
        {
            uint8_t const* pg = static_cast<uint8_t const*>(trx.get_page(page_idx));
            uint32_t const count = get_u32(pg, NODE_COUNT_OFF);
            if (get_u32(pg, NODE_IS_LEAF_OFF)) break;
            if (depth >= MAX_HEIGHT) throw std::runtime_error("BTree height exceeded");
            uint32_t const slot = ub(pg + INTERNAL_KEYS_OFF, count, key);
            path_pages[depth] = page_idx;
            path_slots[depth++] = slot;
            page_idx = get_u64_at(pg + INTERNAL_CHILDREN_OFF, slot);
        }

        uint8_t const* leaf_ro = static_cast<uint8_t const*>(trx.get_page(page_idx));
        uint32_t const count = get_u32(leaf_ro, NODE_COUNT_OFF);
        uint32_t const pos = lb(leaf_ro + LEAF_KEYS_OFF, count, key);

        if (pos < count && get_u64_at(leaf_ro + LEAF_KEYS_OFF, pos) == key)
        {
            put_u64_at(static_cast<uint8_t*>(trx.get_mut_page(page_idx)) + LEAF_VALS_OFF, pos, val);
            return;
        }

        if (count < LEAF_CAP)
        {
            uint8_t* ml = static_cast<uint8_t*>(trx.get_mut_page(page_idx));
            uint8_t *lk = ml + LEAF_KEYS_OFF, *lv = ml + LEAF_VALS_OFF;
            if (pos < count)
            {
                std::memmove(lk + (pos + 1) * 8, lk + pos * 8, (count - pos) * 8);
                std::memmove(lv + (pos + 1) * 8, lv + pos * 8, (count - pos) * 8);
            }
            put_u64_at(lk, pos, key);
            put_u64_at(lv, pos, val);
            put_u32(ml, NODE_COUNT_OFF, count + 1);
            ++meta.count;
            write_meta(trx, meta);
            return;
        }

        uint64_t const right_leaf = alloc_page(trx);
        uint8_t* ml = static_cast<uint8_t*>(trx.get_mut_page(page_idx));
        uint8_t* mr = static_cast<uint8_t*>(trx.get_mut_page(right_leaf));
        uint8_t *lk = ml + LEAF_KEYS_OFF, *lv = ml + LEAF_VALS_OFF;
        uint8_t *rk = mr + LEAF_KEYS_OFF, *rv = mr + LEAF_VALS_OFF;

        constexpr uint32_t lc = static_cast<uint32_t>((LEAF_CAP + 1) / 2);
        constexpr uint32_t rc = static_cast<uint32_t>(LEAF_CAP + 1 - lc);

        if (pos < lc)
        {
            std::memcpy(rk, lk + (lc - 1) * 8, rc * 8);
            std::memcpy(rv, lv + (lc - 1) * 8, rc * 8);
            if (pos < lc - 1)
            {
                std::memmove(lk + (pos + 1) * 8, lk + pos * 8, (lc - 1 - pos) * 8);
                std::memmove(lv + (pos + 1) * 8, lv + pos * 8, (lc - 1 - pos) * 8);
            }
            put_u64_at(lk, pos, key);
            put_u64_at(lv, pos, val);
        }
        else
        {
            uint32_t const rpos = pos - lc;
            if (rpos > 0)
            {
                std::memcpy(rk, lk + lc * 8, rpos * 8);
                std::memcpy(rv, lv + lc * 8, rpos * 8);
            }
            put_u64_at(rk, rpos, key);
            put_u64_at(rv, rpos, val);
            if (pos < LEAF_CAP)
            {
                std::memcpy(rk + (rpos + 1) * 8, lk + pos * 8, (LEAF_CAP - pos) * 8);
                std::memcpy(rv + (rpos + 1) * 8, lv + pos * 8, (LEAF_CAP - pos) * 8);
            }
        }

        put_u32(mr, NODE_IS_LEAF_OFF, 1);
        put_u32(mr, NODE_COUNT_OFF, rc);
        put_u64(mr, LEAF_NEXT_OFF, get_u64(ml, LEAF_NEXT_OFF));
        put_u32(ml, NODE_COUNT_OFF, lc);
        put_u64(ml, LEAF_NEXT_OFF, right_leaf);

        uint64_t pkey = get_u64_at(rk, 0), pchild = right_leaf;

        while (depth > 0)
        {
            --depth;
            uint64_t const ppage = path_pages[depth];
            uint32_t const slot = path_slots[depth];
            uint8_t* mp = static_cast<uint8_t*>(trx.get_mut_page(ppage));
            uint32_t const pcount = get_u32(mp, NODE_COUNT_OFF);

            if (pcount < INTERNAL_CAP)
            {
                uint8_t *pk = mp + INTERNAL_KEYS_OFF, *pc = mp + INTERNAL_CHILDREN_OFF;
                if (slot < pcount)
                {
                    std::memmove(pk + (slot + 1) * 8, pk + slot * 8, (pcount - slot) * 8);
                    std::memmove(pc + (slot + 2) * 8, pc + (slot + 1) * 8, (pcount - slot) * 8);
                }
                put_u64_at(pk, slot, pkey);
                put_u64_at(pc, slot + 1, pchild);
                put_u32(mp, NODE_COUNT_OFF, pcount + 1);
                ++meta.count;
                write_meta(trx, meta);
                return;
            }

            uint64_t const rint = alloc_page(trx);
            mp = static_cast<uint8_t*>(trx.get_mut_page(ppage));
            uint8_t* mri = static_cast<uint8_t*>(trx.get_mut_page(rint));
            uint8_t *pk = mp + INTERNAL_KEYS_OFF, *pc = mp + INTERNAL_CHILDREN_OFF;

            uint64_t tk[INTERNAL_CAP + 1], tc[INTERNAL_CAP + 2];
            if (slot > 0) std::memcpy(tk, pk, slot * 8);
            tk[slot] = pkey;
            if (slot < INTERNAL_CAP) std::memcpy(tk + slot + 1, pk + slot * 8, (INTERNAL_CAP - slot) * 8);

            std::memcpy(tc, pc, (slot + 1) * 8);
            tc[slot + 1] = pchild;
            if (slot < INTERNAL_CAP) std::memcpy(tc + slot + 2, pc + (slot + 1) * 8, (INTERNAL_CAP - slot) * 8);

            constexpr uint32_t mid = static_cast<uint32_t>((INTERNAL_CAP + 1) / 2);
            constexpr uint32_t irc = static_cast<uint32_t>(INTERNAL_CAP - mid);
            std::memcpy(pk, tk, mid * 8);
            std::memcpy(pc, tc, (mid + 1) * 8);
            put_u32(mp, NODE_COUNT_OFF, mid);

            std::memcpy(mri + INTERNAL_KEYS_OFF, tk + mid + 1, irc * 8);
            std::memcpy(mri + INTERNAL_CHILDREN_OFF, tc + mid + 1, (irc + 1) * 8);
            put_u32(mri, NODE_IS_LEAF_OFF, 0);
            put_u32(mri, NODE_COUNT_OFF, irc);

            pkey = tk[mid];
            pchild = rint;
        }

        uint64_t const nroot = alloc_page(trx);
        uint8_t* mroot = static_cast<uint8_t*>(trx.get_mut_page(nroot));
        put_u32(mroot, NODE_IS_LEAF_OFF, 0);
        put_u32(mroot, NODE_COUNT_OFF, 1);
        put_u64_at(mroot + INTERNAL_KEYS_OFF, 0, pkey);
        put_u64_at(mroot + INTERNAL_CHILDREN_OFF, 0, meta.root_page);
        put_u64_at(mroot + INTERNAL_CHILDREN_OFF, 1, pchild);
        meta.root_page = nroot;
        ++meta.height;
        ++meta.count;
        write_meta(trx, meta);
    }

    static std::optional<uint64_t> find(auto& trx, uint64_t key)
    {
        if (trx.pages_count() == 0) return std::nullopt;
        MetaPage const meta = read_meta(trx);
        if (meta.root_page == 0) return std::nullopt;

        uint64_t page_idx = meta.root_page;
        while (true)
        {
            uint8_t const* pg = static_cast<uint8_t const*>(trx.get_page(page_idx));
            uint32_t const count = get_u32(pg, NODE_COUNT_OFF);
            if (get_u32(pg, NODE_IS_LEAF_OFF))
            {
                uint32_t const pos = lb(pg + LEAF_KEYS_OFF, count, key);
                if (pos < count && get_u64_at(pg + LEAF_KEYS_OFF, pos) == key)
                    return get_u64_at(pg + LEAF_VALS_OFF, pos);
                return std::nullopt;
            }
            page_idx = get_u64_at(pg + INTERNAL_CHILDREN_OFF, ub(pg + INTERNAL_KEYS_OFF, count, key));
        }
    }
};

static Db mdb;

// ---------------- SQLite helpers ----------------

struct DbGuard { sqlite3* db = nullptr; ~DbGuard() { if (db) sqlite3_close(db); } };
struct StmtGuard { sqlite3_stmt* stmt = nullptr; ~StmtGuard() { if (stmt) sqlite3_finalize(stmt); } };

[[noreturn]] void throw_sqlite(sqlite3* db, char const* op)
{
    throw std::runtime_error(std::string(op) + ": " + (db ? sqlite3_errmsg(db) : "null db"));
}

inline void check_sqlite(int rc, sqlite3* db, char const* op)
{
    if (rc != SQLITE_OK) [[unlikely]] throw_sqlite(db, op);
}

inline void exec_sql(sqlite3* db, char const* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        std::string msg = std::string(sql) + " -> " + (err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

inline void prepare(sqlite3* db, StmtGuard& g, char const* sql, size_t sz)
{
    check_sqlite(sqlite3_prepare_v3(db, sql, static_cast<int>(sz), SQLITE_PREPARE_PERSISTENT, &g.stmt, nullptr), db, sql);
}

inline void bind_key(sqlite3* db, sqlite3_stmt* stmt, char const* uname, bool int_key)
{
    if (int_key)
        check_sqlite(sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(pack_key(uname))), db, "bind_int64(key)");
    else
        check_sqlite(sqlite3_bind_text(stmt, 1, uname, 8, SQLITE_STATIC), db, "bind_text(key)");
}

sqlite3* open_sqlite_conn(char const* path, Cfg const& cfg, bool setup)
{
    sqlite3* db = nullptr;
    check_sqlite(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr), db, "open");
    sqlite3_busy_timeout(db, 5000);
    if (setup)
    {
        exec_sql(db, "PRAGMA page_size = 8192;");
        exec_sql(db, "PRAGMA journal_mode = WAL;");
        exec_sql(db, "PRAGMA locking_mode = NORMAL;");
    }
    exec_sql(db, (!setup && cfg.sync) ? "PRAGMA synchronous = NORMAL;" : "PRAGMA synchronous = OFF;");
    exec_sql(db, "PRAGMA cache_size = -65536;");
    exec_sql(db, "PRAGMA mmap_size = 2147483648;");
    exec_sql(db, "PRAGMA temp_store = MEMORY;");
    return db;
}

// ---------------- Shared reader & writer loops ----------------

template <class LookupFn>
void reader_loop(uint32_t tid, Cfg const& cfg, std::atomic<int> const& phase, std::atomic<uint32_t>& ready, ReadRes& res, LookupFn&& lookup)
{
    Rng rng(0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(tid) + 1) * 0xBF58476D1CE4E5B9ULL);
    ready.fetch_add(1, std::memory_order_release);
    while (phase.load(std::memory_order_acquire) < 0) std::this_thread::yield();

    uint64_t iters = 0;
    while (true)
    {
        int const p = phase.load(std::memory_order_relaxed);
        if (p == 2) break;

        bool const is_miss = cfg.miss_pct > 0 && (rng.next() % 100) < cfg.miss_pct;
        uint64_t const id = is_miss ? (MISSING_BASE + (rng.next() % cfg.users)) : pick_hit_id(rng, cfg.users, cfg.skew);
        char uname[8];
        make_username(id, uname);

        bool const sample = (p == 1) && ((iters & 63) == 0);
        auto const t0 = sample ? Clock::now() : Clock::time_point{};

        auto const val = lookup(uname);
        if (is_miss)
        {
            if (val.has_value()) throw std::runtime_error("Verification failed: hit on missing key");
        }
        else
        {
            if (!val.has_value()) throw std::runtime_error("Verification failed: missing existing key");
            if (id % 10 != 0)
            {
                if (*val != immutable_val(id)) throw std::runtime_error("Verification failed: immutable mismatch");
            }
            else if (!verify_mutable_val(id, *val))
            {
                throw std::runtime_error("Verification failed: mutable corruption");
            }
            res.checksum += *val;
        }

        if (sample)
        {
            int64_t const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
            res.hist.add(ns > 0 ? static_cast<uint64_t>(ns) : 1);
        }
        if (p == 1)
        {
            ++res.ops;
            is_miss ? ++res.misses : ++res.hits;
        }
        ++iters;
    }
}

template <class BatchFn>
void writer_loop(Cfg const& cfg, std::atomic<int> const& phase, std::atomic<uint32_t>& ready, WriteRes& res, BatchFn&& run_batch)
{
    Rng rng(0xD6E8FEB86659FD93ULL);
    uint64_t const num_mut = (cfg.users + 9) / 10;
    uint64_t new_users = 0;
    uint32_t epoch = 1;

    auto next_mut = [&]()
    {
        bool const is_insert = (rng.next() % 5) == 0;
        uint64_t const id = is_insert ? (cfg.users + new_users++) : ((rng.next() % num_mut) * 10);
        std::array<char, 8> uname{};
        make_username(id, uname.data());
        return std::pair{ uname, mutable_val(id, epoch++) };
    };

    ready.fetch_add(1, std::memory_order_release);
    while (phase.load(std::memory_order_acquire) < 0) std::this_thread::yield();

    if (cfg.batch == 0)
    {
        while (phase.load(std::memory_order_relaxed) != 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    uint64_t const batch_ns = cfg.write_rate > 0
        ? static_cast<uint64_t>((static_cast<double>(cfg.batch) * 1e9) / static_cast<double>(cfg.write_rate))
        : 0;
    auto deadline = Clock::now();

    while (true)
    {
        int const p = phase.load(std::memory_order_relaxed);
        if (p == 2) break;

        run_batch(next_mut);
        if (p == 1) { res.ops += cfg.batch; ++res.commits; }

        if (batch_ns > 0)
        {
            deadline += std::chrono::nanoseconds(batch_ns);
            auto now = Clock::now();
            if (now - deadline > std::chrono::milliseconds(100)) deadline = now;
            while (phase.load(std::memory_order_relaxed) != 2 && (now = Clock::now()) < deadline)
            {
                auto const rem = deadline - now;
                std::this_thread::sleep_for(rem < std::chrono::milliseconds(25) ? rem : std::chrono::milliseconds(25));
            }
        }
    }
}

// ---------------- CLI ----------------

Cfg parse_args(int argc, char** argv)
{
    Cfg c;
    auto need = [&](int& i) -> char const*
    {
        if (++i >= argc) throw std::runtime_error(std::string("Missing value for ") + argv[i - 1]);
        return argv[i];
    };

    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--engine") c.engine = need(i);
        else if (a == "--threads") c.threads = static_cast<uint32_t>(std::stoul(need(i)));
        else if (a == "--users") c.users = std::stoull(need(i));
        else if (a == "--warmup") c.warmup = static_cast<uint32_t>(std::stoul(need(i)));
        else if (a == "--duration") c.duration = static_cast<uint32_t>(std::stoul(need(i)));
        else if (a == "--batch") c.batch = static_cast<uint32_t>(std::stoul(need(i)));
        else if (a == "--write-rate") c.write_rate = std::stoull(need(i));
        else if (a == "--miss-pct") c.miss_pct = static_cast<uint32_t>(std::stoul(need(i)));
        else if (a == "--skew") c.skew = std::stod(need(i));
        else if (a == "--sync") c.sync = std::stoul(need(i)) != 0;
        else if (a == "--int-key") c.int_key = std::stoul(need(i)) != 0;
        else if (a == "--db") c.db_path = need(i);
        else
        {
            std::cerr << "Usage: " << argv[0] << " --engine <sqlite|megazine> [options]\n"
                      << "  --threads <N>     reader threads (default 4)\n"
                      << "  --users <N>       initial users (default 1000000)\n"
                      << "  --warmup <sec>    warmup seconds (default 2)\n"
                      << "  --duration <sec>  timed seconds (default 10)\n"
                      << "  --batch <N>       writes/commit, 0=read-only (default 100)\n"
                      << "  --write-rate <N>  target writes/s, 0=unpaced (default 10000)\n"
                      << "  --miss-pct <0..100> missing key % (default 5)\n"
                      << "  --skew <theta>    7.21=80/20, 1.0=uniform (default 7.21)\n"
                      << "  --sync <0|1>      SQLite synchronous OFF/NORMAL (default 0)\n"
                      << "  --int-key <0|1>   SQLite INTEGER PK vs TEXT WITHOUT ROWID (default 0)\n"
                      << "  --db <path>       database file path\n";
            std::exit(a == "--help" ? 0 : 1);
        }
    }
    if (c.engine != "sqlite" && c.engine != "megazine")
        throw std::runtime_error("Mandatory --engine <sqlite|megazine> missing or invalid");
    if (c.threads == 0 || c.users == 0 || c.duration == 0 || c.miss_pct > 100)
        throw std::runtime_error("Invalid numeric parameter");
    if (c.db_path.empty())
        c.db_path = (c.engine == "sqlite") ? "bench_sqlite.db" : "bench_megazine.db";
    return c;
}

void entry_point(int argc, char** argv)
{
    Cfg const cfg = parse_args(argc, argv);

    std::error_code ec;
    std::filesystem::remove(cfg.db_path, ec);
    std::filesystem::remove(cfg.db_path + "-wal", ec);
    std::filesystem::remove(cfg.db_path + "-shm", ec);
    std::filesystem::remove(cfg.db_path + "-journal", ec);

    bool const use_sqlite = (cfg.engine == "sqlite");
    if (use_sqlite && sqlite3_threadsafe() == 0)
        throw std::runtime_error("SQLite must be compiled with -DSQLITE_THREADSAFE=2 (or 1)");
    if (!use_sqlite)
    {
        mdb = Db::open(cfg.db_path.c_str(), {
            .page_size = PAGE_SIZE,
            .initial_pages_capacity_reader = 512*10,
            .initial_pages_capacity_writer = 512*10,
            .initial_roots_pool_capacity = 100,

            .commit_atomically = cfg.sync,
            .threads_count = 1 + cfg.threads
        });
    }
    
    std::atomic<bool> init_done{ false };
    std::atomic<uint32_t> ready{ 0 };
    std::atomic<int> phase{ -1 };
    double load_ms = 0.0;
    WriteRes wres;

    std::thread writer([&]
    {
        try
        {
            auto const t0 = Clock::now();
            if (use_sqlite)
            {
                DbGuard dbg{ open_sqlite_conn(cfg.db_path.c_str(), cfg, true) };
                exec_sql(dbg.db, cfg.int_key
                    ? "CREATE TABLE users (username INTEGER PRIMARY KEY, magic_number INTEGER NOT NULL);"
                    : "CREATE TABLE users (username TEXT PRIMARY KEY, magic_number INTEGER NOT NULL) WITHOUT ROWID;");
                StmtGuard ins;
                static constexpr char load_sql[] = "INSERT INTO users (username, magic_number) VALUES (?, ?);";
                prepare(dbg.db, ins, load_sql, sizeof(load_sql));

                for (uint64_t base = 0; base < cfg.users; base += 100'000)
                {
                    exec_sql(dbg.db, "BEGIN IMMEDIATE;");
                    uint64_t const end = std::min(cfg.users, base + 100'000);
                    for (uint64_t id = base; id < end; ++id)
                    {
                        char uname[8];
                        make_username(id, uname);
                        bind_key(dbg.db, ins.stmt, uname, cfg.int_key);
                        uint64_t const val = (id % 10 != 0) ? immutable_val(id) : mutable_val(id, 0);
                        check_sqlite(sqlite3_bind_int64(ins.stmt, 2, static_cast<sqlite3_int64>(val)), dbg.db, "bind_val");
                        if (sqlite3_step(ins.stmt) != SQLITE_DONE) throw_sqlite(dbg.db, "step(load)");
                        check_sqlite(sqlite3_reset(ins.stmt), dbg.db, "reset(load)");
                    }
                    exec_sql(dbg.db, "COMMIT;");
                }
            }
            else
            {
                for (uint64_t base = 0; base < cfg.users; base += 100'000)
                {
                    auto trx = mdb.trx_w(0);
                    DEFER_close(trx);
                    if (base == 0) MegazineBTree::init_empty(trx);
                    uint64_t const end = std::min(cfg.users, base + 100'000);
                    for (uint64_t id = base; id < end; ++id)
                    {
                        char uname[8];
                        make_username(id, uname);
                        uint64_t const val = (id % 10 != 0) ? immutable_val(id) : mutable_val(id, 0);
                        MegazineBTree::insert(trx, pack_key(uname), val);
                    }
                }
            }
            load_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            init_done.store(true, std::memory_order_release);

            if (use_sqlite)
            {
                DbGuard dbg{ open_sqlite_conn(cfg.db_path.c_str(), cfg, false) };
                StmtGuard beg, com, ups;
                static constexpr char beg_sql[] = "BEGIN IMMEDIATE;";
                static constexpr char com_sql[] = "COMMIT;";
                static constexpr char ups_sql[] =
                    "INSERT INTO users (username, magic_number) VALUES (?, ?) "
                    "ON CONFLICT(username) DO UPDATE SET magic_number = excluded.magic_number;";
                prepare(dbg.db, beg, beg_sql, sizeof(beg_sql));
                prepare(dbg.db, com, com_sql, sizeof(com_sql));
                prepare(dbg.db, ups, ups_sql, sizeof(ups_sql));

                writer_loop(cfg, phase, ready, wres, [&](auto&& next_mut)
                {
                    if (sqlite3_step(beg.stmt) != SQLITE_DONE) throw_sqlite(dbg.db, "BEGIN");
                    check_sqlite(sqlite3_reset(beg.stmt), dbg.db, "reset(BEGIN)");
                    for (uint32_t i = 0; i < cfg.batch; ++i)
                    {
                        auto const [uname, val] = next_mut();
                        bind_key(dbg.db, ups.stmt, uname.data(), cfg.int_key);
                        check_sqlite(sqlite3_bind_int64(ups.stmt, 2, static_cast<sqlite3_int64>(val)), dbg.db, "bind_val");
                        if (sqlite3_step(ups.stmt) != SQLITE_DONE) throw_sqlite(dbg.db, "UPSERT");
                        check_sqlite(sqlite3_reset(ups.stmt), dbg.db, "reset(UPSERT)");
                    }
                    if (sqlite3_step(com.stmt) != SQLITE_DONE) throw_sqlite(dbg.db, "COMMIT");
                    check_sqlite(sqlite3_reset(com.stmt), dbg.db, "reset(COMMIT)");
                });
            }
            else
            {
                writer_loop(cfg, phase, ready, wres, [&](auto&& next_mut)
                {
                    auto trx = mdb.trx_w(0);
                    DEFER_close(trx);
                    for (uint32_t i = 0; i < cfg.batch; ++i)
                    {
                        auto const [uname, val] = next_mut();
                        MegazineBTree::insert(trx, pack_key(uname.data()), val);
                    }
                });
            }
        }
        catch (std::exception const& e)
        {
            std::cerr << "Writer error: " << e.what() << '\n';
            std::exit(1);
        }
    });

    while (!init_done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::vector<ReadRes> rres(cfg.threads);
    std::vector<std::thread> readers;
    readers.reserve(cfg.threads);

    for (uint32_t t = 0; t < cfg.threads; ++t)
    {
        readers.emplace_back([&, t, tid = t + 1]
        {
            try
            {
                if (use_sqlite)
                {
                    DbGuard dbg{ open_sqlite_conn(cfg.db_path.c_str(), cfg, false) };
                    StmtGuard sel;
                    static constexpr char sel_sql[] = "SELECT magic_number FROM users WHERE username = ?;";
                    prepare(dbg.db, sel, sel_sql, sizeof(sel_sql));

                    reader_loop(tid, cfg, phase, ready, rres[t], [&](char const* uname) -> std::optional<uint64_t>
                    {
                        bind_key(dbg.db, sel.stmt, uname, cfg.int_key);
                        int const rc = sqlite3_step(sel.stmt);
                        std::optional<uint64_t> out;
                        if (rc == SQLITE_ROW) out = static_cast<uint64_t>(sqlite3_column_int64(sel.stmt, 0));
                        else if (rc != SQLITE_DONE) throw_sqlite(dbg.db, "SELECT");
                        check_sqlite(sqlite3_reset(sel.stmt), dbg.db, "reset(SELECT)");
                        return out;
                    });
                }
                else
                {
                    reader_loop(tid, cfg, phase, ready, rres[t], [&](char const* uname) -> std::optional<uint64_t>
                    {
                        auto trx = mdb.trx_r(static_cast<thread_id_t>(tid));
                        DEFER_close(trx);
                        return MegazineBTree::find(trx, pack_key(uname));
                    });
                }
            }
            catch (std::exception const& e)
            {
                std::cerr << "Reader " << tid << " error: " << e.what() << '\n';
                std::exit(1);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < 1 + cfg.threads) std::this_thread::yield();

    if (cfg.warmup > 0)
    {
        phase.store(0, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::seconds(cfg.warmup));
    }

    auto const t0 = Clock::now();
    phase.store(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(cfg.duration));
    phase.store(2, std::memory_order_release);
    auto const t1 = Clock::now();

    writer.join();
    for (auto& th : readers) th.join();

    double const sec = std::chrono::duration<double>(t1 - t0).count();
    ReadRes tot;
    for (auto const& r : rres)
    {
        tot.ops += r.ops;
        tot.hits += r.hits;
        tot.misses += r.misses;
        tot.checksum += r.checksum;
        tot.hist.merge(r.hist);
    }

    std::cout << "[" << cfg.engine << "]\n"
              << "  config:       readers=" << cfg.threads << " users=" << cfg.users
              << " batch=" << cfg.batch << " write_rate=" << cfg.write_rate
              << " skew=" << cfg.skew << " miss=" << cfg.miss_pct << "%";
    if (use_sqlite) std::cout << " key=" << (cfg.int_key ? "INT" : "TEXT") << " sync=" << (cfg.sync ? "NORMAL" : "OFF");
    std::cout << "\n"
              << "  load:         " << load_ms << " ms\n"
              << "  elapsed:      " << sec << " s\n"
              << "  reads/sec:    " << static_cast<uint64_t>(tot.ops / sec)
              << " (total=" << tot.ops << ", hits=" << tot.hits << ", misses=" << tot.misses << ")\n"
              << "  writes/sec:   " << static_cast<uint64_t>(wres.ops / sec)
              << " (" << static_cast<uint64_t>(wres.commits / sec) << " commits/s, total=" << wres.ops << ")\n"
              << "  read latency: p50=" << tot.hist.pct(0.50) << " ns, p99=" << tot.hist.pct(0.99)
              << " ns, p99.9=" << tot.hist.pct(0.999) << " ns\n"
              << "  checksum:     " << tot.checksum << "\n";

    if (!use_sqlite)
        mdb.close();
}

int main(int argc, char** argv)
{
    entry_point(argc, argv);
    return 0;
}