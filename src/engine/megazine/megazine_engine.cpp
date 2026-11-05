#include "megazine_engine.hpp"




//
//bool add_blank_page(struct PagedBuffer* b, uint64_t* out_page_index)
//{
//	if (b->used_map_capacity == b->map_capacity)
//	{
//		b->map_capacity *= 2;
//		void* r = realloc(b->map, b->map_capacity);
//		if (r == NULL)
//			return false;
//		b->map = (uint64_t*)r;
//	}
//
//	*out_page_index = b->used_map_capacity;
//	b->used_map_capacity += 1;
//	return alloc_and_map_page(b, *out_page_index);
//}

//void clone_diff_buffer_from(struct PagedBuffer* b, struct PagedBuffer* from)
//{
//	b->used_arena_capacity = from->used_arena_capacity;
//	b->arena_capacity = from->arena_capacity;
//	b->arena = (uint8_t*)malloc(b->arena_capacity);
//	if (b->arena != NULL)
//		memcpy(b->arena, from->arena, b->arena_capacity);
//
//	b->used_map_capacity = from->used_map_capacity;
//	b->map_capacity = from->map_capacity;
//	uint64_t malloc_size_for_map = b->map_capacity * sizeof(uint64_t);
//	b->map = (uint64_t*)malloc(malloc_size_for_map);
//	if (b->map != NULL)
//		memcpy(b->map, from->map, malloc_size_for_map);
//}

//void init_roots_pool(RootsPool * p)
//{
//	p->capacity = INITIAL_ROOTS_POOL_CAPACITY;
//
//	p->arena_len = 0;
//	p->arena = (SharedReadonlyPagedBuffer*)malloc(p->capacity * sizeof(SharedReadonlyPagedBuffer));
//
//	p->slots_len = 0;
//	p->free_slots = (uint32_t*)malloc(p->capacity * sizeof(uint32_t));
//}
//
//void apply_diff_buffer_to_file(Db* db)
//{
//	// TODO
//	// convert this function to use WTrx
//	// 
//	// TODO
//	// file flushing can be done ONLY on a readonly_base that has no previous verions alive
//	// 
//	// TODO
//	// make this crash safe
//	// 
//	// TODO
//	// only write W pages, R pages can be ignored, they are up to date
//	for (uint64_t page_index = 0; page_index < pages_count(&db->root->paged_buffer); page_index++)
//	{
//		if (db->root->paged_buffer.map[page_index] == MAP_SLOT_NOT_LOADED)
//			continue;
//
//		uint64_t file_offset = page_index * PAGE_SIZE;
//		write_page_to_file(db->fd, file_offset, (void*)pointer_of(&db->root->paged_buffer, page_index));
//	}
//}

// TODO
// right now it clones the whole diff buffer
// instead, it should clone the map only
// and the map should contain pointers instead
// so that it can store memory of different allocators
//TrxR begin_read_transaction(Db* db)
//{
//	TrxR trx;
//	{
//		trx.fd = db->fd;
//		trx.root = db->root;
//	}
//
//	return trx;
//}
//
//void end_read_transaction(TrxR * trx)
//{
//	// TODO
//	// implement atomic refcounting
//	// or threads array
//	deinit_diff_buffer(&trx->root->paged_buffer);
//}