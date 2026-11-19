// list full scan (optimized)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include "megazine_engine.hpp"

struct User
{
    char username[8];
    uint64_t magic_number;
};

constexpr uint64_t PAGE_SIZE = 4096 * 512;

// false = keep exact AoS User[] layout inside each 2 MB page
// true  = columnar SoA layout inside each 2 MB page (half read bandwidth + SIMD)
constexpr bool USE_SOA_PAGE_LAYOUT = true;

static_assert(sizeof(User) == 16);
static_assert(std::is_trivially_copyable_v<User>);
static_assert(PAGE_SIZE >= sizeof(User), "PAGE_SIZE must fit at least one User");

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 100'000'000;
constexpr size_t NUM_QUERIES = 10;
constexpr size_t USERS_PER_PAGE = PAGE_SIZE / sizeof(User);

struct alignas(64) SoaPage
{
    static constexpr size_t HEADER_BYTES = 64;
    // Multiple of 8 keeps magic_numbers 64-byte aligned
    static constexpr size_t CAPACITY =
        ((PAGE_SIZE - HEADER_BYTES) / sizeof(User)) & ~size_t{7};

    uint64_t count;
    uint8_t pad[HEADER_BYTES - sizeof(uint64_t)];
    std::array<char[USERNAME_LEN], CAPACITY> usernames;
    std::array<uint64_t, CAPACITY> magic_numbers;
};

static_assert(sizeof(SoaPage) <= PAGE_SIZE);
static_assert(offsetof(SoaPage, magic_numbers) % 64 == 0);

constexpr size_t PAGE_CAPACITY =
    USE_SOA_PAGE_LAYOUT ? SoaPage::CAPACITY : USERS_PER_PAGE;
constexpr size_t PAGES_NEEDED =
    (NUM_USERS + PAGE_CAPACITY - 1) / PAGE_CAPACITY;

static Db db;

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
    constexpr char const* DB_PATH = "bench1.db";
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

    // 1) Write phase
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        auto trx = db.trx_w(THREAD_ID);
        DEFER_close(trx);

        trx.modify_file_size(PAGES_NEEDED * PAGE_SIZE);

        for (uint64_t page_idx = 0; page_idx < PAGES_NEEDED; ++page_idx)
        {
            uint8_t* page = static_cast<uint8_t*>(trx.get_mut_page(page_idx));
            size_t const offset = static_cast<size_t>(page_idx) * PAGE_CAPACITY;
            size_t const count = std::min(PAGE_CAPACITY, NUM_USERS - offset);

            if constexpr (USE_SOA_PAGE_LAYOUT)
            {
                uint64_t const cnt = count;
                std::memcpy(page, &cnt, sizeof(cnt));

                uint8_t* names_dst = page + offsetof(SoaPage, usernames);
                for (size_t i = 0; i < count; ++i)
                {
                    std::memcpy(names_dst + i * USERNAME_LEN, users[offset + i].username, USERNAME_LEN);
                }

                uint8_t* magic_dst = page + offsetof(SoaPage, magic_numbers);
                for (size_t i = 0; i < count; ++i)
                {
                    std::memcpy(magic_dst + i * sizeof(uint64_t), &users[offset + i].magic_number, sizeof(uint64_t));
                }
            }
            else
            {
                std::memcpy(page, &users[offset], count * sizeof(User));
                if (count < USERS_PER_PAGE)
                {
                    std::memset(page + count * sizeof(User), 0, PAGE_SIZE - count * sizeof(User));
                }
            }
        }
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Read phase: NUM_QUERIES separate read transactions
    uint64_t last_avg = 0;
    uint64_t last_count = 0;
    uint64_t checksum = 0;

    auto const t_read_start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < NUM_QUERIES; ++q)
    {
        auto trx = db.trx_r(THREAD_ID);
        DEFER_close(trx);

        uint64_t counter = 0;
        uint64_t record_count = 0;
        uint64_t const pages = trx.pages_count();

        for (uint64_t page_idx = 0; page_idx < pages; ++page_idx)
        {
            uint8_t const* page = static_cast<uint8_t const*>(trx.get_page(page_idx));

            if constexpr (USE_SOA_PAGE_LAYOUT)
            {
                uint64_t count = 0;
                std::memcpy(&count, page, sizeof(count));

                uint8_t const* magic_src = page + offsetof(SoaPage, magic_numbers);
                // Chunked memcpy into L1 buffer guarantees strict-aliasing safety + SIMD auto-vectorization
                constexpr size_t CHUNK = 512;
                alignas(64) uint64_t buf[CHUNK];
                size_t i = 0;
                for (; i + CHUNK <= count; i += CHUNK)
                {
                    std::memcpy(buf, magic_src + i * sizeof(uint64_t), sizeof(buf));
                    for (size_t k = 0; k < CHUNK; ++k)
                    {
                        counter += buf[k];
                    }
                }
                for (; i < count; ++i)
                {
                    uint64_t m;
                    std::memcpy(&m, magic_src + i * sizeof(uint64_t), sizeof(m));
                    counter += m;
                }
                record_count += count;
            }
            else
            {
                char last_first_char = '\0';
                std::memcpy(&last_first_char, page + (USERS_PER_PAGE - 1) * sizeof(User), 1);

                if (last_first_char != '\0') [[likely]]
                {
                    // Branchless unrolled scan for full pages
                    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                    size_t slot = 0;
                    for (; slot + 4 <= USERS_PER_PAGE; slot += 4)
                    {
                        uint64_t m0, m1, m2, m3;
                        std::memcpy(&m0, page + (slot + 0) * sizeof(User) + offsetof(User, magic_number), sizeof(uint64_t));
                        std::memcpy(&m1, page + (slot + 1) * sizeof(User) + offsetof(User, magic_number), sizeof(uint64_t));
                        std::memcpy(&m2, page + (slot + 2) * sizeof(User) + offsetof(User, magic_number), sizeof(uint64_t));
                        std::memcpy(&m3, page + (slot + 3) * sizeof(User) + offsetof(User, magic_number), sizeof(uint64_t));
                        s0 += m0;
                        s1 += m1;
                        s2 += m2;
                        s3 += m3;
                    }
                    counter += s0 + s1 + s2 + s3;
                    for (; slot < USERS_PER_PAGE; ++slot)
                    {
                        uint64_t m;
                        std::memcpy(&m, page + slot * sizeof(User) + offsetof(User, magic_number), sizeof(uint64_t));
                        counter += m;
                    }
                    record_count += USERS_PER_PAGE;
                }
                else
                {
                    for (size_t slot = 0; slot < USERS_PER_PAGE; ++slot)
                    {
                        User u;
                        std::memcpy(&u, page + slot * sizeof(User), sizeof(User));
                        if (u.username[0] == '\0')
                        {
                            break;
                        }
                        counter += u.magic_number;
                        ++record_count;
                    }
                }
            }
        }

        last_count = record_count;
        last_avg = (record_count > 0) ? (counter / record_count) : 0;
        checksum += last_avg;
    }
    auto const t_read_end = std::chrono::steady_clock::now();

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Verification failed!");
    }

    auto const write_us = std::chrono::duration<double, std::micro>(t_write_end - t_write_start).count();
    auto const read_us = std::chrono::duration<double, std::micro>(t_read_end - t_read_start).count();

    std::cout << "[megazine]\n"
              << "  records:        " << last_count << '\n'
              << "  iterations:     " << NUM_QUERIES << '\n'
              << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
              << "  write (" << NUM_USERS << "):    " << write_us / 1000.0 << " ms (" << write_us << " us)\n"
              << "  read (" << NUM_QUERIES << "x):   " << read_us / 1000.0 << " ms (" << read_us / NUM_QUERIES << " us/query)\n"
              << "  total:          " << (write_us + read_us) / 1000.0 << " ms\n";

    return 0;
}