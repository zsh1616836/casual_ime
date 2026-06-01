#ifndef UNICODE_READER_H
#define UNICODE_READER_H

#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

class unicode_reader
{
public:
	bool open(const wchar_t* filename);

	void read_lines(std::vector<std::string>& lines) const;

	~unicode_reader();

private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;

	HANDLE mapping_ = nullptr;

	const char* file_data_ = nullptr;

	size_t file_size_ = 0;
};

#endif
