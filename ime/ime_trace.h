#pragma once

#include <windows.h>

#ifndef NDEBUG
void ime_tracef(const wchar_t* event, const wchar_t* fmt, ...);
#else
#define ime_tracef(...) ((void)0)
#endif

void ime_errorf(const wchar_t* event, const wchar_t* fmt, ...);
