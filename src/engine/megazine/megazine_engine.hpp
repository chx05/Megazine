#define NOMINMAX
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <mutex>

// TODO
// try using mmap() for lazy loading instead, the mutex and all the stuff
// made this implementation weird.
// for writers, the diff mechanism can be built ontop of it
// for readers it makes no difference
// BUT first it must be clear what's the workflow for committing correctly


template<typename T>
struct __DeferredDropper
{
	T& value;

	__DeferredDropper(T& value) : value(value)
	{

	}

	~__DeferredDropper()
	{
		value.drop();
	}
};

template<typename T>
struct __DeferredCloser
{
	T& value;

	__DeferredCloser(T& value) : value(value)
	{

	}

	~__DeferredCloser()
	{
		value.close();
	}
};


#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

#define DEFER_close(target) auto CONCAT(_defer_close_, __COUNTER__) = __DeferredCloser(target)
#define DEFER_DROP(target)  auto CONCAT(_defer_drop_, __COUNTER__)  = __DeferredDropper(target)


using page_flag_t = std::atomic<uint8_t>;
using refcounter_t = std::atomic<uint64_t>;
using file_descriptor_t = HANDLE;
using arena_offset_t = uint64_t;

constexpr uint64_t PAGE_SIZE = 16;
constexpr uint64_t INITIAL_MIN_PAGES_COUNT = 10;
constexpr uint64_t INITIAL_ROOTS_POOL_CAPACITY = 5;

constexpr uint8_t PAGE_FLAG_NOT_LOADED = 0;
constexpr uint8_t PAGE_FLAG_LOADING = 1;
constexpr uint8_t PAGE_FLAG_LOADED_FOR_READ = 2;
constexpr uint8_t PAGE_FLAG_LOADED_FOR_WRITE = 3;

inline bool read_page_from_file(file_descriptor_t fd, uint64_t file_offset, void* dest_buffer)
{
	OVERLAPPED ov = { 0 };
	ov.Offset = (DWORD)(file_offset);
	ov.OffsetHigh = (DWORD)(file_offset >> 32);

	DWORD bytes = 0;
	return ReadFile(fd, dest_buffer, (DWORD)PAGE_SIZE, &bytes, &ov);
}

inline bool write_page_to_file(file_descriptor_t fd, uint64_t file_offset, void const* source_buffer)
{
	OVERLAPPED ov = { 0 };
	ov.Offset = (DWORD)(file_offset);
	ov.OffsetHigh = (DWORD)(file_offset >> 32);

	DWORD bytes = 0;
	return WriteFile(fd, source_buffer, (DWORD)PAGE_SIZE, &bytes, &ov);
}

// TODO
// be cautious when resizing this
// the realloc must be followed by a memset of the new blank cells to page_flag_not_loaded
struct PagesMap
{
	uint64_t count = 0;
	uint64_t capacity = 0;
	arena_offset_t* array = nullptr;
	page_flag_t* flags = nullptr;

	static PagesMap create(uint64_t file_size)
	{
		PagesMap map;
		map.count = file_size / PAGE_SIZE;
		map.capacity = std::max(map.count, INITIAL_MIN_PAGES_COUNT);
		map.array = new arena_offset_t[map.capacity];
		map.flags = new page_flag_t[map.capacity];

		// TODO memset should be safe here
		for (uint64_t i = 0; i < map.capacity; ++i)
			map.flags[i].store(PAGE_FLAG_NOT_LOADED, std::memory_order_relaxed);

		return map;
	}

	void drop()
	{
		delete[] array;
		delete[] flags;
		array = nullptr;
		flags = nullptr;
		count = 0;
		capacity = 0;
	}
};

template<typename T>
struct Arena
{
	uint64_t count = 0;
	uint64_t capacity = 0;
	T* buffer = nullptr;

	static Arena create(uint64_t initial_capacity)
	{
		Arena arena;
		arena.count = 0;
		arena.capacity = initial_capacity;
		arena.buffer = new T[initial_capacity];
		return arena;
	}

	void drop()
	{
		delete[] buffer;
		buffer = nullptr;
		count = 0;
		capacity = 0;
	}

	void ensure_has_space_for(uint64_t incoming_count)
	{
		if (count + incoming_count >= capacity)
		{
			capacity = std::max(capacity * 2, capacity + incoming_count);
			auto new_buffer = new T[capacity];
			memcpy(new_buffer, buffer, count); // this is bypassing RAII, which is good in this code base
			auto old_buffer = buffer;
			buffer = new_buffer; // this should be made atomic imo, see TrxR::get_page

			delete[] old_buffer;
		}
	}

	T* alloc()
	{
		ensure_has_space_for(1);
		count++;
		return &buffer[count - 1];
	}
};

struct PagedBuffer
{
	Arena<uint8_t> arena;
	PagesMap map;
	// protects arena allocation
	// this takes negligible time compared to disk i/o that happens in the meanwhile
	std::mutex arena_mutex;

	void init(uint64_t file_size)
	{
		arena = Arena<uint8_t>::create(PAGE_SIZE * INITIAL_MIN_PAGES_COUNT);
		map = PagesMap::create(file_size);
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	void alloc_and_map_page(uint64_t page_index)
	{
		std::lock_guard lock(arena_mutex);

		arena.ensure_has_space_for(PAGE_SIZE);
		map.array[page_index] = arena.count;
		arena.count += PAGE_SIZE;
	}


	uint8_t* pointer_of(uint64_t page_index)
	{
		return &arena.buffer[map.array[page_index]];
	}
};

struct SharedReadonlyPagedBuffer
{
	PagedBuffer paged_buffer;
	refcounter_t refc{ 0 };

	void init(uint64_t file_size)
	{
		paged_buffer.init(file_size);
	}

	void drop()
	{
		paged_buffer.drop();
	}
};

struct TrxR
{
	SharedReadonlyPagedBuffer* root = nullptr;
	file_descriptor_t fd = INVALID_HANDLE_VALUE;

	void load_page(uint64_t page_index)
	{
		root->paged_buffer.alloc_and_map_page(page_index);
		uint64_t file_offset = page_index * PAGE_SIZE;
		read_page_from_file(fd, file_offset, root->paged_buffer.pointer_of(page_index));
	}

	// TODO
	// this should return a relative offset to the arena
	// otherwise the pointer can be invalidated by another thread even before this function returns.
	// but what happens when the shared arena is being reallocated?
	uint8_t const* get_page(uint64_t page_index)
	{
		auto& flag = root->paged_buffer.map.flags[page_index];

		// already loaded?
		uint8_t current = flag.load(std::memory_order_acquire);
		if (current >= PAGE_FLAG_LOADED_FOR_READ)
			return root->paged_buffer.pointer_of(page_index);

		// claiming the job
		uint8_t expected = PAGE_FLAG_NOT_LOADED;
		if (flag.compare_exchange_strong(expected, PAGE_FLAG_LOADING, std::memory_order_acquire))
		{
			load_page(page_index);
			flag.store(PAGE_FLAG_LOADED_FOR_READ, std::memory_order_release);
			flag.notify_all(); // Wake up waiting threads
			return root->paged_buffer.pointer_of(page_index);
		}

		// or waiting for someone who claimed it first to complete the job
		while (flag.load(std::memory_order_acquire) == PAGE_FLAG_LOADING)
			flag.wait(PAGE_FLAG_LOADING, std::memory_order_acquire);

		return root->paged_buffer.pointer_of(page_index);
	}

	uint64_t pages_count() const
	{
		return root->paged_buffer.map.count;
	}

	void close()
	{
		if (root) {
			root->refc--;
		}
	}
};

struct RootsPool
{
	Arena<SharedReadonlyPagedBuffer> arena;
	uint64_t free_slots_count = 0;
	uint64_t* free_slots = nullptr;

	static RootsPool create()
	{
		RootsPool pool;
		pool.arena = Arena<SharedReadonlyPagedBuffer>::create(INITIAL_ROOTS_POOL_CAPACITY);
		pool.free_slots_count = 0;
		pool.free_slots = new uint64_t[pool.arena.capacity];
		return pool;
	}

	void drop()
	{
		for (uint64_t i = 0; i < arena.count; i++)
			arena.buffer[i].drop();
		
		arena.drop();
		delete[] free_slots;
		free_slots = nullptr;
		free_slots_count = 0;
	}

	SharedReadonlyPagedBuffer* alloc_root()
	{
		if (free_slots_count > 0)
		{
			free_slots_count--;
			auto idx = free_slots[free_slots_count];
			return &arena.buffer[idx];
		}

		return arena.alloc();
	}
};

struct Db
{
	char const* filename = nullptr;
	file_descriptor_t fd = INVALID_HANDLE_VALUE;
	uint64_t file_size = 0;
	SharedReadonlyPagedBuffer* root = nullptr;
	RootsPool roots_pool;

	static Db open(char const* filename)
	{
		auto fd = CreateFileA(
			filename,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ,
			NULL,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);

		LARGE_INTEGER file_size_result;
		GetFileSizeEx(fd, &file_size_result);
		auto file_size = (uint64_t)file_size_result.QuadPart;

		auto roots_pool = RootsPool::create();
		auto root = roots_pool.alloc_root();
		root->paged_buffer.init(file_size);

		return Db{
			.filename = filename,
			.fd = fd,
			.file_size = file_size,
			.root = root,
			.roots_pool = roots_pool
		};
	}

	void close()
	{
		roots_pool.drop();

		if (is_filedescriptor_valid())
		{
			CloseHandle(fd);
			fd = INVALID_HANDLE_VALUE;
		}
	}

	bool is_filedescriptor_valid() const
	{
		return fd != INVALID_HANDLE_VALUE;
	}

	TrxR trx_r()
	{
		root->refc++;
		return TrxR{ .root = root, .fd = fd };
	}
};

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