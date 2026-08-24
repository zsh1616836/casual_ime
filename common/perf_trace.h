#pragma once

#include <cstdint>

#ifdef ZIME_PERF_DIAGNOSTIC

std::int64_t zime_perf_now();
std::int64_t zime_perf_elapsed_us(std::int64_t started);
void zime_perf_record(const char* event,
                      std::int64_t value1 = 0,
                      std::int64_t value2 = 0,
                      std::int64_t value3 = 0);
void zime_perf_flush(const wchar_t* role);

class zime_perf_scope
{
public:
    explicit zime_perf_scope(const char* event,
                             std::int64_t value1 = 0,
                             std::int64_t value2 = 0)
        : event_(event), value1_(value1), value2_(value2), started_(zime_perf_now())
    {
    }

    ~zime_perf_scope()
    {
        zime_perf_record(event_,
                         zime_perf_elapsed_us(started_),
                         value1_,
                         value2_);
    }

private:
    const char* event_;
    std::int64_t value1_;
    std::int64_t value2_;
    std::int64_t started_;
};

#define ZIME_PERF_SCOPE(name, value1, value2) \
    zime_perf_scope zime_perf_scope_##__LINE__(name, value1, value2)
#define ZIME_PERF_RECORD(name, value1, value2, value3) \
    zime_perf_record(name, value1, value2, value3)
#define ZIME_PERF_FLUSH(role) zime_perf_flush(role)

#else

#define ZIME_PERF_SCOPE(...) ((void)0)
#define ZIME_PERF_RECORD(...) ((void)0)
#define ZIME_PERF_FLUSH(...) ((void)0)

#endif
