#include "sqlite_tool.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
std::string path_to_utf8(const std::filesystem::path& path)
{
#if defined(_WIN32)
	const std::wstring wide = path.native();
	if (wide.empty())
		return std::string();

	const int size_needed = WideCharToMultiByte(
		CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (size_needed <= 0)
		return std::string();

	std::string utf8(size_needed, '\0');
	const int written = WideCharToMultiByte(
		CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), size_needed, nullptr, nullptr);
	if (written != size_needed)
		return std::string();

	return utf8;
#else
	return path.string();
#endif
}
}

sqlite_tool::sqlite_tool() : db_(nullptr), opened_(false)
{
}

sqlite_tool::~sqlite_tool()
{
	if (opened_)
		close();
}

int sqlite_tool::open(const std::filesystem::path& file)
{
	if (opened_)
		return SQLITE_MISUSE;
	const std::string file_utf8 = path_to_utf8(file);
	if (file_utf8.empty() && !file.empty())
		return SQLITE_CANTOPEN;
	const int rc = sqlite3_open(file_utf8.c_str(), &db_);
	if (rc == SQLITE_OK)
		opened_ = true;
	return rc;
}

int sqlite_tool::open(const std::string& name)
{
	if (opened_)
		return SQLITE_MISUSE;
	const int rc = sqlite3_open(name.c_str(), &db_);
	if (rc == SQLITE_OK)
		opened_ = true;
	return rc;
}

const char* sqlite_tool::last_error() const
{
	if (!db_)
		return "sqlite db is null";
	return sqlite3_errmsg(db_);
}

int sqlite_tool::exec(const std::string& sql) const
{
	if (!opened_ || !db_)
		return SQLITE_MISUSE;
	return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
}

int sqlite_tool::prepare(const std::string& sql, sqlite3_stmt** stmt) const
{
	if (!opened_ || !db_ || !stmt)
		return SQLITE_MISUSE;
	return sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), stmt, nullptr);
}

int sqlite_tool::step(sqlite3_stmt* stmt)
{
	if (!stmt)
		return SQLITE_MISUSE;
	return sqlite3_step(stmt);
}

int sqlite_tool::finalize(sqlite3_stmt* stmt)
{
	if (!stmt)
		return SQLITE_MISUSE;
	return sqlite3_finalize(stmt);
}

int sqlite_tool::close()
{
	if (!opened_ || !db_)
		return SQLITE_OK;
	const int rc = sqlite3_close(db_);
	if (rc == SQLITE_OK)
	{
		db_ = nullptr;
		opened_ = false;
	}
	return rc;
}
