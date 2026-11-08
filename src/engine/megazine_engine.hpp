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

template <typename>
constexpr bool dependent_false = false;

template<typename T>
struct PotentiallyAtomic
{
	static_assert(std::atomic_ref<T>::is_always_lock_free, "T must be lock-free");
	alignas(std::atomic_ref<T>::required_alignment) T value;

	static PotentiallyAtomic<T> create(T value)
	{
		return { .value = value };
	}

	std::atomic_ref<T> get_as_atomic()
	{
		return std::atomic_ref(value);
	}

	T swap_acquire_release(T new_value)
	{
		return get_as_atomic().exchange(new_value, std::memory_order_acq_rel);
	}

	T swap_acquire(T new_value)
	{
		return get_as_atomic().exchange(new_value, std::memory_order_acquire);
	}

	T acquire()
	{
		return get_as_atomic().load(std::memory_order_acquire);
	}

	T load_relaxed()
	{
		return get_as_atomic().load(std::memory_order_relaxed);
	}

	void release(T new_value)
	{
		return get_as_atomic().store(new_value, std::memory_order_release);
	}

	void store_relaxed(T new_value)
	{
		return get_as_atomic().store(new_value, std::memory_order_relaxed);
	}

	bool cas_weak(T* expected_ref, T desired)
	{
		return get_as_atomic().compare_exchange_weak(*expected_ref, desired, std::memory_order_acq_rel);
	}

	bool cas_weak_relaxed(T* expected_ref, T desired)
	{
		return get_as_atomic().compare_exchange_weak(*expected_ref, desired, std::memory_order_relaxed);
	}

	bool cas_strong(T* expected_ref, T desired)
	{
		return get_as_atomic().compare_exchange_strong(*expected_ref, desired, std::memory_order_acq_rel);
	}

	bool cas_strong_acquire(T* expected_ref, T desired)
	{
		return get_as_atomic().compare_exchange_strong(*expected_ref, desired, std::memory_order_acquire);
	}

	void notify_all()
	{
		get_as_atomic().notify_all();
	}

	void wait(T expected)
	{
		get_as_atomic().wait(expected, std::memory_order_acquire);
	}

	void increment()
	{
		get_as_atomic()++;
	}

	void decrement()
	{
		get_as_atomic()--;
	}


	/* non atomic usage */

	// get
	T& raw() noexcept             { return value; }
	const T& raw() const noexcept { return value; }

	// for pointers
	T operator->() const noexcept requires std::is_pointer_v<T>      { return value; }

	// for structs
	T* operator->() noexcept requires std::is_class_v<T>             { return &value; }
	const T* operator->() const noexcept requires std::is_class_v<T> { return &value; }
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


using page_flag_t = PotentiallyAtomic<uint8_t>;
using refcounter_t = PotentiallyAtomic<uint64_t>;
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
			flags[i].store_relaxed(PAGE_FLAG_NOT_LOADED);

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
struct StableArena_BufferNode
{
	PotentiallyAtomic<uint32_t> len;
	T* buffer;
	StableArena_BufferNode* previous_node;
};

template<typename T>
struct StableArena
{
	uint32_t capacity_per_node;
	PotentiallyAtomic<StableArena_BufferNode<T>*> last_node;

	static StableArena<T> create(uint32_t max_capacity_per_node)
	{
		assert(max_capacity_per_node > 0);
		return {
			.capacity_per_node = max_capacity_per_node,
			.last_node = new StableArena_BufferNode<T>{
				.len = 0,
				.buffer = new T[max_capacity_per_node],
				.previous_node = nullptr,
			},
		};
	}

	void drop()
	{
		auto* node = last_node.swap_acquire(nullptr);

		while (node)
		{
			// if .drop() is avaiable, i call it
			if constexpr (requires { node->buffer[0].drop(); }) {
				for (uint32_t i = 0; i < node->len.raw(); i++)
					node->buffer[i].drop();
			}
			// if it is available but with wrong signature, print error instead of silent ignore
			else if constexpr (requires { &T::drop; }) {
				static_assert(dependent_false<T>,
					"T has a member named 'drop', but it cannot be called as obj.drop()");
			}

			auto* prev = node->previous_node;
			delete[] node->buffer;
			delete node;
			node = prev;
		}
	}

	T* concurrent_alloc_many(uint32_t requested_count)
	{
		assert(requested_count <= capacity_per_node);

		while (true)
		{
			auto node = last_node.acquire();
			uint32_t offset = node->len.load_relaxed();

			// allocate in last_node if there is space
			while (offset + requested_count <= capacity_per_node)
			{
				if (node->len.cas_weak_relaxed(&offset, offset + requested_count))
				{
					return &node->buffer[offset];
				}
			}

			// last_node full, allocate a new one
			auto* new_node = new StableArena_BufferNode<T>{
				.len = 0,
				// TODO
				// take in input a per-thread custom allocator instead
				// and use it to allocate each node's buffer
				// so the thread has control over the allocation
				.buffer = new T[capacity_per_node],
				.previous_node = node
			};

			if (last_node.cas_strong(&node, new_node))
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

	T* non_concurrent_alloc_many(uint32_t requested_count)
	{
		assert(requested_count <= capacity_per_node);
		assert(last_node.raw() != nullptr);

		// full node: link a new node
		if (last_node->len.raw() + requested_count > capacity_per_node)
		{
			last_node = PotentiallyAtomic<StableArena_BufferNode<T>*>::create(new StableArena_BufferNode<T>{
				.len = 0,
				.buffer = new T[capacity_per_node],
				.previous_node = last_node.raw()
			});
		}

		// allocates on node
		auto ptr = &last_node->buffer[last_node->len.raw()];
		last_node->len.raw() += requested_count;
		return ptr;
	}
};


struct PagedBuffer
{
	StableArena<uint8_t> arena;
	PagesMap map;

	static PagedBuffer create(uint64_t file_size)
	{
		return {
			.arena = StableArena<uint8_t>::create(PAGE_SIZE * INITIAL_MIN_PAGES_COUNT),
			.map = PagesMap::create(file_size),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	void concurrent_alloc_and_map_page(uint64_t page_index)
	{
		map.array[page_index] = (page_ref_t)arena.concurrent_alloc_many(PAGE_SIZE);
	}

	void non_concurrent_alloc_and_map_page(uint64_t page_index)
	{
		map.array[page_index] = (page_ref_t)arena.non_concurrent_alloc_many(PAGE_SIZE);
	}

	uint8_t* pointer_of(uint64_t page_index)
	{
		return map.array[page_index];
	}
};

struct Root
{
	PagedBuffer paged_buffer;
	// TODO
	// replace refcounting with epoch based cleanup
	// so that readers can be more cache friendly
	refcounter_t refc;

	static Root create(uint64_t file_size)
	{
		return {
			.paged_buffer = PagedBuffer::create(file_size),
			.refc = refcounter_t::create(1),
		};
	}

	void drop()
	{
		paged_buffer.drop();
	}

	void drop_and_decrement_refc()
	{
		drop();
		refc.decrement();
	}
};

struct TrxR
{
	Root* root;
	file_descriptor_t fd;

	void load_page(uint64_t page_index)
	{
		root->paged_buffer.concurrent_alloc_and_map_page(page_index);
		uint64_t file_offset = page_index * PAGE_SIZE;
		read_page_from_file(fd, file_offset, root->paged_buffer.pointer_of(page_index));
	}

	uint8_t const* get_page(uint64_t page_index)
	{
		auto flag = &root->paged_buffer.map.flags[page_index];

		// already in cache?
		uint8_t current = flag->acquire();
		if (current >= PAGE_FLAG_LOADED_FOR_READ)
			return root->paged_buffer.pointer_of(page_index);

		// claiming the job
		uint8_t expected = PAGE_FLAG_NOT_LOADED;
		if (flag->cas_strong_acquire(&expected, PAGE_FLAG_LOADING))
		{
			load_page(page_index);
			flag->release(PAGE_FLAG_LOADED_FOR_READ);
			flag->notify_all(); // idk, seems standard pattern
			return root->paged_buffer.pointer_of(page_index);
		}

		// or waiting for someone who claimed it first to complete the job
		while (flag->acquire() == PAGE_FLAG_LOADING)
			flag->wait(PAGE_FLAG_LOADING);

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
			root->refc.decrement();
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
	PagedBuffer diffs;

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
		auto node = pool.last_node.raw();
		while (node != nullptr)
		{
			for (uint32_t i = 0; i < node->len.raw(); i++)
			{
				auto root_ref = &node->buffer[i];
				if (root_ref->refc.acquire() == 0)
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

		return pool.non_concurrent_alloc_many(1);
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
	PotentiallyAtomic<Root*> root;

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

	TrxR trx_r()
	{
		while (true)
		{
			auto initial_r = root.acquire();
			uint64_t count = initial_r->refc.load_relaxed();

			// increment only if not already 0
			while (count > 0 && !initial_r->refc.cas_weak(&count, count + 1))
			{
				// nothing
			}

			if (count == 0)
				continue; // someone is dropping the root, retry with updated root

			// if root hasn't been replaced while incrementing
			if (root.acquire() == initial_r)
				return TrxR{ .root = initial_r, .fd = fd };

			// if it was replaced mid flight, undo refcount
			initial_r->refc.decrement();
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
		.diffs = PagedBuffer::create(db_ref->file_size),
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

		auto old_root = db_ref->root.swap_acquire_release(new_root);
		old_root->refc.decrement();
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