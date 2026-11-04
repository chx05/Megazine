#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

// TODO
// make all api safe to use with any param once
// the db works

#define PAGE_SIZE 16
#define INITIAL_MIN_PAGES_COUNT 10

#define MAP_SLOT_NOT_LOADED (-1)

typedef HANDLE file_descriptor_t;

struct diff_buffer
{
	// contains pages content
	uint64_t used_arena_capacity;
	uint64_t arena_capacity;
	uint8_t* arena;

	// maps page number to arena offset
	uint64_t used_map_capacity;
	uint64_t map_capacity;
	uint64_t* map;
};

struct read_transaction
{
	struct diff_buffer diff_buffer;
	file_descriptor_t fd;
};

struct write_transaction
{
	struct diff_buffer diff_buffer;
	file_descriptor_t fd;
};

struct db
{
	char const* filename;
	file_descriptor_t fd;
	uint64_t file_size;
	// TODO
	// make this atomically swappable (probably needs to be a pointer)
	struct diff_buffer readonly_base;
};

void init_diff_buffer(struct diff_buffer* b, uint64_t file_size)
{
	b->used_arena_capacity = 0;
	b->arena_capacity = PAGE_SIZE * INITIAL_MIN_PAGES_COUNT;
	b->arena = (uint8_t*)malloc(b->arena_capacity);

	b->used_map_capacity = file_size / PAGE_SIZE;
	b->map_capacity = max(b->used_map_capacity, INITIAL_MIN_PAGES_COUNT);
	uint64_t malloc_size_for_map = b->map_capacity * sizeof(uint64_t);
	b->map = (uint64_t*)malloc(malloc_size_for_map);

	if (b->map != NULL)
		memset(b->map, MAP_SLOT_NOT_LOADED, malloc_size_for_map);
}

bool alloc_and_map_page(struct diff_buffer* b, uint64_t page_index)
{
	if (b->used_arena_capacity == b->arena_capacity)
	{
		b->arena_capacity *= 2;
		void* r = realloc(b->arena, b->arena_capacity);
		if (r == NULL)
			return false;
		b->arena = (uint8_t*)r;
	}

	b->map[page_index] = b->used_arena_capacity;
	b->used_arena_capacity += PAGE_SIZE;
	return true;
}

bool add_blank_page(struct diff_buffer* b, uint64_t* out_page_index)
{
	if (b->used_map_capacity == b->map_capacity)
	{
		b->map_capacity *= 2;
		void* r = realloc(b->map, b->map_capacity);
		if (r == NULL)
			return false;
		b->map = (uint64_t*)r;
	}

	*out_page_index = b->used_map_capacity;
	b->used_map_capacity += 1;
	return alloc_and_map_page(b, *out_page_index);
}

bool read_page_from_file(file_descriptor_t fd, uint64_t file_offset, void* dest_buffer)
{
	OVERLAPPED ov = { 0 };
	{
		ov.Offset = (DWORD)(file_offset);
		ov.OffsetHigh = (DWORD)(file_offset >> 32);
	}

	DWORD bytes = 0;
	return ReadFile(fd, dest_buffer, PAGE_SIZE, &bytes, &ov);
}

void write_page_to_file(file_descriptor_t fd, uint64_t file_offset, void const* source_buffer)
{
	OVERLAPPED ov = { 0 };
	{
		ov.Offset = (DWORD)(file_offset);
		ov.OffsetHigh = (DWORD)(file_offset >> 32);
	}

	DWORD bytes = 0;
	WriteFile(fd, source_buffer, PAGE_SIZE, &bytes, &ov);
}

uint8_t* pointer_of(struct diff_buffer* b, uint64_t page_index)
{
	return &b->arena[b->map[page_index]];
}

bool load_page(struct read_transaction* trx, uint64_t page_index)
{
	if (!alloc_and_map_page(&trx->diff_buffer, page_index))
		return false;

	uint64_t file_offset = page_index * PAGE_SIZE;
	return read_page_from_file(trx->fd, file_offset, pointer_of(&trx->diff_buffer, page_index));
}

uint8_t const* get_page(struct read_transaction* trx, uint64_t page_index)
{
	if (trx->diff_buffer.map[page_index] == MAP_SLOT_NOT_LOADED)
		load_page(trx, page_index);

	return pointer_of(&trx->diff_buffer, page_index);
}

void deinit_diff_buffer(struct diff_buffer* b)
{
	if (b->arena != NULL) free(b->arena);
	if (b->map != NULL) free(b->map);

	b->arena = NULL;
	b->map = NULL;

	b->used_arena_capacity = 0;
	b->arena_capacity = 0;

	b->used_map_capacity = 0;
	b->map_capacity = 0;
}

void clone_diff_buffer_from(struct diff_buffer* b, struct diff_buffer* from)
{
	b->used_arena_capacity = from->used_arena_capacity;
	b->arena_capacity = from->arena_capacity;
	b->arena = (uint8_t*)malloc(b->arena_capacity);
	if (b->arena != NULL)
		memcpy(b->arena, from->arena, b->arena_capacity);

	b->used_map_capacity = from->used_map_capacity;
	b->map_capacity = from->map_capacity;
	uint64_t malloc_size_for_map = b->map_capacity * sizeof(uint64_t);
	b->map = (uint64_t*)malloc(malloc_size_for_map);
	if (b->map != NULL)
		memcpy(b->map, from->map, malloc_size_for_map);
}

bool open_db(struct db* db, char const* filename)
{
	db->filename = filename;
	db->fd = CreateFileA(
		db->filename,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ,
		NULL,
		OPEN_ALWAYS, // opens existing or creates new
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);
	if (db->fd == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER file_size_result;
	if (!GetFileSizeEx(db->fd, &file_size_result))
		return false;

	db->file_size = file_size_result.QuadPart;
	init_diff_buffer(&db->readonly_base, db->file_size);
	return true;
}

void apply_diff_buffer_to_file(struct db* db)
{
	// TODO
	// convert this function to use struct write_transaction
	// 
	// TODO
	// file flushing can be done ONLY on a readonly_base that has no previous verions alive
	// 
	// TODO
	// make this crash safe
	// 
	// TODO
	// only write W pages, R pages can be ignored, they are up to date
	for (uint64_t page_index = 0; page_index < db->readonly_base.used_map_capacity; page_index++)
	{
		if (db->readonly_base.map[page_index] == MAP_SLOT_NOT_LOADED)
			continue;

		uint64_t file_offset = page_index * PAGE_SIZE;
		write_page_to_file(db->fd, file_offset, (void*)pointer_of(&db->readonly_base, page_index));
	}
}

void close_db(struct db* db)
{
	if (db->fd != INVALID_HANDLE_VALUE)
	{
		CloseHandle(db->fd);
		db->fd = INVALID_HANDLE_VALUE;
	}

	deinit_diff_buffer(&db->readonly_base);
}

// TODO
// right now it clones the whole diff buffer
// instead, it should clone the map only
// and the map should contain pointers instead
// so that it can store memory of different allocators
struct read_transaction begin_read_transaction(struct db* db)
{
	struct read_transaction trx;
	clone_diff_buffer_from(&trx.diff_buffer, &db->readonly_base);
	trx.fd = db->fd;

	return trx;
}

void end_read_transaction(struct read_transaction* trx)
{
	// TODO
	// implement atomic refcounting
	// or threads array
	deinit_diff_buffer(&trx->diff_buffer);
}