#include <iostream>
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

int main()
{
    auto db = Db::open("test");
    DEFER_close(db);

    {
        auto trx = db.trx_r();
        DEFER_close(trx);

        for (uint64_t i = 0; i < trx.pages_count(); i++)
        {
            uint8_t const* page_content = trx.get_page(i);
            std::cout << i << ":\t" << std::string_view((char const*)page_content, PAGE_SIZE) << '\n';

            uint64_t const* page_content_128bit = (uint64_t const*)page_content;
            std::cout << '\t' << page_content_128bit[0] << '|' << page_content_128bit[1] << "\n\n";
        }
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
