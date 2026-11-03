#include <stdio.h>
#include "megazine_engine.h"

void fatal(char const* msg)
{
    printf("Fatal: %s", msg);
    abort();
}

struct db_handle
{
    uint64_t offset;
    uint64_t len;
};

int main()
{
    struct db db;
    if (!open_db(&db, "test"))
        fatal("cannot open/create db");

    struct read_transaction trx = begin_read_transaction(&db);
    {
        for (uint64_t i = 0; i < trx.diff_buffer.used_map_capacity; i++)
        {
            uint8_t const* page_content = get_page(&trx, i);
            printf("%llu:\t%.*s\n", i, PAGE_SIZE, page_content);

            uint64_t const* page_content_128bit = (uint64_t const*)page_content;
            printf("\t%llu|%llu\n\n", page_content_128bit[0], page_content_128bit[1]);
        }
    }
    end_read_transaction(&trx);

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
