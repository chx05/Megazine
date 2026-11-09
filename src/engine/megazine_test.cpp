#include <iostream>
#include <syncstream>
#include <thread>
#include <random>
#include "megazine_engine.hpp"

// this is what a fat pointer should look like
// when stored in the db
// we can't store real pointers because they invalidate
// after the program reopens.
// so we store relative offsets.
// they are then converted to page index and internal-to-the-page offset
struct db_handle
{
    uint64_t offset;
    uint64_t len;
};

static Db db;

void concurrent_fn(uint32_t thread_id)
{
    auto trx = db.trx_r();
    DEFER_close(trx);

    for (uint64_t i = 0; i < trx.pages_count(); i++)
    {
        uint8_t const* page_content = trx.get_page(i);
        uint64_t const* page_content_128bit = (uint64_t const*)page_content;

        std::osyncstream(std::cout)
            << "[thread " << thread_id << "] "
            << i << ":\t"
            << std::string_view((char const*)page_content, PAGE_SIZE) << '\n'
            << '\t' << page_content_128bit[0] << '|' << page_content_128bit[1] << "\n\n";
    }
}

void concurrent_infinite_fn(uint32_t thread_id)
{
    auto trx = db.trx_r();
    DEFER_close(trx);

    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(0, trx.pages_count() - 1);

    while (true)
    {
        uint64_t i = dist(gen);
        uint8_t const* page_content = trx.get_page(i);
        uint64_t const* page_content_128bit = (uint64_t const*)page_content;

        std::osyncstream(std::cout)
            << "[thread " << thread_id << "] "
            << i << ":\t"
            << std::string_view((char const*)page_content, PAGE_SIZE) << '\n'
            << '\t' << page_content_128bit[0] << '|' << page_content_128bit[1] << "\n\n";
    }
}

void concurrent_infinite_active_fn(uint32_t thread_id)
{
    while (true)
    {
        auto trx = db.trx_r();
        DEFER_close(trx);

        auto first_page = (uint64_t const*)trx.get_page(0);

        std::osyncstream(std::cout)
            << "[thread " << thread_id << "] "
            << first_page[0] << " " << first_page[1]
            << "\n";
    }
}

void concurrent_infinite_passive_fn(uint32_t thread_id)
{
    auto trx = db.trx_r();
    DEFER_close(trx);

    while (true)
    {
        auto first_page = (uint64_t const*)trx.get_page(0);

        std::osyncstream(std::cout)
            << "[thread " << thread_id << "] "
            << first_page[0] << " " << first_page[1]
            << "\n";
    }
}

void infinite_writer_fn(uint32_t thread_id)
{
    (void)thread_id;

    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(0, 100);

    while (true)
    {
        auto trx = db.trx_w();
        DEFER_close(trx);

        auto first_page_mut = (uint64_t*)trx.get_mut_page(0);
        first_page_mut[0] = dist(gen);

        //trx.abort();
    }
}

int main()
{
    db = Db::open("test");
    DEFER_close(db);

    {
        /*std::thread t1(concurrent_fn, 1);
        std::thread t2(concurrent_fn, 2);

        t1.join();
        t2.join();*/
    }

    {
        /*std::thread t1(concurrent_infinite_fn, 1);
        std::thread t2(concurrent_infinite_fn, 2);

        t1.join();
        t2.join();*/
    }

    {
        //std::thread t1(concurrent_infinite_passive_fn, 1);
        //std::thread t2(concurrent_infinite_passive_fn, 2);

        //std::this_thread::sleep_for(std::chrono::seconds(3));
        //auto trx = db.trx_w();
        //DEFER_close(trx);

        //auto first_page_mut = (uint64_t*)trx.get_mut_page(0);
        //first_page_mut[0] = 30;

        //auto first_page = (uint64_t const*)trx.get_page(0);
        //std::cout << first_page[0] << " " << first_page[1] << "\n";

        ////trx.abort();
        //trx.close();

        //t1.join();
        //t2.join();
    }

    {
        std::thread t1(concurrent_infinite_active_fn, 1);
        std::thread t2(concurrent_infinite_active_fn, 2);
        std::thread t3(infinite_writer_fn, 3);

        t1.join();
        t2.join();
        t3.join();
    }

    {
        /*std::thread t1(concurrent_infinite_passive_fn, 1);
        std::thread t2(concurrent_infinite_passive_fn, 2);
        std::thread t3(infinite_writer_fn, 3);

        t1.join();
        t2.join();
        t3.join();*/
    }

    {
        /*auto trx = db.trx_r();
        DEFER_close(trx);

        for (uint64_t i = 0; i < trx.pages_count(); i++)
        {
            uint8_t const* page_content = trx.get_page(i);
            uint64_t const* page_content_128bit = (uint64_t const*)page_content;
            
            std::osyncstream(std::cout)
                << i << ":\t"
                << std::string_view((char const*)page_content, PAGE_SIZE) << '\n'
                << '\t' << page_content_128bit[0] << '|' << page_content_128bit[1] << "\n\n";
        }*/
    }

    {
        //auto trx = db.trx_w();
        //DEFER_close(trx);

        //auto first_page_mut = (uint64_t*)trx.get_mut_page(0);
        //first_page_mut[0] = 30;

        //auto first_page = (uint64_t const*)trx.get_page(0);
        //std::cout << first_page[0] << " " << first_page[1] << "\n";

        ////trx.abort();
    }

    {
        /*auto trx = db.trx_r();
        DEFER_close(trx);

        auto first_page = (uint64_t const*)trx.get_page(0);
        std::cout << first_page[0] << " " << first_page[1] << "\n";*/
    }

    // unsafe page add
    {
        /*uint64_t page_index;
        if (!add_blank_page(&db.readonly_base, &page_index))
            fatal("cannot insert page");

        char const* new_content = "Yooooooooo";
        uint8_t* page_content = pointer_of(&db.readonly_base, page_index);
        memcpy(page_content, (void const*)new_content, min(16, strlen(new_content)));
        apply_diff_buffer_to_file(&db);*/
    }

    // unsafe page manipulation
    {
        /*alloc_and_map_page(&db.readonly_base, 0);
        struct db_handle* page_content = (struct db_handle*)pointer_of(&db.readonly_base, 0);
        page_content->offset = 20;
        page_content->len = PAGE_SIZE;
        apply_diff_buffer_to_file(&db);*/
    }

    /*trx = begin_read_transaction(&db);
    {
        for (uint64_t i = 0; i < trx.diff_buffer.used_map_capacity; i++)
        {
            printf("%llu: %.*s\n", i, PAGE_SIZE, get_page(&trx, i));
        }
    }
    end_read_transaction(trx);*/

    return 0;
}
