#pragma once

#ifndef NDEBUG
void broker_debug_log(const wchar_t* format, ...);
#else
#define broker_debug_log(...) ((void)0)
#endif

void broker_error_log(const wchar_t* format, ...);
