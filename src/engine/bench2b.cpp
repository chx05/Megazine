// gemini 4 hydrogen wrote this benchmark
// btree username lookup

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

#include "megazine_engine.cpp"

constexpr size_t USERNAME_LEN = 8;
constexpr size_t NUM_USERS = 1000000;
constexpr size_t NUM_QUERIES = 100;

struct User
{
    char username[USERNAME_LEN];
    uint64_t magic_number;
};

struct MegazineBTree
{
    struct MetaPage
    {
        uint64_t root_page;
        uint64_t first_leaf;
        uint64_t count;
        uint32_t height;
    };

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

    static_assert(PAGE_SIZE >= 64 && (PAGE_SIZE % sizeof(uint64_t)) == 0,
        "PAGE_SIZE must be >= 64 and a multiple of 8");
    static_assert(sizeof(MetaPage) <= PAGE_SIZE);
    static_assert(LEAF_CAP >= 4 && INTERNAL_CAP >= 4);
    static_assert(LEAF_VALS_OFF + LEAF_CAP * sizeof(uint64_t) <= PAGE_SIZE);
    static_assert(INTERNAL_CHILDREN_OFF + (INTERNAL_CAP + 1) * sizeof(uint64_t) <= PAGE_SIZE);

    static uint64_t pack_key(char const* s)
    {
        uint64_t k = 0;
        std::memcpy(&k, s, USERNAME_LEN);
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap64(k);
#else
        return ((k & 0x00000000000000FFULL) << 56) |
            ((k & 0x000000000000FF00ULL) << 40) |
            ((k & 0x0000000000FF0000ULL) << 24) |
            ((k & 0x00000000FF000000ULL) << 8) |
            ((k & 0x000000FF00000000ULL) >> 8) |
            ((k & 0x0000FF0000000000ULL) >> 24) |
            ((k & 0x00FF000000000000ULL) >> 40) |
            ((k & 0xFF00000000000000ULL) >> 56);
#endif
    }

    static uint32_t get_u32(uint8_t const* p, size_t byte_off)
    {
        uint32_t v = 0;
        std::memcpy(&v, p + byte_off, sizeof(v));
        return v;
    }

    static void put_u32(uint8_t* p, size_t byte_off, uint32_t v)
    {
        std::memcpy(p + byte_off, &v, sizeof(v));
    }

    static uint64_t get_u64(uint8_t const* p, size_t byte_off)
    {
        uint64_t v = 0;
        std::memcpy(&v, p + byte_off, sizeof(v));
        return v;
    }

    static void put_u64(uint8_t* p, size_t byte_off, uint64_t v)
    {
        std::memcpy(p + byte_off, &v, sizeof(v));
    }

    static uint64_t get_u64_at(uint8_t const* base, size_t idx)
    {
        return get_u64(base, idx * sizeof(uint64_t));
    }

    static void put_u64_at(uint8_t* base, size_t idx, uint64_t v)
    {
        put_u64(base, idx * sizeof(uint64_t), v);
    }

    static MetaPage read_meta(auto& trx)
    {
        if (trx.pages_count() == 0)
        {
            return {};
        }
        MetaPage m{};
        std::memcpy(&m, trx.get_page(0), sizeof(MetaPage));
        return m;
    }

    static void write_meta(auto& trx, MetaPage const& m)
    {
        std::memcpy(trx.get_mut_page(0), &m, sizeof(MetaPage));
    }

    static uint64_t alloc_page(auto& trx)
    {
        uint64_t const p = static_cast<uint64_t>(trx.push_page());
        std::memset(trx.get_mut_page(p), 0, PAGE_SIZE);
        return p;
    }

    static uint32_t lower_bound_keys(uint8_t const* keys_ptr, uint32_t count, uint64_t key)
    {
        uint32_t lo = 0;
        uint32_t hi = count;
        while (lo < hi)
        {
            uint32_t const mid = lo + ((hi - lo) >> 1);
            if (get_u64_at(keys_ptr, mid) < key)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }

    static uint32_t upper_bound_keys(uint8_t const* keys_ptr, uint32_t count, uint64_t key)
    {
        uint32_t lo = 0;
        uint32_t hi = count;
        while (lo < hi)
        {
            uint32_t const mid = lo + ((hi - lo) >> 1);
            if (key < get_u64_at(keys_ptr, mid))
            {
                hi = mid;
            }
            else
            {
                lo = mid + 1;
            }
        }
        return lo;
    }

    static void init_empty(auto& trx)
    {
        if (trx.pages_count() == 0)
        {
            (void)trx.push_page();
        }
        std::memset(trx.get_mut_page(0), 0, PAGE_SIZE);
    }

    static void insert(auto& trx, uint64_t key, uint64_t val)
    {
        if (trx.pages_count() == 0)
        {
            init_empty(trx);
        }

        MetaPage meta = read_meta(trx);
        if (meta.root_page == 0)
        {
            uint64_t const leaf = alloc_page(trx);
            uint8_t* mleaf = trx.get_mut_page(leaf);
            put_u32(mleaf, NODE_IS_LEAF_OFF, 1);
            put_u32(mleaf, NODE_COUNT_OFF, 1);
            put_u64(mleaf, LEAF_NEXT_OFF, 0);
            put_u64_at(mleaf + LEAF_KEYS_OFF, 0, key);
            put_u64_at(mleaf + LEAF_VALS_OFF, 0, val);

            meta.root_page = leaf;
            meta.first_leaf = leaf;
            meta.count = 1;
            meta.height = 1;
            write_meta(trx, meta);
            return;
        }

        uint64_t path_pages[MAX_HEIGHT];
        uint32_t path_slots[MAX_HEIGHT];
        uint32_t depth = 0;
        uint64_t page_idx = meta.root_page;

        while (true)
        {
            uint8_t const* page = trx.get_page(page_idx);
            uint32_t const is_leaf = get_u32(page, NODE_IS_LEAF_OFF);
            uint32_t const count = get_u32(page, NODE_COUNT_OFF);
            if (is_leaf != 0)
            {
                break;
            }
            if (depth >= MAX_HEIGHT)
            {
                throw std::runtime_error("BTree height exceeded");
            }
            uint32_t const slot = upper_bound_keys(page + INTERNAL_KEYS_OFF, count, key);
            path_pages[depth] = page_idx;
            path_slots[depth] = slot;
            ++depth;
            page_idx = get_u64_at(page + INTERNAL_CHILDREN_OFF, slot);
        }

        uint8_t const* leaf_ro = trx.get_page(page_idx);
        uint32_t const count = get_u32(leaf_ro, NODE_COUNT_OFF);
        uint32_t const pos = lower_bound_keys(leaf_ro + LEAF_KEYS_OFF, count, key);

        if (pos < count && get_u64_at(leaf_ro + LEAF_KEYS_OFF, pos) == key)
        {
            uint8_t* mleaf = trx.get_mut_page(page_idx);
            put_u64_at(mleaf + LEAF_VALS_OFF, pos, val);
            return;
        }

        if (count < LEAF_CAP)
        {
            uint8_t* mleaf = trx.get_mut_page(page_idx);
            uint8_t* lk = mleaf + LEAF_KEYS_OFF;
            uint8_t* lv = mleaf + LEAF_VALS_OFF;
            if (pos < count)
            {
                std::memmove(lk + (pos + 1) * sizeof(uint64_t),
                    lk + pos * sizeof(uint64_t),
                    (count - pos) * sizeof(uint64_t));
                std::memmove(lv + (pos + 1) * sizeof(uint64_t),
                    lv + pos * sizeof(uint64_t),
                    (count - pos) * sizeof(uint64_t));
            }
            put_u64_at(lk, pos, key);
            put_u64_at(lv, pos, val);
            put_u32(mleaf, NODE_COUNT_OFF, count + 1);

            ++meta.count;
            write_meta(trx, meta);
            return;
        }

        // Split full leaf
        uint64_t const right_leaf = alloc_page(trx);
        uint8_t* mleft = trx.get_mut_page(page_idx);
        uint8_t* mright = trx.get_mut_page(right_leaf);
        uint8_t* lk = mleft + LEAF_KEYS_OFF;
        uint8_t* lv = mleft + LEAF_VALS_OFF;
        uint8_t* rk = mright + LEAF_KEYS_OFF;
        uint8_t* rv = mright + LEAF_VALS_OFF;

        constexpr uint32_t left_count = static_cast<uint32_t>((LEAF_CAP + 1) / 2);
        constexpr uint32_t right_count = static_cast<uint32_t>(LEAF_CAP + 1 - left_count);

        if (pos < left_count)
        {
            std::memcpy(rk, lk + (left_count - 1) * sizeof(uint64_t), right_count * sizeof(uint64_t));
            std::memcpy(rv, lv + (left_count - 1) * sizeof(uint64_t), right_count * sizeof(uint64_t));
            if (pos < left_count - 1)
            {
                std::memmove(lk + (pos + 1) * sizeof(uint64_t),
                    lk + pos * sizeof(uint64_t),
                    (left_count - 1 - pos) * sizeof(uint64_t));
                std::memmove(lv + (pos + 1) * sizeof(uint64_t),
                    lv + pos * sizeof(uint64_t),
                    (left_count - 1 - pos) * sizeof(uint64_t));
            }
            put_u64_at(lk, pos, key);
            put_u64_at(lv, pos, val);
        }
        else
        {
            uint32_t const rpos = pos - left_count;
            if (rpos > 0)
            {
                std::memcpy(rk, lk + left_count * sizeof(uint64_t), rpos * sizeof(uint64_t));
                std::memcpy(rv, lv + left_count * sizeof(uint64_t), rpos * sizeof(uint64_t));
            }
            put_u64_at(rk, rpos, key);
            put_u64_at(rv, rpos, val);
            if (pos < LEAF_CAP)
            {
                std::memcpy(rk + (rpos + 1) * sizeof(uint64_t),
                    lk + pos * sizeof(uint64_t),
                    (LEAF_CAP - pos) * sizeof(uint64_t));
                std::memcpy(rv + (rpos + 1) * sizeof(uint64_t),
                    lv + pos * sizeof(uint64_t),
                    (LEAF_CAP - pos) * sizeof(uint64_t));
            }
        }

        uint64_t const old_next = get_u64(mleft, LEAF_NEXT_OFF);
        put_u32(mright, NODE_IS_LEAF_OFF, 1);
        put_u32(mright, NODE_COUNT_OFF, right_count);
        put_u64(mright, LEAF_NEXT_OFF, old_next);
        put_u32(mleft, NODE_COUNT_OFF, left_count);
        put_u64(mleft, LEAF_NEXT_OFF, right_leaf);

        uint64_t promote_key = get_u64_at(rk, 0);
        uint64_t promote_child = right_leaf;

        while (depth > 0)
        {
            --depth;
            uint64_t const parent_page = path_pages[depth];
            uint32_t const slot = path_slots[depth];
            uint8_t* mparent = trx.get_mut_page(parent_page);
            uint32_t const pcount = get_u32(mparent, NODE_COUNT_OFF);

            if (pcount < INTERNAL_CAP)
            {
                uint8_t* pk = mparent + INTERNAL_KEYS_OFF;
                uint8_t* pc = mparent + INTERNAL_CHILDREN_OFF;
                if (slot < pcount)
                {
                    std::memmove(pk + (slot + 1) * sizeof(uint64_t),
                        pk + slot * sizeof(uint64_t),
                        (pcount - slot) * sizeof(uint64_t));
                    std::memmove(pc + (slot + 2) * sizeof(uint64_t),
                        pc + (slot + 1) * sizeof(uint64_t),
                        (pcount - slot) * sizeof(uint64_t));
                }
                put_u64_at(pk, slot, promote_key);
                put_u64_at(pc, slot + 1, promote_child);
                put_u32(mparent, NODE_COUNT_OFF, pcount + 1);

                ++meta.count;
                write_meta(trx, meta);
                return;
            }

            // Split full internal node
            uint64_t const right_internal = alloc_page(trx);
            mparent = trx.get_mut_page(parent_page);
            uint8_t* mright_int = trx.get_mut_page(right_internal);
            uint8_t* pk = mparent + INTERNAL_KEYS_OFF;
            uint8_t* pc = mparent + INTERNAL_CHILDREN_OFF;

            uint64_t tmp_k[INTERNAL_CAP + 1];
            uint64_t tmp_c[INTERNAL_CAP + 2];

            if (slot > 0)
            {
                std::memcpy(tmp_k, pk, slot * sizeof(uint64_t));
            }
            tmp_k[slot] = promote_key;
            if (slot < INTERNAL_CAP)
            {
                std::memcpy(tmp_k + slot + 1, pk + slot * sizeof(uint64_t), (INTERNAL_CAP - slot) * sizeof(uint64_t));
            }

            std::memcpy(tmp_c, pc, (slot + 1) * sizeof(uint64_t));
            tmp_c[slot + 1] = promote_child;
            if (slot < INTERNAL_CAP)
            {
                std::memcpy(tmp_c + slot + 2, pc + (slot + 1) * sizeof(uint64_t), (INTERNAL_CAP - slot) * sizeof(uint64_t));
            }

            constexpr uint32_t mid = static_cast<uint32_t>((INTERNAL_CAP + 1) / 2);
            constexpr uint32_t int_left_count = mid;
            constexpr uint32_t int_right_count = static_cast<uint32_t>(INTERNAL_CAP - mid);

            std::memcpy(pk, tmp_k, int_left_count * sizeof(uint64_t));
            std::memcpy(pc, tmp_c, (int_left_count + 1) * sizeof(uint64_t));
            put_u32(mparent, NODE_COUNT_OFF, int_left_count);

            uint8_t* rk_int = mright_int + INTERNAL_KEYS_OFF;
            uint8_t* rc_int = mright_int + INTERNAL_CHILDREN_OFF;
            std::memcpy(rk_int, tmp_k + mid + 1, int_right_count * sizeof(uint64_t));
            std::memcpy(rc_int, tmp_c + mid + 1, (int_right_count + 1) * sizeof(uint64_t));
            put_u32(mright_int, NODE_IS_LEAF_OFF, 0);
            put_u32(mright_int, NODE_COUNT_OFF, int_right_count);

            promote_key = tmp_k[mid];
            promote_child = right_internal;
        }

        uint64_t const new_root = alloc_page(trx);
        uint8_t* mroot = trx.get_mut_page(new_root);
        put_u32(mroot, NODE_IS_LEAF_OFF, 0);
        put_u32(mroot, NODE_COUNT_OFF, 1);
        put_u64_at(mroot + INTERNAL_KEYS_OFF, 0, promote_key);
        put_u64_at(mroot + INTERNAL_CHILDREN_OFF, 0, meta.root_page);
        put_u64_at(mroot + INTERNAL_CHILDREN_OFF, 1, promote_child);

        meta.root_page = new_root;
        ++meta.height;
        ++meta.count;
        write_meta(trx, meta);
    }

    static std::optional<uint64_t> find(auto& trx, uint64_t key)
    {
        if (trx.pages_count() == 0)
        {
            return std::nullopt;
        }
        MetaPage const meta = read_meta(trx);
        if (meta.root_page == 0)
        {
            return std::nullopt;
        }

        uint64_t page_idx = meta.root_page;
        while (true)
        {
            uint8_t const* page = trx.get_page(page_idx);
            uint32_t const is_leaf = get_u32(page, NODE_IS_LEAF_OFF);
            uint32_t const count = get_u32(page, NODE_COUNT_OFF);

            if (is_leaf != 0)
            {
                uint32_t const pos = lower_bound_keys(page + LEAF_KEYS_OFF, count, key);
                if (pos < count && get_u64_at(page + LEAF_KEYS_OFF, pos) == key)
                {
                    return get_u64_at(page + LEAF_VALS_OFF, pos);
                }
                return std::nullopt;
            }

            uint32_t const slot = upper_bound_keys(page + INTERNAL_KEYS_OFF, count, key);
            page_idx = get_u64_at(page + INTERNAL_CHILDREN_OFF, slot);
        }
    }

    static std::pair<uint64_t, uint64_t> sum_and_avg(auto& trx)
    {
        MetaPage const meta = read_meta(trx);
        if (meta.count == 0 || meta.first_leaf == 0)
        {
            return { 0, 0 };
        }

        uint64_t counter = 0;
        uint64_t total_records = 0;
        uint64_t page_idx = meta.first_leaf;

        while (page_idx != 0)
        {
            uint8_t const* page = trx.get_page(page_idx);
            uint32_t const count = get_u32(page, NODE_COUNT_OFF);
            uint64_t const next_leaf = get_u64(page, LEAF_NEXT_OFF);
            uint8_t const* vals = page + LEAF_VALS_OFF;

            for (uint32_t i = 0; i < count; ++i)
            {
                counter += get_u64_at(vals, i);
            }
            total_records += count;
            page_idx = next_leaf;
        }
        return { total_records, total_records > 0 ? (counter / total_records) : 0 };
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

int main_2b()
{
    constexpr char const* DB_PATH = "test2_btree";
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

    db = Db::open(DB_PATH, 1);
    DEFER_close(db);

    // 1) Write phase: populate B+Tree
    auto const t_write_start = std::chrono::steady_clock::now();
    {
        auto trx = db.trx_w(THREAD_ID);
        DEFER_close(trx);

        MegazineBTree::init_empty(trx);
        for (auto const& u : users)
        {
            MegazineBTree::insert(trx, MegazineBTree::pack_key(u.username), u.magic_number);
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

        auto const [cnt, avg] = MegazineBTree::sum_and_avg(trx);
        last_count = cnt;
        last_avg = avg;
        checksum += avg;
    }
    auto const t_scan_end = std::chrono::steady_clock::now();

    if (last_count != NUM_USERS || last_avg != expected_avg)
    {
        throw std::runtime_error("Scan verification failed!");
    }

    // 3) Point-lookup phase (1M O(log N) lookups)
    uint64_t lookup_checksum = 0;
    auto const t_lookup_start = std::chrono::steady_clock::now();
    for (auto const& u : users)
    {
        auto trx = db.trx_r(THREAD_ID);
        DEFER_close(trx);

        auto const res = MegazineBTree::find(trx, MegazineBTree::pack_key(u.username));
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

    std::cout << "[megazine btree]\n"
        << "  records:        " << last_count << '\n'
        << "  avg magic:      " << last_avg << " (checksum=" << checksum << ")\n"
        << "  write (" << NUM_USERS << "):  " << write_us / 1000.0 << " ms\n"
        << "  scan (" << NUM_QUERIES << "x):    " << scan_us / 1000.0 << " ms (" << scan_us / NUM_QUERIES << " us/query)\n"
        << "  lookup (" << NUM_USERS << "x): " << lookup_us / 1000.0 << " ms (" << (lookup_us * 1000.0) / NUM_USERS << " ns/lookup, check=" << lookup_checksum << ")\n";

    return 0;
}