#include "perf_trace.h"

#ifdef ZIME_PERF_DIAGNOSTIC

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr std::size_t kRecordCapacity = 32768;

struct perf_record
{
    std::atomic<std::uint64_t> committed_sequence{UINT64_MAX};
    std::int64_t qpc;
    DWORD thread_id;
    const char* event;
    std::int64_t value1;
    std::int64_t value2;
    std::int64_t value3;
};

struct perf_record_snapshot
{
    std::uint64_t sequence;
    std::int64_t qpc;
    DWORD thread_id;
    const char* event;
    std::int64_t value1;
    std::int64_t value2;
    std::int64_t value3;
};

std::array<perf_record, kRecordCapacity> g_records = {};
std::atomic<std::uint64_t> g_next_sequence{0};

std::int64_t performance_frequency()
{
    static const std::int64_t frequency = []
    {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

std::wstring process_name()
{
    wchar_t path[32768] = {};
    const DWORD length = GetModuleFileNameW(
        nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
        return L"unknown";
    const wchar_t* name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

std::wstring output_path(const wchar_t* role)
{
    wchar_t temp[32768] = {};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(std::size(temp)), temp);
    std::wstring path = length > 0 && length < std::size(temp)
        ? std::wstring(temp, length)
        : std::wstring(L"C:\\Windows\\Temp\\");
    wchar_t file_name[512] = {};
    swprintf_s(file_name,
               L"zime_perf_%s_%s_%lu.csv",
               role ? role : L"unknown",
               process_name().c_str(),
               GetCurrentProcessId());
    path += file_name;
    return path;
}
}

std::int64_t zime_perf_now()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t zime_perf_elapsed_us(std::int64_t started)
{
    const std::int64_t elapsed = zime_perf_now() - started;
    const std::int64_t frequency = performance_frequency();
    return frequency > 0 ? elapsed * 1000000 / frequency : 0;
}

void zime_perf_record(const char* event,
                      std::int64_t value1,
                      std::int64_t value2,
                      std::int64_t value3)
{
    const std::uint64_t sequence = g_next_sequence.fetch_add(
        1, std::memory_order_relaxed);
    perf_record& record = g_records[sequence % kRecordCapacity];
    record.committed_sequence.store(UINT64_MAX, std::memory_order_relaxed);
    record.qpc = zime_perf_now();
    record.thread_id = GetCurrentThreadId();
    record.event = event;
    record.value1 = value1;
    record.value2 = value2;
    record.value3 = value3;
    record.committed_sequence.store(sequence, std::memory_order_release);
}

void zime_perf_flush(const wchar_t* role)
{
    const std::uint64_t end = g_next_sequence.load(std::memory_order_acquire);
    const std::uint64_t begin = end > kRecordCapacity
        ? end - kRecordCapacity
        : 0;

    std::vector<perf_record_snapshot> records;
    records.reserve(static_cast<std::size_t>(end - begin));
    for (std::uint64_t sequence = begin; sequence < end; ++sequence)
    {
        const perf_record& source = g_records[sequence % kRecordCapacity];
        if (source.committed_sequence.load(std::memory_order_acquire) != sequence ||
            !source.event)
        {
            continue;
        }
        records.push_back({sequence,
                           source.qpc,
                           source.thread_id,
                           source.event,
                           source.value1,
                           source.value2,
                           source.value3});
    }
    std::sort(records.begin(), records.end(),
              [](const perf_record_snapshot& left,
                 const perf_record_snapshot& right)
              {
                  return left.sequence < right.sequence;
              });

    std::string output;
    output.reserve(records.size() * 80 + 128);
    char line[512] = {};
    const int header_length = snprintf(
        line,
        sizeof(line),
        "frequency,%lld\r\nsequence,qpc,thread,event,value1,value2,value3\r\n",
        static_cast<long long>(performance_frequency()));
    if (header_length > 0)
        output.append(line, static_cast<std::size_t>(header_length));
    for (const perf_record_snapshot& record : records)
    {
        const int length = snprintf(
            line,
            sizeof(line),
            "%llu,%lld,%lu,%s,%lld,%lld,%lld\r\n",
            static_cast<unsigned long long>(record.sequence),
            static_cast<long long>(record.qpc),
            record.thread_id,
            record.event,
            static_cast<long long>(record.value1),
            static_cast<long long>(record.value2),
            static_cast<long long>(record.value3));
        if (length > 0)
            output.append(line, static_cast<std::size_t>(length));
    }

    const std::wstring path = output_path(role);
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    if (!output.empty())
    {
        WriteFile(file,
                  output.data(),
                  static_cast<DWORD>(output.size()),
                  &written,
                  nullptr);
    }
    CloseHandle(file);
}

#endif
