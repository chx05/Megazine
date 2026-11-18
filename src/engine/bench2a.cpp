// gemini 4 hydrogen wrote this benchmark
// hashmap username lookup

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "megazine_engine.hpp"

constexpr uint64_t PAGE_SIZE = 4096;

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 1000000;
constexpr size_t NUM_QUERIES = 100;

struct User
{
    char username[USERNAME_LEN];
    uint64_t magic_number;
};

struct MegazineSoaMap
{
    struct Header
    {
        uint64_t count;
        uint64_t cap;
    };

    static_assert(PAGE_SIZE >= sizeof(Header) && (PAGE_SIZE % sizeof(uint64_t)) == 0,
        "PAGE_SIZE must be >= 16 and a multiple of 8");

    static constexpr size_t U64_PER_PAGE = PAGE_SIZE / sizeof(uint64_t);

    static uint64_t pack_key(char const* s)
    {
        uint64_t k = 0;
        std::memcpy(&k, s, USERNAME_LEN);
        return k;
    }

    static uint64_t hash_u64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    static uint64_t cap_for(uint64_t expected_count)
    {
        uint64_t need = 16;
        while (need * 3 <= expected_count * 4)
        {
            need <<= 1;
        }
        return need;
    }

    static uint64_t region_pages(uint64_t cap)
    {
        return (cap + U64_PER_PAGE - 1) / U64_PER_PAGE;
    }

    static Header read_header(auto& trx)
    {
        if (trx.pages_count() == 0)
        {
            return { 0, 0 };
        }
        Header h{};
        std::memcpy(&h, trx.get_page(0), sizeof(Header));
        return h;
    }

    static void write_header(auto& trx, Header h)
    {
        std::memcpy(trx.get_mut_page(0), &h, sizeof(Header));
    }

    static uint64_t read_u64(auto& trx, uint64_t base_page, uint64_t idx)
    {
        uint8_t const* p = trx.get_page(base_page + idx / U64_PER_PAGE);
        uint64_t v = 0;
        std::memcpy(&v, p + (idx % U64_PER_PAGE) * sizeof(uint64_t), sizeof(uint64_t));
        return v;
    }

    static void write_u64(auto& trx, uint64_t base_page, uint64_t idx, uint64_t v)
    {
        uint8_t* p = trx.get_mut_page(base_page + idx / U64_PER_PAGE);
        std::memcpy(p + (idx % U64_PER_PAGE) * sizeof(uint64_t), &v, sizeof(uint64_t));
    }

    static void preallocate(auto& database, uint64_t expected_count)
    {
        if constexpr (requires { database.modify_file_size(uint64_t{ 0 }); })
        {
            uint64_t const target_pages = 1 + 2 * region_pages(cap_for(expected_count));
            database.modify_file_size(PAGE_SIZE * target_pages);
        }
        (void)database;
        (void)expected_count;
    }

    static void resize_pages(auto& trx, uint64_t target_pages)
    {
        if constexpr (requires { trx.modify_file_size(uint64_t{ 0 }); })
        {
            if (trx.pages_count() != target_pages)
            {
                trx.modify_file_size(PAGE_SIZE * target_pages);
            }
        }
        else
        {
            while (trx.pages_count() < target_pages)
            {
                trx.push_page();
            }
            if constexpr (requires { trx.pop_page(); })
            {
                while (trx.pages_count() > target_pages)
                {
                    trx.pop_page();
                }
            }
        }
    }

    // Fast path for initializing a fresh DB without reading page 0 first
    static void init_empty(auto& trx, uint64_t expected_count)
    {
        uint64_t const cap = cap_for(expected_count);
        uint64_t const target_pages = 1 + 2 * region_pages(cap);
        resize_pages(trx, target_pages);
        for (uint64_t p = 0; p < target_pages; ++p)
        {
            std::memset(trx.get_mut_page(p), 0, PAGE_SIZE);
        }
        write_header(trx, Header{ 0, cap });
    }

    static void rehash(auto& trx, uint64_t new_cap)
    {
        Header const old = read_header(trx);
        uint64_t const old_rpages = region_pages(old.cap);

        std::vector<std::pair<uint64_t, uint64_t>> entries;
        if (old.cap > 0)
        {
            entries.reserve(old.count);
            for (uint64_t i = 0; i < old.cap; ++i)
            {
                uint64_t const k = read_u64(trx, 1, i);
                if (k != 0)
                {
                    entries.emplace_back(k, read_u64(trx, 1 + old_rpages, i));
                }
            }
        }

        uint64_t const new_rpages = region_pages(new_cap);
        uint64_t const target_pages = 1 + 2 * new_rpages;
        resize_pages(trx, target_pages);

        for (uint64_t p = 0; p < target_pages; ++p)
        {
            std::memset(trx.get_mut_page(p), 0, PAGE_SIZE);
        }

        uint64_t const mask = new_cap - 1;
        for (auto const& [k, v] : entries)
        {
            uint64_t idx = hash_u64(k) & mask;
            while (read_u64(trx, 1, idx) != 0)
            {
                idx = (idx + 1) & mask;
            }
            write_u64(trx, 1, idx, k);
            write_u64(trx, 1 + new_rpages, idx, v);
        }
        write_header(trx, Header{ entries.size(), new_cap });
    }

    static void insert(auto& trx, uint64_t key, uint64_t val)
    {
        Header hdr = read_header(trx);
        if (hdr.cap == 0)
        {
            rehash(trx, 16);
            hdr = read_header(trx);
        }
        else if ((hdr.count + 1) * 4 >= hdr.cap * 3)
        {
            rehash(trx, hdr.cap * 2);
            hdr = read_header(trx);
        }

        uint64_t const rpages = region_pages(hdr.cap);
        uint64_t const mask = hdr.cap - 1;
        uint64_t idx = hash_u64(key) & mask;

        while (true)
        {
            uint64_t const cur = read_u64(trx, 1, idx);
            if (cur == 0)
            {
                write_u64(trx, 1, idx, key);
                write_u64(trx, 1 + rpages, idx, val);
                ++hdr.count;
                write_header(trx, hdr);
                return;
            }
            if (cur == key)
            {
                write_u64(trx, 1 + rpages, idx, val);
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    static std::optional<uint64_t> find(auto& trx, uint64_t key)
    {
        Header const hdr = read_header(trx);
        if (hdr.cap == 0)
        {
            return std::nullopt;
        }
        uint64_t const rpages = region_pages(hdr.cap);
        uint64_t const mask = hdr.cap - 1;
        uint64_t idx = hash_u64(key) & mask;

        while (true)
        {
            uint64_t const cur = read_u64(trx, 1, idx);
            if (cur == key)
            {
                return read_u64(trx, 1 + rpages, idx);
            }
            if (cur == 0)
            {
                return std::nullopt;
            }
            idx = (idx + 1) & mask;
        }
    }

    static std::pair<uint64_t, uint64_t> sum_and_avg(auto& trx)
    {
        Header const hdr = read_header(trx);
        if (hdr.count == 0)
        {
            return { 0, 0 };
        }
        uint64_t const rpages = region_pages(hdr.cap);
        uint64_t const vals_base = 1 + rpages;

        uint64_t counter = 0;
        uint64_t remaining = hdr.cap;
        for (uint64_t p = 0; p < rpages; ++p)
        {
            uint8_t const* page = trx.get_page(vals_base + p);
            size_t const n = std::min<uint64_t>(remaining, U64_PER_PAGE);
            for (size_t i = 0; i < n; ++i)
            {
                uint64_t v = 0;
                std::memcpy(&v, page + i * sizeof(uint64_t), sizeof(uint64_t));
                counter += v;
            }
            remaining -= n;
        }
        return { hdr.count, counter / hdr.count };
    }
};

static Db db;

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
    for (size_t i = 0; i < NUM_USERS; ++i)
    {
        for (size_t c = 0; c < USERNAME_LEN; ++c)
        {
            users[i].username[c] = alphabet[char_dist(rng)];
        }
        users[i].magic_number = magic_dist(rng);
    }
    return users;
}

int main()
{
    constexpr char const* DB_PATH = "test2_soamap";
    constexpr thread_id_t THREAD_ID = 0;

    std::error_code ec;
    std::filesystem::remove(DB_PATH, ec);

    auto const users = generate_users();
    uint64_t expected_sum = 0;
    for (auto const& u : users)
    {
        expected_sum += u.magic_number;
    }
    uint64_t const expected_avg = expected_sum / NUM_USERS;

    db = Db::open(DB_PATH, 1, { .page_size = PAGE_SIZE });
    DEFER_close(db);

    // 1) Write phase: pre-size DB and populate SoA hashmap
    auto const t_write_start = std::chrono::steady_clock::now();
    MegazineSoaMap::preallocate(db, NUM_USERS);
    {
        auto trx = db.trx_w(THREAD_ID);
        DEFER_close(trx);

        MegazineSoaMap::init_empty(trx, NUM_USERS);
        for (auto const& u : users)
        {
            MegazineSoaMap::insert(trx, MegazineSoaMap::pack_key(u.username), u.magic_number);
        }
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Full-scan phase
    uint64_t last_count = 0;
    uint64_t last_avg = 0;
    uint64_t checksum = 0;

    auto const t_scan_start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < NUM_QUERIES; ++q)
    {
        auto trx = db.trx_r(THREAD_ID);
        DEFER_close(trx);

        auto const [cnt, avg] = MegazineSoaMap::sum_and_avg(trx);
        last_count = cnt;
        last_avg = avg;
        checksum += avg;
    }
    auto const t_scan_end = std::chrono::steady_clock::now();

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Scan verification failed!");
    }

    // 3) Point-lookup phase (100k O(1) lookups)
    uint64_t lookup_checksum = 0;
    auto const t_lookup_start = std::chrono::steady_clock::now();
    for (auto const& u : users)
    {
        auto trx = db.trx_r(THREAD_ID);
        DEFER_close(trx);

        auto const res = MegazineSoaMap::find(trx, MegazineSoaMap::pack_key(u.username));
        if (!res || *res != u.magic_number)
        {
            throw std::runtime_error("Lookup verification failed!");
        }
        lookup_checksum += *res;
    }
    auto const t_lookup_end = std::chrono::steady_clock::now();

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const scan_us = std::chrono::duration<double, std::micro>(t_scan_end - t_scan_start).count();
    auto const lookup_us = std::chrono::duration<double, std::micro>(t_lookup_end - t_lookup_start).count();

    std::cout << "[megazine soa-hashmap]\n"
        << "  records:        " << last_count << '\n'
        << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
        << "  write (" << NUM_USERS << "):  " << write_us / 1000.0 << " ms\n"
        << "  scan (" << NUM_QUERIES << "x):    " << scan_us / 1000.0 << " ms (" << scan_us / NUM_QUERIES << " us/query)\n"
        << "  lookup (" << NUM_USERS << "x): " << lookup_us / 1000.0 << " ms (" << (lookup_us * 1000.0) / NUM_USERS << " ns/lookup, check=" << lookup_checksum << ")\n";

    return 0;
}