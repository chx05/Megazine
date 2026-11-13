#pragma once

#define NOMINMAX
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <memory>

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

	T increment()
	{
		return get_as_atomic()++;
	}

	T decrement()
	{
		return get_as_atomic()--;
	}


	/* non atomic usage */

	// get
	T raw() const { return value; }
	T* raw_mut()  { return &value; }

	// for pointers
	T operator->() const noexcept requires std::is_pointer_v<T>      { return value; }

	// for structs
	T* operator->() noexcept requires std::is_class_v<T>             { return &value; }
	T const* operator->() const noexcept requires std::is_class_v<T> { return &value; }
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


enum struct PageFlag : uint8_t
{
	// for reader page map: means page not loaded from disk
	// for writer page map: means page never modified by write transaction
	not_loaded = 0,

	// only for reader page map
	loading = 1,

	// for reader page map: means page loaded from disk
	// for writer page map: means page modified by write transaction
	loaded = 2,

	// TODO
	// add "modified" so when memory is running low the read transaction
	// can eject some page EXCEPT the ones flagged as modified.
	// then when the file is finally flushed to the original file
	// all "modified" pages can be marked to loaded, which qualifies them as ejectable
};

using atomic_page_flag_t = PotentiallyAtomic<PageFlag>;
using file_descriptor_t = HANDLE;
// points to chunk of contiguous memory with length PAGE_SIZE
using page_ref_t = uint8_t*;
using epoch_t = uint64_t;
using page_index_t = uint64_t;
using thread_id_t = uint32_t;

constexpr uint64_t PAGE_SIZE = 16;
constexpr uint64_t INITIAL_MIN_PAGES_COUNT = 10;
constexpr uint64_t INITIAL_MIN_DIFF_PAGES_COUNT = 10;
constexpr uint64_t INITIAL_ROOTS_POOL_CAPACITY = 5;

constexpr epoch_t EPOCH_NONE = (epoch_t)4611686018427387903; // 2^62 - 1 (see ThreadInfo, its 62 bits not 64)

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
			std::copy_n(old_buffer, count, new_buffer);
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

	T& operator[](uint64_t index) { return buffer[index]; }
	const T& operator[](uint64_t index) const { return buffer[index]; }
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
	uint32_t capacity_per_node = 0;
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
		if (capacity_per_node == 0)
			return;

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
		*last_node->len.raw_mut() += requested_count;
		return ptr;
	}
};


// single threaded only
// resizable and isolated from the one in trx_r(), but it's a 1:1
// designed for writer-transactions
struct Writer_PageMap
{
	Arena<page_ref_t> refs;
	// this doesn't need to be atomic, it is always used as plain value.
	// the atomic wrapper is there so on commit i can move flags.buffer
	// directly to the new root's reader page map in O(1)
	// TODO
	// when this becomes resizable
	// remember to default the new resized space to PAGE_FLAG_NOT_LOADED
	Arena<atomic_page_flag_t> flags;

	static Writer_PageMap create(uint64_t file_size)
	{
		uint64_t initial_capacity = file_size / PAGE_SIZE;

		auto refs = Arena<page_ref_t>::create(initial_capacity);
		refs.count = initial_capacity;

		auto flags = Arena<atomic_page_flag_t>::create(initial_capacity);
		flags.count = initial_capacity;

		//std::fill_n(flags, flags.count, Writer_PageFlag::no_diff);
		for (uint64_t i = 0; i < flags.count; i++)
			flags[i] = atomic_page_flag_t::create(PageFlag::not_loaded);

		return {
			.refs = refs,
			.flags = flags,
		};
	}

	void drop()
	{
		refs.drop();
		flags.drop();
	}
};


// concurrent mapping of page_index -> page content ptr
// designed for reader-transactions
struct Reader_PageMap
{
	uint64_t count;
	page_ref_t* refs = nullptr;
	atomic_page_flag_t* flags = nullptr;

	static Reader_PageMap create(uint64_t file_size)
	{
		uint64_t count = file_size / PAGE_SIZE;
		auto refs = new page_ref_t[count];
		auto flags = new atomic_page_flag_t[count];

		//std::fill_n(flags, count, PotentiallyAtomic<Reader_PageFlag>::create(Reader_PageFlag::not_loaded));
		for (uint64_t i = 0; i < count; i++)
			flags[i] = atomic_page_flag_t::create(PageFlag::not_loaded);

		return {
			.count = count,
			.refs = refs,
			.flags = flags,
		};
	}

	// consumes a writer page map to compose a reader page map
	// it MUST NOT BE USED anymore, not even dropped
	static Reader_PageMap consume(Writer_PageMap writer_page_map)
	{
		// TODO
		// part of these two buffers are wasted (capacity - count)
		// maybe i can do something to minimize this
		return {
			.count = writer_page_map.refs.count,
			.refs = writer_page_map.refs.buffer,
			.flags = writer_page_map.flags.buffer,
		};
	}

	void drop()
	{
		if (refs == nullptr)
			return;

		delete[] refs;
		delete[] flags;
		refs = nullptr;
		flags = nullptr;
		count = 0;
	}
};


struct PagedBuffer
{
	StableArena<uint8_t> arena;
	Reader_PageMap map;

	static PagedBuffer create(uint64_t file_size)
	{
		return {
			.arena = StableArena<uint8_t>::create(PAGE_SIZE * INITIAL_MIN_PAGES_COUNT),
			.map = Reader_PageMap::create(file_size),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	void concurrent_alloc_and_map_page(page_index_t page_index)
	{
		map.refs[page_index] = (page_ref_t)arena.concurrent_alloc_many(PAGE_SIZE);
	}

	void non_concurrent_alloc_and_map_page(page_index_t page_index)
	{
		map.refs[page_index] = (page_ref_t)arena.non_concurrent_alloc_many(PAGE_SIZE);
	}

	uint8_t const* pointer_of(page_index_t page_index)
	{
		return map.refs[page_index];
	}
};

struct DiffBuffer
{
	StableArena<uint8_t> arena;
	Writer_PageMap map;

	static DiffBuffer create(uint64_t file_size)
	{
		return {
			.arena = StableArena<uint8_t>::create(PAGE_SIZE * INITIAL_MIN_DIFF_PAGES_COUNT),
			.map = Writer_PageMap::create(file_size),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	void alloc_and_map_page(page_index_t page_index)
	{
		map.refs[page_index] = (page_ref_t)arena.non_concurrent_alloc_many(PAGE_SIZE);
	}

	uint8_t const* pointer_of(page_index_t page_index)
	{
		return map.refs[page_index];
	}

	uint8_t* mut_pointer_of(page_index_t page_index)
	{
		return map.refs[page_index];
	}
};

struct Root
{
	PagedBuffer paged_buffer;

	static Root create(uint64_t file_size)
	{
		return {
			.paged_buffer = PagedBuffer::create(file_size),
		};
	}

	static Root consume(DiffBuffer diffs)
	{
		return {
			.paged_buffer = {
				.arena = diffs.arena,
				.map = Reader_PageMap::consume(diffs.map)
			}
		};
	}

	void drop()
	{
		paged_buffer.drop();
	}
};


struct Db;

struct TrxR
{
	Root* root;
	Db* db_ref;
	epoch_t epoch_stamp;
	thread_id_t thread_id;

	void load_page(page_index_t page_index);

	uint8_t const* get_page(page_index_t page_index)
	{
		auto flag = &root->paged_buffer.map.flags[page_index];

		// already in cache?
		auto current = flag->acquire();
		if (current == PageFlag::loaded)
			return root->paged_buffer.pointer_of(page_index);

		// claiming the job
		auto expected = PageFlag::not_loaded;
		if (flag->cas_strong_acquire(&expected, PageFlag::loading))
		{
			load_page(page_index);
			flag->release(PageFlag::loaded);
			flag->notify_all(); // idk, seems standard pattern
			return root->paged_buffer.pointer_of(page_index);
		}

		// or waiting for someone who claimed it first to complete the job
		while (flag->acquire() == PageFlag::loading)
			flag->wait(PageFlag::loading);

		return root->paged_buffer.pointer_of(page_index);
	}

	uint64_t pages_count() const
	{
		return root->paged_buffer.map.count;
	}

	void close();
};

enum struct TrxW_Status { Open, Closed, Aborted };
struct TrxW
{
	TrxW_Status status;
	TrxR trx_r;
	DiffBuffer diffs;

	static TrxW create(TrxR trx_r);

	uint64_t pages_count() const
	{
		return diffs.map.refs.count;
	}

	uint8_t const* get_page(page_index_t page_index)
	{
		auto flag = diffs.map.flags[page_index].raw();
		if (flag == PageFlag::loaded)
			return diffs.map.refs[page_index];

		return trx_r.get_page(page_index);
	}

	uint8_t* get_mut_page(page_index_t page_index)
	{
		auto flag = diffs.map.flags[page_index].raw();
		if (flag == PageFlag::loaded)
			return diffs.map.refs[page_index];

		auto real_page = trx_r.get_page(page_index);

		diffs.alloc_and_map_page(page_index);
		auto diff_page = diffs.mut_pointer_of(page_index);
		std::copy_n(real_page, PAGE_SIZE, diff_page);
		*diffs.map.flags[page_index].raw_mut() = PageFlag::loaded;

		return diff_page;
	}

	void abort()
	{
		if (status == TrxW_Status::Open)
		{
			status = TrxW_Status::Aborted;
			trx_r.close();
			diffs.drop();
		}
	}

	void close();
};


// TODO
// consider the use of alignas(64) which increases throughtput
// at the cost of wasting 56 bytes for each threadinfo
struct ThreadInfo
{
	epoch_t current_epoch;
	Root* root_in_use;
};

struct ThreadMap
{
	uint32_t count;
	PotentiallyAtomic<ThreadInfo>* buffer;

	static ThreadMap create(uint32_t count)
	{
		auto buffer = new PotentiallyAtomic<ThreadInfo>[count];
		for (uint64_t i = 0; i < count; i++)
			buffer[i] = PotentiallyAtomic<ThreadInfo>::create({.current_epoch = EPOCH_NONE, .root_in_use = nullptr});

		return {
			.count = count,
			.buffer = buffer,
		};
	}
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
			.pool = StableArena<Root>::create(INITIAL_ROOTS_POOL_CAPACITY)
		};
	}

	void drop()
	{
		pool.drop();
	}

	Root* alloc_root(ThreadMap const* thread_map_ref)
	{
		auto node = pool.last_node.raw();
		while (node != nullptr)
		{
			for (uint32_t i = 0; i < node->len.raw(); i++)
			{
				auto root_ref = &node->buffer[i];
				bool root_ref_is_still_in_use = false;
				for (uint32_t j = 0; j < thread_map_ref->count; j++)
				{
					// this is thread safe only as long as there is one write transaction only
					if (thread_map_ref->buffer[i].acquire().root_in_use == root_ref)
					{
						root_ref_is_still_in_use = true;
						break;
					}
				}

				if (root_ref_is_still_in_use == false)
				{
					// TODO
					// find some clever way to cleanup ALL dead roots instead of just the chosen one
					// and to do it even without creating a write transaction.
					// for example freed roots should be put in a list
					// and the list used here instead of the cleanup
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
	file_descriptor_t fd = INVALID_HANDLE_VALUE;
	uint64_t file_size;
	
	RootsPool roots_pool;
	PotentiallyAtomic<Root*> root;
	PotentiallyAtomic<epoch_t> epoch;

	ThreadMap thread_map;

	static Db open(char const* filename, uint32_t threads_count)
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

		auto thread_map = ThreadMap::create(threads_count);
		auto roots_pool = RootsPool::create();
		auto first_root = roots_pool.alloc_root(&thread_map);
		*first_root = Root::create(file_size);

		return {
			.filename = filename,
			.fd = fd,
			.file_size = file_size,
			.roots_pool = roots_pool,
			.root = first_root,
			.epoch = PotentiallyAtomic<epoch_t>::create(0),
			.thread_map = thread_map
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

	TrxR trx_r(thread_id_t thread_id)
	{
		auto epoch_stamp = epoch.increment();
		auto current_root = root.acquire();

		this->thread_map.buffer[thread_id].release({.current_epoch = epoch_stamp, .root_in_use = current_root});

		return {
			.root = current_root,
			.db_ref = this,
			.epoch_stamp = epoch_stamp,
			.thread_id = thread_id
		};
	}

	TrxW trx_w(thread_id_t thread_id)
	{
		return TrxW::create(trx_r(thread_id));
	}
};




inline void TrxR::load_page(page_index_t page_index)
{
	root->paged_buffer.concurrent_alloc_and_map_page(page_index);
	uint64_t file_offset = page_index * PAGE_SIZE;
	read_page_from_file(db_ref->fd, file_offset, (void*)root->paged_buffer.pointer_of(page_index));
}

inline void TrxR::close()
{
	if (root == nullptr)
		return;

	root = nullptr;
	db_ref->thread_map.buffer[thread_id].release({.current_epoch = EPOCH_NONE});
}

inline TrxW TrxW::create(TrxR trx_r)
{
	return {
		.status = TrxW_Status::Open,
		.trx_r = trx_r,
		.diffs = DiffBuffer::create(trx_r.db_ref->file_size),
	};
}

inline void TrxW::close()
{
	if (status == TrxW_Status::Open)
	{
		status = TrxW_Status::Closed;

		// alloc new root and cleanup previous usage, if any
		auto new_root = trx_r.db_ref->roots_pool.alloc_root(&trx_r.db_ref->thread_map);
		new_root->drop();

		// deliberately not dropping diffbuffer
		*new_root = Root::consume(diffs);
		trx_r.db_ref->root.swap_acquire_release(new_root);

		// TODO
		// flushing to .epochN.temp file
	}
}


// TODO
// as long as only one write transaction is allowed, alloc_root is ok
// but it's not thread safe right now so this must be changed for concurrent writers
//struct RootsPool
//{
//	StableArena<Root> pool;
//
//	static RootsPool create()
//	{
//		return {
//			.pool = StableArena<Root>::create(INITIAL_ROOTS_POOL_CAPACITY),
//		};
//	}
//
//	void drop()
//	{
//		pool.drop();
//	}
//
//	Root* alloc_root()
//	{
//		auto node = pool.last_node.raw();
//		while (node != nullptr)
//		{
//			for (uint32_t i = 0; i < node->len.raw(); i++)
//			{
//				auto root_ref = &node->buffer[i];
//				if (root_ref->refc.acquire() == 0)
//				{
//					// TODO
//					// find some clever way to cleanup ALL dead roots instead of just the chosen one
//					// and to do it even without creating a write transaction.
//					// for example freed roots should be put in a list
//					// and the list used here instead of the cleanup
//					root_ref->drop(); // cleanup first
//					return root_ref;
//				}
//			}
//
//			node = node->previous_node;
//		}
//
//		return pool.non_concurrent_alloc_many(1);
//	}
//};

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