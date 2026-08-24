#ifndef SQLITE_UTILS_H
#define SQLITE_UTILS_H

#include <filesystem>
#include <string>

#include "sqlite3.h"

class sqlite_tool
{
public:
	sqlite_tool();
	~sqlite_tool();

	sqlite_tool(const sqlite_tool&) = delete;
	sqlite_tool& operator=(const sqlite_tool&) = delete;

	int open(const std::filesystem::path& file);
	int open(const std::filesystem::path& file, int flags);
	int open(const std::string& file);
	int close();

	[[nodiscard]] bool is_open() const { return opened_; }
	[[nodiscard]] sqlite3* handle() const { return db_; }
	[[nodiscard]] const char* last_error() const;

	int exec(const std::string& sql) const;
	int prepare(const std::string& sql, sqlite3_stmt** stmt) const;
	static int step(sqlite3_stmt* stmt);
	static int finalize(sqlite3_stmt* stmt);

private:
	sqlite3* db_;
	bool opened_;
};

#endif
