#pragma once

#define NOMINMAX
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cassert>

// TODO
// try using mmap() for lazy loading instead, the mutex and all the stuff
// made this implementation weird.
// for writers, the diff mechanism can be built ontop of it
// for readers it makes no difference
// BUT first it must be clear what's the workflow for committing correctly

// TODO
// add asserts and report helpers in debug builds


template<typename T>
struct Atomic : std::atomic<T>
{
	using std::atomic<T>::atomic;

	constexpr Atomic() noexcept : std::atomic<T>(T{}) {}
	constexpr Atomic(T val) noexcept : std::atomic<T>(val) {}

	Atomic(const Atomic& other) noexcept
		: std::atomic<T>(other.load(std::memory_order_relaxed)) {}

	Atomic& operator=(const Atomic& other) noexcept
	{
		this->store(other.load(std::memory_order_relaxed), std::memory_order_relaxed);
		return *this;
	}

	Atomic& operator=(T val) noexcept
	{
		this->store(val, std::memory_order_relaxed);
		return *this;
	}
};


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


using page_flag_t = Atomic<uint8_t>;
using refcounter_t = Atomic<uint64_t>;
using file_descriptor_t = HANDLE;
// points to chunk of contiguous memory with length PAGE_SIZE
using page_ref_t = uint8_t*;

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
	ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == 0)
		return false;

	DWORD bytes = 0;
	BOOL ok = ReadFile(fd, dest_buffer, (DWORD)PAGE_SIZE, &bytes, &ov);
	if (!ok)
	{
		if (GetLastError() == ERROR_IO_PENDING)
		{
			// Wait synchronously for this specific thread's operation
			ok = GetOverlappedResult(fd, &ov, &bytes, TRUE);
		}
	}

	CloseHandle(ov.hEvent);
	return ok && (bytes == PAGE_SIZE);
}

// TODO this must be ported to async as well like read_page_from_file
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
	uint64_t count;
	uint64_t capacity;
	page_ref_t* array;
	page_flag_t* flags;

	static PagesMap create(uint64_t file_size)
	{
		uint64_t count = file_size / PAGE_SIZE;
		// TODO this could theoretically go away if used only for reader transactions
		uint64_t capacity = std::max(count, INITIAL_MIN_PAGES_COUNT);
		auto* array = new page_ref_t[capacity];
		auto* flags = new page_flag_t[capacity];

		// TODO memset should be safe here
		for (uint64_t i = 0; i < capacity; ++i)
			flags[i].store(PAGE_FLAG_NOT_LOADED, std::memory_order_relaxed);

		return {
			.count = count,
			.capacity = capacity,
			.array = array,
			.flags = flags,
		};
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
	uint64_t count;
	uint64_t capacity;
	T* buffer;

	static Arena create(uint64_t initial_capacity)
	{
		return {
			.count = 0,
			.capacity = initial_capacity,
			.buffer = new T[initial_capacity],
		};
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
			auto old_buffer = buffer;

			capacity = std::max(capacity * 2, capacity + incoming_count);
			auto new_buffer = new T[capacity];
			memcpy(new_buffer, old_buffer, count); // this is bypassing RAII, which is good in this code base
			buffer = new_buffer;

			delete[] old_buffer;
		}
	}

	T* alloc_many(uint64_t requested_count)
	{
		ensure_has_space_for(requested_count);
		auto start = count;
		count += requested_count;
		return &buffer[start];
	}
};

template<typename T>
struct StableArenaBufferNode
{
	uint32_t len;
	T* buffer;
	StableArenaBufferNode* previous_node;
};

template<typename T>
struct StableArena
{
	uint32_t capacity_per_node;
	StableArenaBufferNode<T>* last_node;

	static StableArena create(uint32_t max_capacity_per_node)
	{
		assert(max_capacity_per_node > 0);
		return {
			.capacity_per_node = max_capacity_per_node,
			.last_node = new StableArenaBufferNode<T>{
				.len = 0,
				.buffer = new T[max_capacity_per_node],
				.previous_node = nullptr,
			},
		};
	}

	void drop()
	{
		auto* node = last_node;
		last_node = nullptr;

		while (node)
		{
			for (uint32_t i = 0; i < node->len; i++)
				node->buffer[i].drop();

			auto* prev = node->previous_node;
			delete[] node->buffer;
			delete node;
			node = prev;
		}
	}

	T* alloc_many(uint32_t requested_count)
	{
		assert(requested_count <= capacity_per_node);
		assert(last_node != nullptr);

		// full node: link a new node
		if (last_node->len + requested_count > capacity_per_node)
		{
			last_node = new StableArenaBufferNode<T>{
				.len = 0,
				.buffer = new T[capacity_per_node],
				.previous_node = last_node
			};
		}

		// allocates on node
		T* ptr = &last_node->buffer[last_node->len];
		last_node->len += requested_count;
		return ptr;
	}
};


struct SharedStableArena_BufferNode
{
	Atomic<uint32_t> len;
	uint8_t* buffer;
	SharedStableArena_BufferNode* previous_node;
};

struct SharedStableArena
{
	uint32_t capacity_per_node;
	Atomic<SharedStableArena_BufferNode*> last_node;

	static SharedStableArena create(uint32_t initial_capacity)
	{
		auto* first_node = new SharedStableArena_BufferNode{
			.len = 0,
			.buffer = new uint8_t[initial_capacity],
			.previous_node = nullptr,
		};

		return {
			.capacity_per_node = initial_capacity,
			.last_node = first_node,
		};
	}

	void drop()
	{
		auto* curr = last_node.exchange(nullptr, std::memory_order_acquire);
		while (curr)
		{
			auto* prev = curr->previous_node;
			delete[] curr->buffer;
			delete curr;
			curr = prev;
		}
	}

	uint8_t* alloc(uint32_t requested_size)
	{
		assert(requested_size <= capacity_per_node);

		while (true)
		{
			auto* node = last_node.load(std::memory_order_acquire);
			uint32_t offset = node->len.load(std::memory_order_relaxed);

			// allocate in last_node if there is space
			while (offset + requested_size <= capacity_per_node)
			{
				if (node->len.compare_exchange_weak(/*by ref*/ offset, offset + requested_size, std::memory_order_relaxed))
				{
					return &node->buffer[offset];
				}
			}

			// last_node full, allocate a new one
			auto* new_node = new SharedStableArena_BufferNode{
				.len = 0,
				// TODO
				// take in input a per-thread custom allocator instead
				// and use it to allocate each node's buffer
				// so the thread has control over the allocation
				.buffer = new uint8_t[capacity_per_node],
				.previous_node = node
			};

			if (last_node.compare_exchange_strong(node, new_node,
				std::memory_order_release,
				std::memory_order_acquire))
			{
				// winner, the next cycle will allocate
			}
			else
			{
				// looser, another thread added a new node first
				// i clean up and the next cycle will allocate using that
				delete[] new_node->buffer;
				delete new_node;
			}
		}
	}
};


struct SharedPagedBuffer
{
	SharedStableArena arena;
	PagesMap map;

	static SharedPagedBuffer create(uint64_t file_size)
	{
		return {
			.arena = SharedStableArena::create(PAGE_SIZE * INITIAL_MIN_PAGES_COUNT),
			.map = PagesMap::create(file_size),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	void alloc_and_map_page(uint64_t page_index)
	{
		map.array[page_index] = (page_ref_t)arena.alloc(PAGE_SIZE);
	}

	uint8_t* pointer_of(uint64_t page_index)
	{
		return map.array[page_index];
	}
};

struct Root
{
	SharedPagedBuffer paged_buffer;
	// TODO
	// replace refcounting with epoch based cleanup
	// so that readers can be more cache friendly
	refcounter_t refc;

	static Root create(uint64_t file_size)
	{
		return {
			.paged_buffer = SharedPagedBuffer::create(file_size),
			.refc = 1,
		};
	}

	void drop()
	{
		paged_buffer.drop();
	}

	void drop_and_decrement_refc()
	{
		drop();
		refc--;
	}
};

struct TrxR
{
	Root* root;
	file_descriptor_t fd;

	void load_page(uint64_t page_index)
	{
		root->paged_buffer.alloc_and_map_page(page_index);
		uint64_t file_offset = page_index * PAGE_SIZE;
		read_page_from_file(fd, file_offset, root->paged_buffer.pointer_of(page_index));
	}

	uint8_t const* get_page(uint64_t page_index)
	{
		auto flag = &root->paged_buffer.map.flags[page_index];

		// already in cache?
		uint8_t current = flag->load(std::memory_order_acquire);
		if (current >= PAGE_FLAG_LOADED_FOR_READ)
			return root->paged_buffer.pointer_of(page_index);

		// claiming the job
		uint8_t expected = PAGE_FLAG_NOT_LOADED;
		if (flag->compare_exchange_strong(expected, PAGE_FLAG_LOADING, std::memory_order_acquire))
		{
			load_page(page_index);
			flag->store(PAGE_FLAG_LOADED_FOR_READ, std::memory_order_release);
			flag->notify_all(); // idk, seems standard pattern
			return root->paged_buffer.pointer_of(page_index);
		}

		// or waiting for someone who claimed it first to complete the job
		while (flag->load(std::memory_order_acquire) == PAGE_FLAG_LOADING)
			flag->wait(PAGE_FLAG_LOADING, std::memory_order_acquire);

		return root->paged_buffer.pointer_of(page_index);
	}

	uint64_t pages_count() const
	{
		return root->paged_buffer.map.count;
	}

	void close()
	{
		if (root != nullptr)
		{
			root->refc--;
			root = nullptr;
		}
	}
};

struct Db;
enum struct TrxW_Status { Open, Closed, Aborted };
struct TrxW
{
	TrxW_Status status;

	Db* db_ref;
	TrxR trx_r;

	Root* new_root;
	SharedPagedBuffer diffs;

	static TrxW create(Db* db_ref, TrxR trx_r, Root* new_root);

	uint64_t pages_count() const
	{
		return new_root->paged_buffer.map.count;
	}

	uint8_t const* get_page(uint64_t page_index)
	{
		(void)page_index;
		return nullptr;
	}

	uint8_t* get_mut_page(uint64_t page_index)
	{
		(void)page_index;
		return nullptr;
	}

	void abort();
	void close();
};

// TODO
// as long as only one write transaction is allowed, alloc_root is ok
// but it's not thread safe right now so this must be changed for concurrent writers
struct RootsPool
{
	StableArena<Root> pool;

	static RootsPool create()
	{
		return {
			.pool = StableArena<Root>::create(INITIAL_ROOTS_POOL_CAPACITY),
		};
	}

	void drop()
	{
		pool.drop();
	}

	Root* alloc_root()
	{
		auto node = pool.last_node;
		while (node != nullptr)
		{
			for (uint32_t i = 0; i < node->len; i++)
			{
				auto root_ref = &node->buffer[i];
				if (root_ref->refc.load(std::memory_order_acquire) == 0)
				{
					// TODO
					// find some clever way to cleanup ALL dead roots instead of just the chosen one
					// and to do it even without creating a write transaction
					root_ref->drop(); // cleanup first
					return root_ref;
				}
			}

			node = node->previous_node;
		}

		return pool.alloc_many(1);
	}
};

struct Db
{
	char const* filename;
	file_descriptor_t fd;
	uint64_t file_size;
	// TODO
	// if only one writer transaction is allowed
	// then probably no pool is needed, just a 2 element swap
	RootsPool roots_pool;
	Atomic<Root*> root;

	static Db open(char const* filename)
	{
		auto fd = CreateFileA(
			filename,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ,
			NULL,
			OPEN_ALWAYS,
			FILE_FLAG_OVERLAPPED,
			NULL
		);

		LARGE_INTEGER file_size_result;
		GetFileSizeEx(fd, &file_size_result);
		auto file_size = (uint64_t)file_size_result.QuadPart;

		auto roots_pool = RootsPool::create();
		auto root = roots_pool.alloc_root();
		*root = Root::create(file_size);

		return {
			.filename = filename,
			.fd = fd,
			.file_size = file_size,
			.roots_pool = roots_pool,
			.root = root
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

	TrxR trx_r() const
	{
		while (true)
		{
			auto* r = root.load(std::memory_order_acquire);
			uint64_t count = r->refc.load(std::memory_order_relaxed);

			// increment only if not already 0
			while (count > 0 && !r->refc.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel))
			{
				// nothing
			}

			if (count == 0)
				continue; // someone is dropping the root, retry with updated root

			// if root hasn't been replaced while incrementing
			if (root.load(std::memory_order_acquire) == r)
				return TrxR{ .root = r, .fd = fd };

			// if it was replaced mid flight, undo refcount
			r->refc--;
		}
	}

	TrxW trx_w()
	{
		auto new_root = roots_pool.alloc_root();
		*new_root = Root::create(file_size);

		return TrxW::create(this, trx_r(), new_root);
	}
};


inline TrxW TrxW::create(Db* db_ref, TrxR trx_r, Root* new_root)
{
	return {
		.status = TrxW_Status::Open,
		.db_ref = db_ref,
		.trx_r = trx_r,
		.new_root = new_root,
		.diffs = SharedPagedBuffer::create(db_ref->file_size),
	};
}

inline void TrxW::abort()
{
	if (status == TrxW_Status::Open)
	{
		status = TrxW_Status::Aborted;
		trx_r.close();
		new_root->drop_and_decrement_refc();
	}
}

inline void TrxW::close()
{
	if (status == TrxW_Status::Open)
	{
		status = TrxW_Status::Closed;
		trx_r.close();

		auto old_root = db_ref->root.exchange(new_root, std::memory_order_acq_rel);
		old_root->refc--;
	}
}


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