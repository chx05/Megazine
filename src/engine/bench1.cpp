// gemini 4 hydrogen wrote this benchmark
// list full scan

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "megazine_engine.cpp"

struct User
{
    char username[8];
    uint64_t magic_number;
};

static_assert(sizeof(User) == 16);
static_assert(std::is_trivially_copyable_v<User>);
static_assert(PAGE_SIZE >= sizeof(User), "PAGE_SIZE must fit at least one User (24 bytes)");

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 1000000;
constexpr size_t NUM_QUERIES = 100;
constexpr size_t USERS_PER_PAGE = PAGE_SIZE / sizeof(User);
constexpr size_t PAGES_NEEDED = (NUM_USERS + USERS_PER_PAGE - 1) / USERS_PER_PAGE;

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

    std::vector<User> users{};
    users.reserve(NUM_USERS);
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
            /*for (size_t j = 0; j < i; ++j)
            {
                if (std::memcmp(users[i].username, users[j].username, USERNAME_LEN) == 0)
                {
                    unique = false;
                    break;
                }
            }*/
        }
        users[i].magic_number = magic_dist(rng);
    }
    return users;
}

int main_1()
{
    constexpr char const* DB_PATH = "test2";
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

    // 1 thread slot since everything runs sequentially on thread_id = 0
    db = Db::open(DB_PATH, 1);
    DEFER_close(db);

    // 1) Write phase: register 100 users in a single write transaction
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        auto trx = db.trx_w(THREAD_ID);
        DEFER_close(trx);

        trx.modify_file_size(PAGES_NEEDED * PAGE_SIZE);

        uint8_t* current_page = nullptr;
        for (size_t i = 0; i < NUM_USERS; ++i)
        {
            size_t const slot = i % USERS_PER_PAGE;
            if (slot == 0)
            {
                auto page_index = i / USERS_PER_PAGE;
                //auto page_index = trx.push_page();
                current_page = (uint8_t*)trx.get_mut_page(page_index);
                if constexpr (USERS_PER_PAGE > 1)
                {
                    std::memset(current_page, 0, PAGE_SIZE);
                }
            }
            std::memcpy(current_page + slot * sizeof(User), &users[i], sizeof(User));
        }
    }
    auto const t_write_end = std::chrono::steady_clock::now();

    // 2) Read phase: run 1000 separate read transactions, scanning all records each time
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
            uint8_t const* page = (uint8_t const*)trx.get_page(page_idx);
            for (size_t slot = 0; slot < USERS_PER_PAGE; ++slot)
            {
                User u;
                std::memcpy(&u, page + slot * sizeof(User), sizeof(User));
                if constexpr (USERS_PER_PAGE > 1)
                {
                    if (u.username[0] == '\0')
                    {
                        break;
                    }
                }
                counter += u.magic_number;
                ++record_count;
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
        << "  write (100):    " << write_us / 1000.0 << " ms (" << write_us << " us)\n"
        << "  read (1000x):   " << read_us / 1000.0 << " ms (" << read_us / NUM_QUERIES << " us/query)\n"
        << "  total:          " << (write_us + read_us) / 1000.0 << " ms\n";

    return 0;
}