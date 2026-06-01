#include "unicode_reader.h"

bool unicode_reader::open(const wchar_t* filename)
{
	handle_ = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ,
	                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle_ == INVALID_HANDLE_VALUE) return false;

	mapping_ = CreateFileMappingW(handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
	if (!mapping_)
	{
		CloseHandle(handle_);
		return false;
	}

	file_data_ = static_cast<const char*>(
		MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
	if (!file_data_)
	{
		CloseHandle(mapping_);
		CloseHandle(handle_);
		return false;
	}

	LARGE_INTEGER size;
	GetFileSizeEx(handle_, &size);
	file_size_ = size.QuadPart;

	return true;
}

void unicode_reader::read_lines(std::vector<std::string>& lines) const
{
	if (!file_data_) return;

	const char* lineStart = file_data_;
	const char* end = file_data_ + file_size_;

	for (const char* ptr = file_data_; ptr < end; ++ptr)
	{
		if (*ptr == '\n')
		{
			const char* lineEnd = ptr;
			if (ptr > lineStart && *(ptr - 1) == '\r')
			{
				lineEnd = ptr - 1;
			}

			lines.emplace_back(lineStart, lineEnd - lineStart);
			lineStart = ptr + 1;
		}
	}

	if (lineStart < end)
	{
		lines.emplace_back(lineStart, end - lineStart);
	}
}

unicode_reader::~unicode_reader()
{
	if (file_data_)
		UnmapViewOfFile(file_data_);
	if (mapping_)
		CloseHandle(mapping_);
	if (handle_ != INVALID_HANDLE_VALUE)
		CloseHandle(handle_);
}
