#pragma once

#ifndef NOMINMAX
	#define NOMINMAX
#endif

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <memory>

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

	T load_seq_cst()
	{
		return get_as_atomic().load(std::memory_order_seq_cst);
	}

	void store_seq_cst(T v)
	{
		get_as_atomic().store(v, std::memory_order_seq_cst);
	}

	T swap_seq_cst(T v)
	{
		return get_as_atomic().exchange(v, std::memory_order_seq_cst);
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

	// only for reader page map
	loaded = 2,

	// reader page map: a previous write transaction modified and committed this page
	// writer page map: current write transaction modified and committed this page
	modified = 3
};

using atomic_page_flag_t = PotentiallyAtomic<PageFlag>;
using file_descriptor_t = HANDLE;
// points to chunk of contiguous memory with length PAGE_SIZE
using page_ref_t = uint8_t*;
using epoch_t = uint64_t;
using page_index_t = uint64_t;
using thread_id_t = uint32_t;

constexpr epoch_t EPOCH_NONE = (epoch_t)-1;

struct DbConfig
{
	// all of these can be changed on the fly even during the program is running
	// but be very cautious about what you change
	uint64_t page_size = 4096;
	uint64_t initial_pages_capacity_reader = 10;
	uint64_t initial_pages_capacity_writer = 10;
	uint64_t initial_roots_pool_capacity = 5;

	// none of these can be changed after initialization, new values will be ignored
	char const* postfix_for_temp_commit_file = ".last_commit";
	bool commit_atomically = true;
	uint32_t threads_count = 1;
};

bool read_page_from_file(file_descriptor_t fd, page_index_t page_index, void* dest_buffer, DbConfig const* config)
{
	auto file_offset = page_index * config->page_size;

	OVERLAPPED ov = { 0 };
	ov.Offset = (DWORD)(file_offset);
	ov.OffsetHigh = (DWORD)(file_offset >> 32);
	ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == 0)
		return false;

	DWORD bytes = 0;
	BOOL ok = ReadFile(fd, dest_buffer, (DWORD)config->page_size, &bytes, &ov);
	if (!ok)
	{
		if (GetLastError() == ERROR_IO_PENDING)
		{
			// wait synchronously for this specific thread's operation
			ok = GetOverlappedResult(fd, &ov, &bytes, TRUE);
		}
	}

	CloseHandle(ov.hEvent);
	return ok && (bytes == config->page_size);
}

bool write_page_to_file(file_descriptor_t fd, page_index_t page_index, void const* source_buffer, DbConfig const* config)
{
	auto file_offset = page_index * config->page_size;

	OVERLAPPED ov = { 0 };
	ov.Offset = (DWORD)(file_offset);
	ov.OffsetHigh = (DWORD)(file_offset >> 32);

	DWORD bytes = 0;
	return WriteFile(fd, source_buffer, (DWORD)config->page_size, &bytes, &ov);
}

void write_all_file(file_descriptor_t fd, const void* data, uint64_t size)
{
    LARGE_INTEGER li;

    // 1. Pre-allocate exact size & truncate old data (prevents NTFS fragmentation/allocation overhead)
    li.QuadPart = size;
    SetFilePointerEx(fd, li, NULL, FILE_BEGIN);
    SetEndOfFile(fd);

    // 2. Rewind to beginning
    li.QuadPart = 0;
    SetFilePointerEx(fd, li, NULL, FILE_BEGIN);

    // 3. Write sequentially
    const auto* ptr = static_cast<const unsigned char*>(data);
    while (size > 0)
    {
        DWORD to_write = static_cast<DWORD>(size > 0xFFFFFFFF ? 0xFFFFFFFF : size);
        DWORD written = 0;
        if (!WriteFile(fd, ptr, to_write, &written, NULL) || written == 0)
            break;

        ptr += written;
        size -= written;
    }
}

bool atomic_write_file(const char* path, const void* data, uint64_t size)
{
	char tmp_path[MAX_PATH];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	// 1. Write to a temporary file in the same directory
	HANDLE h = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return false;

	DWORD written;
	if (!WriteFile(h, data, (DWORD)size, &written, NULL) || written != (DWORD)size) {
		CloseHandle(h);
		DeleteFileA(tmp_path);
		return false;
	}

	// 2. Ensure data hits disk before replacing
	FlushFileBuffers(h);
	CloseHandle(h);

	// 3. Atomically replace the target file
	if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileA(tmp_path);
		return false;
	}

	return true;
}

bool file_exists(const char* path)
{
	DWORD attr = GetFileAttributesA(path);
	return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

uint8_t* read_all_file(const char* path, uint64_t* out_size)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return nullptr;

	LARGE_INTEGER size;
	if (!GetFileSizeEx(h, &size)) {
		CloseHandle(h);
		return nullptr;
	}

	uint8_t* buffer = new uint8_t[size.QuadPart];
	uint64_t total_read = 0;

	while (total_read < (uint64_t)size.QuadPart) {
		DWORD to_read = (DWORD)std::min((uint64_t)size.QuadPart - total_read, (uint64_t)MAXDWORD);
		DWORD bytes_read = 0;
		if (!ReadFile(h, buffer + total_read, to_read, &bytes_read, nullptr) || bytes_read == 0) {
			delete[] buffer;
			CloseHandle(h);
			return nullptr;
		}
		total_read += bytes_read;
	}

	CloseHandle(h);
	if (out_size) *out_size = total_read;
	return buffer; // Free with: delete[] buffer;
}

bool resize_file(file_descriptor_t fd, uint64_t page_count, DbConfig const* config)
{
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)(page_count * config->page_size);
	return SetFilePointerEx(fd, li, NULL, FILE_BEGIN) && SetEndOfFile(fd);
}

void write_buffer(void* dest, uint64_t* cursor, void const* buffer, uint64_t count)
{
	memcpy((void*)((uint64_t)dest + *cursor), buffer, count);
	*cursor += count;
}

void write_u64(void* dest, uint64_t* cursor, uint64_t value)
{
	write_buffer(dest, cursor, &value, sizeof(uint64_t));
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

	void enlarge_if_needed(uint64_t incoming_count)
	{
		if (count + incoming_count <= capacity)
			return;
		
		auto old_buffer = buffer;

		capacity = std::max(capacity * 2, capacity + incoming_count);
		auto new_buffer = new T[capacity];
		std::copy_n(old_buffer, count, new_buffer);
		buffer = new_buffer;

		delete[] old_buffer;
	}

	T* alloc_many(uint64_t requested_count)
	{
		enlarge_if_needed(requested_count);
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
	PotentiallyAtomic<uint64_t> len;
	T* buffer;
	StableArena_BufferNode* previous_node;
};

template<typename T>
struct StableArena
{
	uint64_t capacity_per_node = 0;
	PotentiallyAtomic<StableArena_BufferNode<T>*> last_node;

	static StableArena<T> create(uint64_t max_capacity_per_node)
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
				for (auto i = 0; i < node->len.raw(); i++)
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

	T* concurrent_alloc_many(uint64_t requested_count)
	{
		assert(requested_count <= capacity_per_node);

		while (true)
		{
			auto node = last_node.acquire();
			auto offset = node->len.load_relaxed();

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

	T* non_concurrent_alloc_many(uint64_t requested_count)
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
	Arena<atomic_page_flag_t> flags;

	static Writer_PageMap create(uint64_t file_size, DbConfig const* config)
	{
		uint64_t initial_capacity = file_size / config->page_size;

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

	void ensure_capacity(uint64_t expected_capacity)
	{
		auto current_pages_count = flags.count;
		auto incoming_count = expected_capacity - current_pages_count;
		assert(incoming_count >= 0);

		refs.enlarge_if_needed(incoming_count);
		flags.enlarge_if_needed(incoming_count);
	}
};


// concurrent mapping of page_index -> page content ptr
// designed for reader-transactions
struct Reader_PageMap
{
	uint64_t count;
	page_ref_t* refs = nullptr;
	atomic_page_flag_t* flags = nullptr;

	static Reader_PageMap create(uint64_t file_size, DbConfig const* config)
	{
		uint64_t count = file_size / config->page_size;
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

	static PagedBuffer create(uint64_t file_size, DbConfig const* config)
	{
		return {
			.arena = StableArena<uint8_t>::create(config->page_size * config->initial_pages_capacity_reader),
			.map = Reader_PageMap::create(file_size, config),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	uint64_t count() const
	{
		return map.count;
	}

	void concurrent_alloc_and_map_page(page_index_t page_index, DbConfig const* config)
	{
		map.refs[page_index] = (page_ref_t)arena.concurrent_alloc_many(config->page_size);
	}

	void non_concurrent_alloc_and_map_page(page_index_t page_index, DbConfig const* config)
	{
		map.refs[page_index] = (page_ref_t)arena.non_concurrent_alloc_many(config->page_size);
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

	static DiffBuffer create(uint64_t file_size, DbConfig const* config)
	{
		return {
			.arena = StableArena<uint8_t>::create(config->page_size * config->initial_pages_capacity_writer),
			.map = Writer_PageMap::create(file_size, config),
		};
	}

	void drop()
	{
		arena.drop();
		map.drop();
	}

	uint64_t count() const
	{
		return map.flags.count;
	}

	void alloc_and_map_page(page_index_t page_index, DbConfig const* config)
	{
		map.refs[page_index] = (page_ref_t)arena.non_concurrent_alloc_many(config->page_size);
	}

	uint8_t const* pointer_of(page_index_t page_index)
	{
		return map.refs[page_index];
	}

	uint8_t* mut_pointer_of(page_index_t page_index)
	{
		return map.refs[page_index];
	}

	void flush_diffs_to_commit_file(file_descriptor_t last_commit_fd, char const* last_commit_filename, DbConfig const* config) const
	{
		// counting pages
		uint64_t pages_count = 0;
		for (auto i = 0; i < map.flags.count; i++)
			if (map.flags.buffer[i].raw() == PageFlag::modified)
				pages_count++;

		// TODO avoid allocation here
		auto dest_size =
			+ sizeof(uint64_t) // total pages count
			+ sizeof(uint64_t) // diffed pages count
			+ (pages_count * sizeof(uint64_t) + pages_count * config->page_size)
			; // indices + contents

		auto dest = new uint8_t[dest_size];
		uint64_t cursor = 0;

		write_u64(dest, &cursor, count());     // total pages count
		write_u64(dest, &cursor, pages_count); // diffed pages count

		// writing all page indices
		for (auto i = 0; i < map.flags.count; i++)
			if (map.flags.buffer[i].raw() == PageFlag::modified)
				write_u64(dest, &cursor, i);

		// writing all page contents
		for (auto i = 0; i < map.flags.count; i++)
			if (map.flags.buffer[i].raw() == PageFlag::modified)
				write_buffer(dest, &cursor, map.refs.buffer[i], config->page_size);

		if (config->commit_atomically)
			atomic_write_file(last_commit_filename, dest, dest_size);
		else
			write_all_file(last_commit_fd, dest, dest_size);
		
		delete[] dest;
	}

	void apply_to_master_file(file_descriptor_t fd, DbConfig const* config)
	{
		resize_file(fd, count(), config);

		for (auto i = 0; i < map.flags.count; i++)
			if (map.flags.buffer[i].raw() == PageFlag::modified)
			{
				*map.flags.buffer[i].raw_mut() = PageFlag::not_loaded;
				write_page_to_file(fd, i, map.refs.buffer[i], config);
			}

		// TODO
		// idk rethink this
		// instead of dropping, the chain should be pruned
		// to leave only one node
		auto capacity_per_node = arena.capacity_per_node;
		arena.drop();
		arena = StableArena<uint8_t>::create(capacity_per_node);
	}
};

struct Root
{
	PagedBuffer paged_buffer;
	epoch_t epoch_stamp;

	static Root create(epoch_t epoch_stamp, uint64_t file_size, DbConfig const* config)
	{
		return {
			.paged_buffer = PagedBuffer::create(file_size, config),
			.epoch_stamp = epoch_stamp
		};
	}

	static Root consume(epoch_t epoch_stamp, DiffBuffer diffs)
	{
		return {
			.paged_buffer = {
				.arena = diffs.arena,
				.map = Reader_PageMap::consume(diffs.map)
			},
			.epoch_stamp = epoch_stamp
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
	thread_id_t thread_id;

	void load_page(page_index_t page_index);

	uint8_t const* get_page(page_index_t page_index)
	{
		auto flag = &root->paged_buffer.map.flags[page_index];

		// already in cache?
		auto current = flag->acquire();
		if (current >= PageFlag::loaded)
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

	uint64_t page_size() const
	{
		return db_config()->page_size;
	}

	DbConfig const* db_config() const;

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

	uint64_t file_size() const
	{
		return pages_count() * page_size();
	}

	uint8_t const* get_page(page_index_t page_index)
	{
		auto flag = diffs.map.flags[page_index].raw();
		if (flag == PageFlag::modified)
			return diffs.map.refs[page_index];

		return trx_r.get_page(page_index);
	}

	uint8_t* get_mut_page(page_index_t page_index)
	{
		auto flag = diffs.map.flags[page_index].raw();
		if (flag == PageFlag::modified)
			return diffs.map.refs[page_index];

		auto real_page = trx_r.get_page(page_index);

		diffs.alloc_and_map_page(page_index, db_config());
		auto diff_page = diffs.mut_pointer_of(page_index);
		std::copy_n(real_page, page_size(), diff_page);
		*diffs.map.flags[page_index].raw_mut() = PageFlag::modified;

		return diff_page;
	}

	uint64_t page_size() const
	{
		return db_config()->page_size;
	}

	DbConfig const* db_config() const;

	page_index_t push_page()
	{
		modify_file_size(file_size() + page_size());
		return pages_count() - 1;
	}

	void pop_page()
	{
		assert(pages_count() > 0);
		modify_file_size(file_size() - page_size());
	}

	void modify_file_size(uint64_t new_file_size)
	{
		assert(new_file_size % page_size() == 0);

		auto previous_pages_count = diffs.count();
		auto new_pages_count = new_file_size / page_size();

		if (new_pages_count > previous_pages_count)
			diffs.map.ensure_capacity(new_pages_count);

		diffs.map.flags.count = new_pages_count;
		diffs.map.refs.count = new_pages_count;

		// alloc and update default flag to new uninitialized space
		for (uint64_t i = previous_pages_count; i < new_pages_count; i++)
		{
			diffs.alloc_and_map_page(i, db_config());
			diffs.map.flags[i] = atomic_page_flag_t::create(PageFlag::modified);
		}
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
// test this without alignment as well
// since it might not even compile on hw without 16byte atomic ops
struct AtomicThreadInfo
{
	PotentiallyAtomic<epoch_t> current_epoch;
	PotentiallyAtomic<Root*> root_in_use;
};

#pragma warning(push)
#pragma warning(disable : 4324)
	template<typename T>
	struct alignas(64) Aligned64
	{
		T aligned_value;
	};
#pragma warning(pop)

struct ThreadMap
{
	uint64_t count;
	Aligned64<AtomicThreadInfo>* buffer;

	static ThreadMap create(DbConfig const* config)
	{
		auto buffer = new Aligned64<AtomicThreadInfo>[config->threads_count];
		for (uint32_t i = 0; i < config->threads_count; i++)
			buffer[i] = {
				.aligned_value = {
					.current_epoch = PotentiallyAtomic<epoch_t>::create(EPOCH_NONE),
					.root_in_use = PotentiallyAtomic<Root*>::create(nullptr)
				}
			};

		return {
			.count = config->threads_count,
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

	static RootsPool create(DbConfig const* config)
	{
		return {
			.pool = StableArena<Root>::create(config->initial_roots_pool_capacity)
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
			for (auto i = 0; i < node->len.raw(); i++)
			{
				auto root_ref = &node->buffer[i];
				bool root_ref_is_still_in_use = false;
				for (auto j = 0; j < thread_map_ref->count; j++)
				{
					// this is thread safe only as long as there is one write transaction only
					if (thread_map_ref->buffer[j].aligned_value.root_in_use.acquire() == root_ref)
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
	DbConfig config;

	char const* filename;
	file_descriptor_t fd = INVALID_HANDLE_VALUE;

	char const* last_commit_filename;
	// IMPORTANT: this is only used when config.commit_atomically=false
	file_descriptor_t last_commit_fd = INVALID_HANDLE_VALUE;
	
	RootsPool roots_pool;
	PotentiallyAtomic<Root*> root;
	PotentiallyAtomic<epoch_t> epoch;

	ThreadMap thread_map;

	// TODO put threads_count inside config instead
	static Db open(char const* filename, DbConfig config)
	{
		auto master_file_existed_before_opening_fd = file_exists(filename);
		auto fd = CreateFileA(
			filename,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ,
			NULL,
			OPEN_ALWAYS,
			FILE_FLAG_OVERLAPPED,
			NULL
		);

		// TODO avoid using strlen here, use ptr+len strings instead
		auto last_commit_filename_len = strlen(filename) + strlen(config.postfix_for_temp_commit_file);
		auto last_commit_filename = new char[last_commit_filename_len + 1];
		snprintf(last_commit_filename, last_commit_filename_len + 1, "%s%s", filename, config.postfix_for_temp_commit_file);

		if (file_exists(last_commit_filename))
		{
			// we ignore the last commit if the master file did not exist on db open
			// the useless commit file will be deleted as well after this
			if (master_file_existed_before_opening_fd)
			{
				// easier to understand by taking a look at flush_diffs_to_commit_file
				uint64_t last_commit_size;
				auto last_commit = read_all_file(last_commit_filename, &last_commit_size);
				auto last_commit_u64 = (uint64_t const*)last_commit;
				auto total_pages_count = last_commit_u64[0];
				auto diff_pages_count = last_commit_u64[1];
				auto indices = &last_commit_u64[2];
				auto contents = (uint8_t const*)(
					&indices[diff_pages_count]
				);

				resize_file(fd, total_pages_count, &config);

				for (auto i = 0; i < diff_pages_count; i++)
					write_page_to_file(fd, indices[i], &contents[i * config.page_size], &config);

				delete[] last_commit;
			}

			std::remove(last_commit_filename);
		}

		file_descriptor_t last_commit_fd =
			config.commit_atomically
			? INVALID_HANDLE_VALUE
			: CreateFileA(
				last_commit_filename,
				GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ,
				NULL,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				NULL
			);

		LARGE_INTEGER file_size_result;
		GetFileSizeEx(fd, &file_size_result);
		auto file_size = (uint64_t)file_size_result.QuadPart;

		auto starting_epoch = PotentiallyAtomic<epoch_t>::create(0);
		auto thread_map = ThreadMap::create(&config);
		auto roots_pool = RootsPool::create(&config);
		auto first_root = roots_pool.alloc_root(&thread_map);
		*first_root = Root::create(starting_epoch.increment(), file_size, &config);

		return {
			.config = config,

			.filename = filename,
			.fd = fd,

			.last_commit_filename = last_commit_filename,
			.last_commit_fd = last_commit_fd,

			.roots_pool = roots_pool,
			.root = first_root,
			.epoch = starting_epoch,
			.thread_map = thread_map
		};
	}

	uint64_t file_size()
	{
		return root.acquire()->paged_buffer.count() * config.page_size;
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
		auto slot = &thread_map.buffer[thread_id].aligned_value;
		
		// take the first root that is not being discarded in that same moment
		Root* current_root;
		for (;;)
		{
			current_root = root.acquire();
			// announce
			slot->current_epoch.store_seq_cst(current_root->epoch_stamp);
			slot->root_in_use.store_seq_cst(current_root);
			// if the root changed in between, the writer may have missed the announcement -> retry
			if (root.load_seq_cst() == current_root)
				break;
		}
		
		return {
			.root = current_root,
			.db_ref = this,
			.thread_id = thread_id
		};
	}

	TrxW trx_w(thread_id_t thread_id)
	{
		return TrxW::create(trx_r(thread_id));
	}

	bool is_youngest_trx(epoch_t trx_epoch)
	{
		for (auto i = 0; i < thread_map.count; i++)
			if (thread_map.buffer[i].aligned_value.current_epoch.load_relaxed() < trx_epoch)
				return false;
		
		return true;
	}
};




void TrxR::load_page(page_index_t page_index)
{
	root->paged_buffer.concurrent_alloc_and_map_page(page_index, db_config());
	read_page_from_file(db_ref->fd, page_index, (void*)root->paged_buffer.pointer_of(page_index), db_config());
}

DbConfig const* TrxR::db_config() const
{
	return &db_ref->config;
}

void TrxR::close()
{
	if (root == nullptr)
		return;

	root = nullptr;
	auto slot = &db_ref->thread_map.buffer[thread_id].aligned_value;
	slot->current_epoch.release(EPOCH_NONE);
	slot->root_in_use.release(nullptr);
}

TrxW TrxW::create(TrxR trx_r)
{
	return {
		.status = TrxW_Status::Open,
		.trx_r = trx_r,
		.diffs = DiffBuffer::create(trx_r.db_ref->file_size(), trx_r.db_config()),
	};
}

DbConfig const* TrxW::db_config() const
{
	return &trx_r.db_ref->config;
}

void TrxW::close()
{
	if (status != TrxW_Status::Open)
		return;

	status = TrxW_Status::Closed;

	// base root is still pinned by our thread slot, so it can't be reclaimed here
	auto new_root = trx_r.db_ref->roots_pool.alloc_root(&trx_r.db_ref->thread_map);
	auto base_map = &trx_r.root->paged_buffer.map;

	// merge: copy into the diff every page that the base root has loaded
	// and that this transaction did not modify
	for (uint64_t i = 0; i < diffs.map.flags.count; i++)
	{
		if (diffs.map.flags[i].raw() == PageFlag::modified)
			continue; // modified by this transaction, keep it

		if (base_map->flags[i].acquire() != PageFlag::modified)
			continue; // never modified, a reader will load it from the file

		auto base_page = base_map->refs[i];
		diffs.alloc_and_map_page(i, db_config());
		std::copy_n(base_page, page_size(), diffs.mut_pointer_of(i));
		*diffs.map.flags[i].raw_mut() = PageFlag::modified;
	}

	// persist commit to disk
	// TODO
	// implement disk-commit atomicity via checksum, without fsync
	diffs.flush_diffs_to_commit_file(trx_r.db_ref->last_commit_fd, trx_r.db_ref->last_commit_filename, db_config());
	if (trx_r.db_ref->is_youngest_trx(trx_r.root->epoch_stamp))
		diffs.apply_to_master_file(trx_r.db_ref->fd, db_config());

	// deliberately not dropping diffbuffer
	auto new_epoch = trx_r.db_ref->epoch.increment();
	*new_root = Root::consume(new_epoch, diffs);
	trx_r.db_ref->root.swap_seq_cst(new_root);

	// release the base root only after publishing
	trx_r.close();
}

