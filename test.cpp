// Alice in Mediocreland (2020)

#include <cstdint>
#include <atomic>
#include <vector>
#include <charconv>
#include <iostream>

#include <windows.h>

struct allocator_t
{
	void* (*allocate)(size_t size);
	void (*deallocate)(void* memory);
	const char* name;
};

bool acquire_lock_memory_privilege();
double test(const allocator_t& allocator, const std::vector<uint64_t>& offset_array, size_t memory_size, uint32_t sample_pass_num);

void* allocate_large_pages(size_t size);
void deallocate_large_pages(void* memory);

constexpr allocator_t MALLOC_ALLOCATOR = {
	malloc,
	free,
	"default allocator"
};

constexpr allocator_t LARGE_PAGE_ALLOCATOR = {
	allocate_large_pages,
	deallocate_large_pages,
	"large page allocator"
};

template<typename T>
void read_arg(const char* arg, const char* format, T& value)
{
	if (strstr(arg, format) != arg)
		return;

	const char* arg_value = arg + strlen(format);
	const char* arg_value_end = arg_value + strlen(arg_value);
	std::from_chars(arg_value, arg_value_end, value);
}

void fill_offset_array(bool use_random_offsets, std::vector<uint64_t>& offsets)
{
	if (use_random_offsets)
	{
		for (size_t i = 0; i < offsets.size(); i++)
			offsets[i] = (i + rand()) % offsets.size();
	}
	else
	{
		SYSTEM_INFO system_info = {};
		GetSystemInfo(&system_info);

		const size_t stride = system_info.dwPageSize / sizeof(uint64_t);

		for (size_t i = 0; i < offsets.size(); i++)
			offsets[i] = ((i * stride) % offsets.size()) + (rand() % stride);
	}
}

int main(int argc, const char* argv[])
{
	if (!acquire_lock_memory_privilege())
	{
		std::cout << "failed to acquire SeLockMemoryPrivilege" << std::endl;
		return 1;
	}

	SYSTEM_INFO system_info = {};
	GetSystemInfo(&system_info);

	const size_t default_page_size = system_info.dwPageSize;
	const size_t large_page_size = GetLargePageMinimum();
	std::cout << "default page size: " << default_page_size << "B" << std::endl;
	std::cout << "large page size: " << large_page_size << "B" << std::endl;

	size_t memory_size = 256 * 1024 * 1024;
	uint32_t sample_num = 100;
	uint32_t sample_pass_num = 1;
	uint32_t use_random_offsets = 0;

	for (int i = 1; i < argc; i++)
	{
		read_arg(argv[i], "--size=", memory_size);
		read_arg(argv[i], "--sample_num=", sample_num);
		read_arg(argv[i], "--sample_pass_num=", sample_num);
		read_arg(argv[i], "--use_random_offsets=", use_random_offsets);
	}

	memory_size = ((memory_size + large_page_size - 1) / large_page_size) * large_page_size;
	std::cout << "test memory size: " << memory_size << "B" << std::endl;

	std::vector<uint64_t> offset_array(memory_size / sizeof(uint64_t));
	fill_offset_array(use_random_offsets, offset_array);

	double malloc_allocator_time = 0.0;
	double large_page_allocator_time = 0.0;

	for (uint32_t i = 0; i < sample_num; i++)
		large_page_allocator_time += test(LARGE_PAGE_ALLOCATOR, offset_array, memory_size, sample_pass_num);

	for (uint32_t i = 0; i < sample_num; i++)
		malloc_allocator_time += test(MALLOC_ALLOCATOR, offset_array, memory_size, sample_pass_num);

	std::cout << std::endl;
	std::cout << std::endl;

	std::cout << "malloc time: " << malloc_allocator_time << "s (avg: " << malloc_allocator_time / sample_num << "s)" << std::endl;
	std::cout << "large page time: " << large_page_allocator_time << "s (avg: " << large_page_allocator_time / sample_num << "s)" << std::endl;

	const double delta = malloc_allocator_time - large_page_allocator_time;
	const double delta_percentage = floor((delta / malloc_allocator_time) * 10000.0) * 0.01;
	std::cout << "(m - l) / m = " << delta_percentage << "%" << std::endl;
	std::cout << "\tm: malloc allocator" << std::endl;
	std::cout << "\tl: large page allocator" << std::endl;

	return 0;
}

double test(const allocator_t& allocator, const std::vector<uint64_t>& offset_array, size_t memory_size, uint32_t sample_pass_num)
{
	uint64_t* items = (uint64_t*)allocator.allocate(memory_size);

	if (items == nullptr)
	{
		std::cout << "failed to allocate " << memory_size << "B using " << allocator.name << std::endl;
		return 0.0;
	}

	const size_t item_num = memory_size / sizeof(uint64_t);

	memcpy(items, offset_array.data(), item_num * sizeof(uint64_t));

	LARGE_INTEGER begin;
	QueryPerformanceCounter(&begin);

	std::atomic_thread_fence(std::memory_order_acquire);

	uint64_t value = 0;
	for (volatile size_t i = 0; i < sample_pass_num; i++)
	{
		for (size_t j = 0; j < item_num; j++)
		{
			const uint64_t b = items[j];
			const uint64_t c = items[b];
			value += b * c;
		}
	}

	std::atomic_thread_fence(std::memory_order_release);

	LARGE_INTEGER end;
	QueryPerformanceCounter(&end);

	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);

	const double inv_frequency = 1.0 / double(frequency.QuadPart);
	const double delta = (end.QuadPart - begin.QuadPart) * inv_frequency;

	allocator.deallocate(items);

	volatile uint64_t result = value;

	return delta;
}

bool acquire_lock_memory_privilege()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return false;

	TOKEN_PRIVILEGES token_privileges = {};
	if (!LookupPrivilegeValueW(NULL, L"SeLockMemoryPrivilege", &token_privileges.Privileges[0].Luid))
	{
		CloseHandle(token);
		return false;
	}

	token_privileges.PrivilegeCount = 1;
	token_privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(token, FALSE, &token_privileges, 0, (PTOKEN_PRIVILEGES)nullptr, 0))
	{
		CloseHandle(token);
		return false;
	}

	const DWORD error = GetLastError();

	if (!CloseHandle(token))
		return false;

	return error == ERROR_SUCCESS;
}

void* allocate_large_pages(size_t size)
{
	constexpr DWORD flags = MEM_LARGE_PAGES | MEM_RESERVE | MEM_COMMIT;
	constexpr DWORD access = PAGE_READWRITE;

	return VirtualAlloc(nullptr, size, flags, PAGE_READWRITE);
}

void deallocate_large_pages(void* memory)
{
	VirtualFree(memory, 0, MEM_RELEASE);
}