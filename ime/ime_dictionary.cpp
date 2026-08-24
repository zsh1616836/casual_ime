#include "ime_dictionary.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <cwctype>
#include <ctime>
#include <limits>

#include "3rd/sqlite_tool.h"
#include "tool.h"

namespace
{
constexpr uint32_t kDictMagic = 0x44494458;  // "DIDX"
constexpr uint32_t kMaxWubiCodeCount = 200000;
constexpr uint32_t kMaxCodeEntryCount = 2000000;
constexpr uint16_t kMaxWubiCodeLength = 128;
constexpr uint16_t kMaxCodeLength = 64;

int bind_text(sqlite3_stmt* stmt, int index, const std::string& value)
{
	return sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

bool run_sqlite_stmt(sqlite3_stmt* stmt)
{
	if (!stmt)
		return false;
	const int rc = sqlite_tool::step(stmt);
	return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

bool open_overlay_db(const std::filesystem::path& path, sqlite_tool& db, std::string& error)
{
	std::error_code ec;
	if (!path.parent_path().empty())
		std::filesystem::create_directories(path.parent_path(), ec);

	const int open_rc = db.open(path);
	if (open_rc != SQLITE_OK)
	{
		error = "Cannot open user_dict.db: ";
		error += db.last_error();
		return false;
	}

	if (db.exec("PRAGMA journal_mode=WAL;") != SQLITE_OK ||
		db.exec("PRAGMA synchronous=NORMAL;") != SQLITE_OK)
	{
		error = "Cannot set sqlite pragmas: ";
		error += db.last_error();
		return false;
	}

	const char* schema_sql =
		"CREATE TABLE IF NOT EXISTS user_words("
		"code TEXT NOT NULL,"
		"candidate TEXT NOT NULL,"
		"wubi_code TEXT NOT NULL DEFAULT '',"
		"uncommon INTEGER NOT NULL DEFAULT 0,"
		"PRIMARY KEY(code, candidate)"
		");"
		"CREATE TABLE IF NOT EXISTS blocked_words("
		"code TEXT NOT NULL,"
		"candidate TEXT NOT NULL,"
		"PRIMARY KEY(code, candidate)"
		");"
		"CREATE TABLE IF NOT EXISTS uncommon_words("
		"code TEXT NOT NULL,"
		"candidate TEXT NOT NULL,"
		"PRIMARY KEY(code, candidate)"
		");"
		"CREATE TABLE IF NOT EXISTS candidate_stats("
		"code TEXT NOT NULL,"
		"candidate TEXT NOT NULL,"
		"use_count INTEGER NOT NULL DEFAULT 0,"
		"last_used_at INTEGER NOT NULL DEFAULT 0,"
		"PRIMARY KEY(code, candidate)"
		");";
	if (db.exec(schema_sql) != SQLITE_OK)
	{
		error = "Cannot initialize user_dict.db schema: ";
		error += db.last_error();
		return false;
	}
	return true;
}

bool open_overlay_db_read_only(const std::filesystem::path& path,
	                           sqlite_tool& db)
{
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) || error)
		return false;
	return db.open(path, SQLITE_OPEN_READONLY) == SQLITE_OK;
}

bool write_utf8_lines_atomic(const std::filesystem::path& path,
                             const std::vector<std::string>& lines,
                             bool write_bom,
                             std::string& error)
{
	std::error_code ec;
	if (!path.parent_path().empty())
	{
		std::filesystem::create_directories(path.parent_path(), ec);
	}

	std::filesystem::path temp_path = path;
	temp_path += L".tmp";

	std::ofstream fout(temp_path, std::ios::binary | std::ios::trunc);
	if (!fout)
	{
		const DWORD err = GetLastError();
		error = "Cannot open temp file for writing (Win32=" + std::to_string(err) + ")";
		return false;
	}

	if (write_bom)
	{
		const char bom[3] = { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) };
		fout.write(bom, 3);
	}
	for (size_t i = 0; i < lines.size(); ++i)
	{
		fout.write(lines[i].c_str(), static_cast<std::streamsize>(lines[i].size()));
		if (i + 1 < lines.size())
			fout.write("\n", 1);
	}
	fout.flush();
	fout.close();

	if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD err = GetLastError();
		DeleteFileW(temp_path.c_str());
		error = "Cannot replace dictionary file atomically (Win32=" + std::to_string(err) + ")";
		return false;
	}
	return true;
}
}

Candidate::Candidate(std::wstring t, uint32_t code_idx, bool from_user, bool is_uncommon): text(std::move(t)), wubi_code_index(code_idx), user_defined(from_user), uncommon(is_uncommon)
{
}

bool Candidate::is_pinyin() const
{ return wubi_code_index != 0; }

ime_dict::ime_dict()
	: m_use_file_dict(false),
		  m_index_format_version(0),
		  m_dict_file_size(0),
		  m_show_uncommon_candidates(false),
		  m_candidate_sort_mode(candidate_sort_mode::frequency)
{
	// 预留索引0为"无编码"标记
	m_wubi_code_pool.push_back("");
}

ime_dict::~ime_dict()
{
}

bool ime_dict::init()
{
	// 仅允许加载新版索引词库；失败时直接返回false
	if (load_from_index("dict.idx"))
	{
		load_user_dict();
		load_blocked_dict();
		load_candidate_stats();
		m_use_file_dict = true;
		return true;
	}

	m_use_file_dict = false;
	OutputDebugStringA("ERROR: failed to load dict.idx from IME DLL directory\n");
	return false;
}

void ime_dict::unload()
{
	m_dict_bytes.clear();
	m_code_index.clear();
	m_wubi_code_pool.clear();
	m_wubi_code_pool.push_back("");
	m_user_code_to_candidates.clear();
	m_blocked_candidates.clear();
	m_runtime_uncommon_candidates.clear();
	m_candidate_usage_stats.clear();
	m_use_file_dict = false;
	m_index_format_version = 0;
	m_dict_file_size = 0;
}

bool ime_dict::load_from_index(const char* filename)
{
	m_dict_bytes.clear();
	m_dict_file_size = 0;

	std::filesystem::path pwd = tool::get_current_dll_path();
	pwd = pwd / std::string(filename);
	
	std::ifstream fin(pwd, std::ios::binary);
	if (!fin)
		return false;

	fin.seekg(0, std::ios::end);
	const std::streamoff end_pos = fin.tellg();
	if (end_pos < static_cast<std::streamoff>(sizeof(uint32_t) * 5))
		return false;
	const uint64_t file_size = static_cast<uint64_t>(end_pos);
	fin.seekg(0, std::ios::beg);

	auto read_bytes = [&fin](void* dst, size_t sz) -> bool {
		if (sz == 0)
			return true;
		return static_cast<bool>(fin.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(sz)));
	};
	
	// 读取Header
	uint32_t magic, version, wubi_code_count, code_entry_count;
	uint32_t data_section_offset;
	
	if (!read_bytes(&magic, sizeof(magic)))
		return false;
	if (magic != kDictMagic)
		return false;

	if (!read_bytes(&version, sizeof(version)))
		return false;
	if (version != 2 && version != 3 && version != 4)
		return false;
	m_index_format_version = version;

	if (!read_bytes(&wubi_code_count, sizeof(wubi_code_count)) ||
	    !read_bytes(&code_entry_count, sizeof(code_entry_count)) ||
	    !read_bytes(&data_section_offset, sizeof(data_section_offset)))
		return false;
	if (wubi_code_count == 0 || wubi_code_count > kMaxWubiCodeCount)
		return false;
	if (code_entry_count > kMaxCodeEntryCount)
		return false;
	if (data_section_offset > file_size)
		return false;

	// 读取Wubi Code Pool
	m_wubi_code_pool.clear();
	m_wubi_code_pool.reserve(wubi_code_count);
	
	for (uint32_t i = 0; i < wubi_code_count; ++i)
	{
		uint16_t len;
		if (!read_bytes(&len, sizeof(len)))
			return false;
		if (len > kMaxWubiCodeLength)
			return false;
		
		if (len > 0)
		{
			std::string code(len, '\0');
			if (!read_bytes(&code[0], len))
				return false;
			m_wubi_code_pool.push_back(std::move(code));
		}
		else
		{
			m_wubi_code_pool.push_back("");
		}
	}

	// 读取Code Index Section
	m_code_index.clear();
	
	for (uint32_t i = 0; i < code_entry_count; ++i)
	{
		// 读取编码长度和字符串
		uint16_t code_len;
		if (!read_bytes(&code_len, sizeof(code_len)))
			return false;
		if (code_len == 0 || code_len > kMaxCodeLength)
			return false;
		
		std::string code(code_len, '\0');
		if (!read_bytes(&code[0], code_len))
			return false;
		
		CodeIndexEntry entry;
		if (!read_bytes(&entry.file_offset, sizeof(entry.file_offset)) ||
		    !read_bytes(&entry.candidate_count, sizeof(entry.candidate_count)))
			return false;
		if (entry.file_offset > file_size)
			return false;
		if (entry.candidate_count > 0)
		{
			if (entry.file_offset < data_section_offset)
				return false;
			const uint64_t min_candidate_size = (m_index_format_version >= 4) ? 9ULL :
			                                    ((m_index_format_version >= 3) ? 8ULL : 6ULL);
			const uint64_t min_required = static_cast<uint64_t>(entry.candidate_count) * min_candidate_size;
			if (min_required > file_size - entry.file_offset)
				return false;
		}
		if (!m_code_index.emplace(code, entry).second)
			return false;
	}

	if (file_size > (std::numeric_limits<std::size_t>::max)() ||
		file_size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
	{
		return false;
	}
	m_dict_bytes.resize(static_cast<std::size_t>(file_size));
	fin.clear();
	fin.seekg(0, std::ios::beg);
	if (!fin.read(reinterpret_cast<char*>(m_dict_bytes.data()),
		static_cast<std::streamsize>(m_dict_bytes.size())))
	{
		m_dict_bytes.clear();
		return false;
	}
	m_dict_file_size = file_size;

	return true;
}

bool ime_dict::add_custom_word(const std::wstring& word, const std::string& code, std::string& error)
{
	if (word.empty() || code.empty())
	{
		error = "Word or code is empty";
		return false;
	}
	const std::string word_utf8 = wstring_to_utf8(word);

	sqlite_tool db;
	if (!open_overlay_db(get_user_db_path(), db, error))
		return false;

	if (db.exec("BEGIN IMMEDIATE;") != SQLITE_OK)
	{
		error = "Cannot start sqlite transaction: ";
		error += db.last_error();
		return false;
	}

	sqlite3_stmt* upsert_stmt = nullptr;
	if (db.prepare("INSERT INTO user_words(code, candidate, wubi_code, uncommon) VALUES(?, ?, '', 0) "
		"ON CONFLICT(code, candidate) DO UPDATE SET wubi_code=excluded.wubi_code;", &upsert_stmt) != SQLITE_OK)
	{
		error = "Cannot prepare user_words upsert: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}
	bind_text(upsert_stmt, 1, code);
	bind_text(upsert_stmt, 2, word_utf8);
	if (!run_sqlite_stmt(upsert_stmt))
	{
		error = "Cannot insert user word: ";
		error += db.last_error();
		sqlite_tool::finalize(upsert_stmt);
		db.exec("ROLLBACK;");
		return false;
	}
	sqlite_tool::finalize(upsert_stmt);

	sqlite3_stmt* delete_blocked_stmt = nullptr;
	if (db.prepare("DELETE FROM blocked_words WHERE code=? AND candidate=?;", &delete_blocked_stmt) == SQLITE_OK)
	{
		bind_text(delete_blocked_stmt, 1, code);
		bind_text(delete_blocked_stmt, 2, word_utf8);
		run_sqlite_stmt(delete_blocked_stmt);
		sqlite_tool::finalize(delete_blocked_stmt);
	}

	if (db.exec("COMMIT;") != SQLITE_OK)
	{
		error = "Cannot commit sqlite transaction: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}

	load_user_dict();
	load_blocked_dict();
	return true;
}

std::filesystem::path ime_dict::get_user_db_path() const
{
	return tool::get_user_data_path() / L"user_dict.db";
}

std::string ime_dict::make_candidate_stat_key(const std::string& code, const std::string& candidate_utf8)
{
	std::string key;
	key.reserve(code.size() + 1 + candidate_utf8.size());
	key.append(code);
	key.push_back('\t');
	key.append(candidate_utf8);
	return key;
}

bool ime_dict::load_user_dict()
{
	m_user_code_to_candidates.clear();
	const std::filesystem::path path = get_user_db_path();
	std::error_code file_error;
	if (!std::filesystem::exists(path, file_error))
		return true;
	sqlite_tool db;
	if (!open_overlay_db_read_only(path, db))
		return false;

	sqlite3_stmt* stmt = nullptr;
	if (db.prepare("SELECT code, candidate, wubi_code, uncommon FROM user_words;", &stmt) != SQLITE_OK)
		return false;
	while (sqlite_tool::step(stmt) == SQLITE_ROW)
	{
		const char* code_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		const char* cand_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
		const char* wubi_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
		const bool uncommon = sqlite3_column_int(stmt, 3) != 0;
		if (!code_ptr || !cand_ptr)
			continue;

		const std::string code(code_ptr);
		const std::string cand_utf8(cand_ptr);
		const std::string wubi = wubi_ptr ? std::string(wubi_ptr) : std::string();
		const std::wstring text = utf8_to_wstring(cand_utf8);
		const uint32_t wubi_index = wubi.empty() ? 0 : get_or_create_wubi_code_index(wubi);
		m_user_code_to_candidates[code].emplace_back(text, wubi_index, true, uncommon);
	}
	sqlite_tool::finalize(stmt);
	return true;
}

bool ime_dict::load_blocked_dict()
{
	m_blocked_candidates.clear();
	m_runtime_uncommon_candidates.clear();
	const std::filesystem::path path = get_user_db_path();
	std::error_code file_error;
	if (!std::filesystem::exists(path, file_error))
		return true;
	sqlite_tool db;
	if (!open_overlay_db_read_only(path, db))
		return false;

	sqlite3_stmt* blocked_stmt = nullptr;
	if (db.prepare("SELECT code, candidate FROM blocked_words;", &blocked_stmt) == SQLITE_OK)
	{
		while (sqlite_tool::step(blocked_stmt) == SQLITE_ROW)
		{
			const char* code_ptr = reinterpret_cast<const char*>(sqlite3_column_text(blocked_stmt, 0));
			const char* cand_ptr = reinterpret_cast<const char*>(sqlite3_column_text(blocked_stmt, 1));
			if (!code_ptr || !cand_ptr)
				continue;
			m_blocked_candidates[std::string(code_ptr)].insert(utf8_to_wstring(std::string(cand_ptr)));
		}
		sqlite_tool::finalize(blocked_stmt);
	}

	sqlite3_stmt* uncommon_stmt = nullptr;
	if (db.prepare("SELECT code, candidate FROM uncommon_words;", &uncommon_stmt) == SQLITE_OK)
	{
		while (sqlite_tool::step(uncommon_stmt) == SQLITE_ROW)
		{
			const char* code_ptr = reinterpret_cast<const char*>(sqlite3_column_text(uncommon_stmt, 0));
			const char* cand_ptr = reinterpret_cast<const char*>(sqlite3_column_text(uncommon_stmt, 1));
			if (!code_ptr || !cand_ptr)
				continue;
			m_runtime_uncommon_candidates[std::string(code_ptr)].insert(utf8_to_wstring(std::string(cand_ptr)));
		}
		sqlite_tool::finalize(uncommon_stmt);
	}
	return true;
}

bool ime_dict::load_candidate_stats()
{
	m_candidate_usage_stats.clear();
	const std::filesystem::path path = get_user_db_path();
	std::error_code file_error;
	if (!std::filesystem::exists(path, file_error))
		return true;
	sqlite_tool db;
	if (!open_overlay_db_read_only(path, db))
		return false;

	sqlite3_stmt* stmt = nullptr;
	if (db.prepare("SELECT code, candidate, use_count, last_used_at FROM candidate_stats;", &stmt) != SQLITE_OK)
		return false;

	while (sqlite_tool::step(stmt) == SQLITE_ROW)
	{
		const char* code_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		const char* cand_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
		if (!code_ptr || !cand_ptr)
			continue;

		CandidateUsageStat stat;
		stat.use_count = static_cast<int64_t>(sqlite3_column_int64(stmt, 2));
		stat.last_used_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 3));
		if (stat.use_count < 0)
			stat.use_count = 0;
		if (stat.last_used_at < 0)
			stat.last_used_at = 0;

		m_candidate_usage_stats[make_candidate_stat_key(code_ptr, cand_ptr)] = stat;
	}
	sqlite_tool::finalize(stmt);
	return true;
}

bool ime_dict::delete_candidate(const std::wstring& code, const std::wstring& candidate, std::string& error)
{
	if (code.empty() || candidate.empty())
	{
		error = "Code or candidate is empty";
		return false;
	}

	const std::string narrow_code = wstring_to_utf8(code);
	const std::string cand_utf8 = wstring_to_utf8(candidate);

	sqlite_tool db;
	if (!open_overlay_db(get_user_db_path(), db, error))
		return false;
	if (db.exec("BEGIN IMMEDIATE;") != SQLITE_OK)
	{
		error = "Cannot start sqlite transaction: ";
		error += db.last_error();
		return false;
	}

	sqlite3_stmt* delete_user_stmt = nullptr;
	if (db.prepare("DELETE FROM user_words WHERE code=? AND candidate=?;", &delete_user_stmt) != SQLITE_OK)
	{
		error = "Cannot prepare delete user_words: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}
	bind_text(delete_user_stmt, 1, narrow_code);
	bind_text(delete_user_stmt, 2, cand_utf8);
	if (!run_sqlite_stmt(delete_user_stmt))
	{
		error = "Cannot delete from user_words: ";
		error += db.last_error();
		sqlite_tool::finalize(delete_user_stmt);
		db.exec("ROLLBACK;");
		return false;
	}
	sqlite_tool::finalize(delete_user_stmt);

	sqlite3_stmt* insert_blocked_stmt = nullptr;
	if (db.prepare("INSERT OR IGNORE INTO blocked_words(code, candidate) VALUES(?, ?);", &insert_blocked_stmt) != SQLITE_OK)
	{
		error = "Cannot prepare insert blocked_words: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}
	bind_text(insert_blocked_stmt, 1, narrow_code);
	bind_text(insert_blocked_stmt, 2, cand_utf8);
	if (!run_sqlite_stmt(insert_blocked_stmt))
	{
		error = "Cannot insert into blocked_words: ";
		error += db.last_error();
		sqlite_tool::finalize(insert_blocked_stmt);
		db.exec("ROLLBACK;");
		return false;
	}
	sqlite_tool::finalize(insert_blocked_stmt);

	if (db.exec("COMMIT;") != SQLITE_OK)
	{
		error = "Cannot commit sqlite transaction: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}

	load_user_dict();
	load_blocked_dict();
	return true;
}

bool ime_dict::mark_candidate_uncommon(const std::wstring& code, const std::wstring& candidate, std::string& error)
{
	if (code.empty() || candidate.empty())
	{
		error = "Code or candidate is empty";
		return false;
	}

	const std::string narrow_code = wstring_to_utf8(code);
	const std::string cand_utf8 = wstring_to_utf8(candidate);

	sqlite_tool db;
	if (!open_overlay_db(get_user_db_path(), db, error))
		return false;

	if (db.exec("BEGIN IMMEDIATE;") != SQLITE_OK)
	{
		error = "Cannot start sqlite transaction: ";
		error += db.last_error();
		return false;
	}

	sqlite3_stmt* insert_uncommon_stmt = nullptr;
	if (db.prepare("INSERT OR IGNORE INTO uncommon_words(code, candidate) VALUES(?, ?);", &insert_uncommon_stmt) != SQLITE_OK)
	{
		error = "Cannot prepare insert uncommon_words: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}
	bind_text(insert_uncommon_stmt, 1, narrow_code);
	bind_text(insert_uncommon_stmt, 2, cand_utf8);
	if (!run_sqlite_stmt(insert_uncommon_stmt))
	{
		error = "Cannot insert into uncommon_words: ";
		error += db.last_error();
		sqlite_tool::finalize(insert_uncommon_stmt);
		db.exec("ROLLBACK;");
		return false;
	}
	sqlite_tool::finalize(insert_uncommon_stmt);

	sqlite3_stmt* update_user_uncommon_stmt = nullptr;
	if (db.prepare("UPDATE user_words SET uncommon=1 WHERE code=? AND candidate=?;", &update_user_uncommon_stmt) == SQLITE_OK)
	{
		bind_text(update_user_uncommon_stmt, 1, narrow_code);
		bind_text(update_user_uncommon_stmt, 2, cand_utf8);
		run_sqlite_stmt(update_user_uncommon_stmt);
		sqlite_tool::finalize(update_user_uncommon_stmt);
	}

	if (db.exec("COMMIT;") != SQLITE_OK)
	{
		error = "Cannot commit sqlite transaction: ";
		error += db.last_error();
		db.exec("ROLLBACK;");
		return false;
	}

	load_user_dict();
	load_blocked_dict();
	return true;
}

bool ime_dict::record_candidate_selected(const std::wstring& code,
	                                     const std::wstring& candidate,
	                                     std::string* error_out)
{
	if (code.empty() || candidate.empty())
	{
		if (error_out)
			*error_out = "Code or candidate is empty";
		return false;
	}

	const std::string narrow_code = wstring_to_utf8(code);
	const std::string cand_utf8 = wstring_to_utf8(candidate);
	if (narrow_code.empty() || cand_utf8.empty())
	{
		if (error_out)
			*error_out = "Cannot encode code or candidate";
		return false;
	}

	const int64_t now = static_cast<int64_t>(std::time(nullptr));

	std::string error;
	sqlite_tool db;
	if (!open_overlay_db(get_user_db_path(), db, error))
	{
		if (error_out)
			*error_out = error;
		return false;
	}

	sqlite3_stmt* stmt = nullptr;
	if (db.prepare(
		"INSERT INTO candidate_stats(code, candidate, use_count, last_used_at) VALUES(?, ?, 1, ?) "
		"ON CONFLICT(code, candidate) DO UPDATE SET "
		"use_count = candidate_stats.use_count + 1, "
		"last_used_at = excluded.last_used_at;",
		&stmt) != SQLITE_OK)
	{
		if (error_out)
			*error_out = db.last_error();
		return false;
	}

	bind_text(stmt, 1, narrow_code);
	bind_text(stmt, 2, cand_utf8);
	sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(now));
	if (!run_sqlite_stmt(stmt))
	{
		if (error_out)
			*error_out = db.last_error();
		sqlite_tool::finalize(stmt);
		return false;
	}
	sqlite_tool::finalize(stmt);

	const std::string key = make_candidate_stat_key(narrow_code, cand_utf8);
	CandidateUsageStat& stat = m_candidate_usage_stats[key];
	if (stat.use_count < 0)
		stat.use_count = 0;
	stat.use_count += 1;
	stat.last_used_at = now;
	return true;
}

bool ime_dict::export_raw_dictionary(const std::filesystem::path& output_path, std::string& error)
{
	std::vector<std::string> lines;
	std::map<std::string, std::vector<Candidate>> export_entries;
	std::map<std::string, std::vector<Candidate>> base_entries;

	// 先按文件偏移顺序预取主词库候选，减少随机 seek。
	std::vector<std::pair<std::string, CodeIndexEntry>> by_offset;
	by_offset.reserve(m_code_index.size());
	for (const auto& kv : m_code_index)
	{
		by_offset.push_back(kv);
	}
	std::sort(by_offset.begin(), by_offset.end(),
		[](const auto& lhs, const auto& rhs) {
			return lhs.second.file_offset < rhs.second.file_offset;
		});
	for (const auto& kv : by_offset)
	{
		std::vector<Candidate> current;
		if (!read_candidates_from_file(kv.second.file_offset, kv.second.candidate_count, current))
		{
			error = "Failed to read dict.idx while exporting";
			return false;
		}
		base_entries.emplace(kv.first, std::move(current));
	}

	std::map<std::string, bool> all_codes;
	for (const auto& kv : m_code_index)
		all_codes[kv.first] = true;
	for (const auto& kv : m_user_code_to_candidates)
		all_codes[kv.first] = true;

	for (const auto& kv : all_codes)
	{
		const std::string& code = kv.first;
		std::vector<Candidate> merged;
		merged.reserve(32);

		auto user_it = m_user_code_to_candidates.find(code);
		if (user_it != m_user_code_to_candidates.end())
		{
			for (const auto& c : user_it->second)
			{
				merged.emplace_back(c.text, c.wubi_code_index, true, c.uncommon);
			}
		}

		auto base_it = base_entries.find(code);
		if (base_it != base_entries.end())
		{
			for (auto& c : base_it->second)
			{
				merged.push_back(c);
			}
		}

		auto blocked_it = m_blocked_candidates.find(code);
		if (blocked_it != m_blocked_candidates.end() && !blocked_it->second.empty())
		{
			merged.erase(
				std::remove_if(merged.begin(), merged.end(),
					[&blocked_it](const Candidate& c) {
						return blocked_it->second.find(c.text) != blocked_it->second.end();
					}),
				merged.end());
		}

		auto uncommon_it = m_runtime_uncommon_candidates.find(code);
		if (uncommon_it != m_runtime_uncommon_candidates.end() && !uncommon_it->second.empty())
		{
			for (auto& c : merged)
			{
				if (uncommon_it->second.find(c.text) != uncommon_it->second.end())
					c.uncommon = true;
			}
		}

		std::unordered_map<std::wstring, size_t> seen;
		std::vector<Candidate> dedup;
		dedup.reserve(merged.size());
		for (const auto& cand : merged)
		{
			auto sit = seen.find(cand.text);
			if (sit == seen.end())
			{
				seen.emplace(cand.text, dedup.size());
				dedup.push_back(cand);
				continue;
			}
			Candidate& existing = dedup[sit->second];
			if (!existing.user_defined && cand.user_defined)
			{
				const bool uncommon = existing.uncommon || cand.uncommon;
				existing = cand;
				existing.uncommon = uncommon;
			}
			else if (!existing.uncommon && cand.uncommon)
			{
				existing.uncommon = true;
			}
		}

		std::stable_sort(dedup.begin(), dedup.end(),
			[](const Candidate& lhs, const Candidate& rhs) {
				return lhs.user_defined && !rhs.user_defined;
			});

		if (dedup.empty())
			continue;

		std::string row = code;
		for (const auto& c : dedup)
		{
			std::string token = wstring_to_utf8(c.text);
			if (c.is_pinyin() && c.wubi_code_index < m_wubi_code_pool.size())
			{
				token += "(";
				token += m_wubi_code_pool[c.wubi_code_index];
				token += ")";
			}
			if (c.uncommon)
			{
				token += ")";
			}
			row += ",";
			row += token;
		}
		lines.push_back(std::move(row));
		export_entries[code] = dedup;
	}

	bool has_bom = true;
	if (!write_utf8_lines_atomic(output_path, lines, has_bom, error))
		return false;

	std::filesystem::path idx_path = output_path;
	idx_path.replace_extension(L".idx");
	std::filesystem::path idx_tmp = idx_path;
	idx_tmp += L".tmp";

	std::ofstream fout(idx_tmp, std::ios::binary | std::ios::trunc);
	if (!fout)
	{
		error = "Cannot create output idx temp file";
		return false;
	}

	uint32_t magic = 0x44494458;  // "DIDX"
	uint32_t version = 4;
	uint32_t wubi_code_count = static_cast<uint32_t>(m_wubi_code_pool.size());
	uint32_t code_entry_count = static_cast<uint32_t>(export_entries.size());
	uint32_t data_section_offset = 0;

	fout.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
	fout.write(reinterpret_cast<const char*>(&version), sizeof(version));
	fout.write(reinterpret_cast<const char*>(&wubi_code_count), sizeof(wubi_code_count));
	fout.write(reinterpret_cast<const char*>(&code_entry_count), sizeof(code_entry_count));
	size_t offset_pos = static_cast<size_t>(fout.tellp());
	fout.write(reinterpret_cast<const char*>(&data_section_offset), sizeof(data_section_offset));

	for (const auto& code : m_wubi_code_pool)
	{
		uint16_t len = static_cast<uint16_t>(code.length());
		fout.write(reinterpret_cast<const char*>(&len), sizeof(len));
		if (len > 0)
			fout.write(code.c_str(), len);
	}

	struct IndexEntry
	{
		std::string code;
		uint32_t file_offset;
		uint16_t candidate_count;
	};
	std::vector<IndexEntry> index_entries;
	index_entries.reserve(export_entries.size());

	size_t index_start = static_cast<size_t>(fout.tellp());
	for (const auto& kv : export_entries)
	{
		IndexEntry entry;
		entry.code = kv.first;
		entry.file_offset = 0;
		entry.candidate_count = static_cast<uint16_t>(kv.second.size());
		index_entries.push_back(entry);

		uint16_t code_len = static_cast<uint16_t>(entry.code.length());
		fout.write(reinterpret_cast<const char*>(&code_len), sizeof(code_len));
		if (code_len > 0)
			fout.write(entry.code.c_str(), code_len);
		fout.write(reinterpret_cast<const char*>(&entry.file_offset), sizeof(entry.file_offset));
		fout.write(reinterpret_cast<const char*>(&entry.candidate_count), sizeof(entry.candidate_count));
	}

	data_section_offset = static_cast<uint32_t>(fout.tellp());

	size_t idx = 0;
	for (const auto& kv : export_entries)
	{
		index_entries[idx].file_offset = static_cast<uint32_t>(fout.tellp());
		for (const auto& cand : kv.second)
		{
			fout.write(reinterpret_cast<const char*>(&cand.wubi_code_index), sizeof(cand.wubi_code_index));
			uint8_t flags = cand.uncommon ? 0x01 : 0x00;
			fout.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
			uint32_t text_len = static_cast<uint32_t>(cand.text.length());
			fout.write(reinterpret_cast<const char*>(&text_len), sizeof(text_len));
			if (text_len > 0)
				fout.write(reinterpret_cast<const char*>(cand.text.c_str()), text_len * sizeof(wchar_t));
		}
		++idx;
	}

	fout.seekp(offset_pos);
	fout.write(reinterpret_cast<const char*>(&data_section_offset), sizeof(data_section_offset));
	fout.seekp(index_start);
	for (const auto& entry : index_entries)
	{
		uint16_t code_len = static_cast<uint16_t>(entry.code.length());
		fout.write(reinterpret_cast<const char*>(&code_len), sizeof(code_len));
		if (code_len > 0)
			fout.write(entry.code.c_str(), code_len);
		fout.write(reinterpret_cast<const char*>(&entry.file_offset), sizeof(entry.file_offset));
		fout.write(reinterpret_cast<const char*>(&entry.candidate_count), sizeof(entry.candidate_count));
	}
	fout.close();

	if (!MoveFileExW(idx_tmp.c_str(), idx_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(idx_tmp.c_str());
		error = "Cannot replace output idx file";
		return false;
	}

	return true;
}

bool ime_dict::read_candidates_from_file(uint32_t offset, uint16_t count, std::vector<Candidate>& candidates, const std::wstring& ext)
{
	if (m_dict_bytes.empty())
	{
		OutputDebugStringA("ERROR: dictionary memory is empty\n");
		return false;
	}
	if (count == 0)
		return true;
	if (offset >= m_dict_file_size)
		return false;

	// candidates.clear();
	// candidates.reserve(count);
	candidates.reserve(candidates.size() + count);
	uint64_t cursor = static_cast<uint64_t>(offset);
	const uint64_t candidate_header_size = (m_index_format_version >= 4) ? 9ULL :
	                                       ((m_index_format_version >= 3) ? 8ULL : 6ULL);
	auto read_bytes = [this, &cursor](void* dst, size_t sz) -> bool {
		if (sz == 0)
			return true;
		if (cursor > m_dict_bytes.size() || sz > m_dict_bytes.size() - cursor)
			return false;
		std::memcpy(dst, m_dict_bytes.data() + cursor, sz);
		return true;
	};

	// 读取候选词
	for (uint16_t i = 0; i < count; ++i)
	{
		if (cursor > m_dict_file_size || (m_dict_file_size - cursor) < candidate_header_size)
			return false;

		uint32_t wubi_code_index = 0;
		uint8_t flags = 0;
		uint32_t text_len;

		if (m_index_format_version >= 3)
		{
			if (!read_bytes(&wubi_code_index, sizeof(wubi_code_index)))
				return false;
			cursor += sizeof(wubi_code_index);
		}
		else
		{
			uint16_t wubi_code_index_v2 = 0;
			if (!read_bytes(&wubi_code_index_v2, sizeof(wubi_code_index_v2)))
				return false;
			wubi_code_index = static_cast<uint32_t>(wubi_code_index_v2);
			cursor += sizeof(wubi_code_index_v2);
		}
		if (m_index_format_version >= 4)
		{
			if (!read_bytes(&flags, sizeof(flags)))
				return false;
			cursor += sizeof(flags);
		}
		if (!read_bytes(&text_len, sizeof(text_len)))
			return false;
		cursor += sizeof(text_len);

		if (wubi_code_index >= m_wubi_code_pool.size())
			return false;
		if (text_len > (std::numeric_limits<size_t>::max)() / sizeof(wchar_t))
			return false;
		const uint64_t text_bytes = static_cast<uint64_t>(text_len) * sizeof(wchar_t);
		if (cursor > m_dict_file_size || text_bytes > (m_dict_file_size - cursor))
			return false;

		std::wstring text(text_len, L'\0');
		if (text_len > 0)
		{
			if (!read_bytes(text.data(), static_cast<size_t>(text_bytes)))
				return false;
		}
		cursor += text_bytes;
		const bool uncommon = (flags & 0x01) != 0;
		if (ext.empty())	//正常查询
			candidates.emplace_back(text, wubi_code_index, false, uncommon);
		else if (wubi_code_index == 0) //在扩展时，只查询五笔编码，而不查询拼音编码
		{
			candidates.emplace_back(text, wubi_code_index, false, uncommon);
			candidates.back().prompt = ext;
		}
	}

	return true;
}


bool ime_dict::get_more(const std::string& narrow_code, std::vector<Candidate>& file_candidates, const std::wstring& ext)
{
	auto user_it = m_user_code_to_candidates.find(narrow_code);
	if (user_it != m_user_code_to_candidates.end())
	{
		for (const auto& c : user_it->second)
		{
			if (ext.empty())
			{
				file_candidates.emplace_back(c.text, c.wubi_code_index, true, c.uncommon);
			}
			else if (c.wubi_code_index == 0)
			{
				file_candidates.emplace_back(c.text, c.wubi_code_index, true, c.uncommon);
				file_candidates.back().prompt = ext;
			}
		}
	}

	// 使用文件词库：从索引查找偏移量，然后读取候选词
	auto it = m_code_index.find(narrow_code);
	if (it == m_code_index.end())
	{
		return true;
	}

	const CodeIndexEntry& entry = it->second;

	std::vector<Candidate> current;
	// 从文件读取候选词
	if (!read_candidates_from_file(entry.file_offset, entry.candidate_count, current, ext))
	{
		return false;
	}

	for (auto& i : current)
	{
		file_candidates.push_back(std::move(i));
	}

	// 用户屏蔽词过滤
	auto blocked_it = m_blocked_candidates.find(narrow_code);
	if (blocked_it != m_blocked_candidates.end() && !blocked_it->second.empty())
	{
		file_candidates.erase(
			std::remove_if(file_candidates.begin(), file_candidates.end(),
				[&blocked_it](const Candidate& c) {
					return blocked_it->second.find(c.text) != blocked_it->second.end();
				}),
			file_candidates.end());
	}

	auto uncommon_it = m_runtime_uncommon_candidates.find(narrow_code);
	if (uncommon_it != m_runtime_uncommon_candidates.end() && !uncommon_it->second.empty())
	{
		for (auto& c : file_candidates)
		{
			if (uncommon_it->second.find(c.text) != uncommon_it->second.end())
				c.uncommon = true;
		}
	}

	if (!m_show_uncommon_candidates)
	{
		file_candidates.erase(
			std::remove_if(file_candidates.begin(), file_candidates.end(),
				[](const Candidate& c) {
					return c.uncommon;
				}),
			file_candidates.end());
	}
	
	return file_candidates.size() <= 50;
}

bool ime_dict::get_candidates(const std::wstring& code,
                              std::vector<std::wstring>& candidates,
                              std::vector<std::wstring>& view_texts,
                              std::vector<bool>* pinyin_flags,
                              std::vector<bool>* exact_match_flags)
{
	candidates.clear();
	view_texts.clear();
	if (pinyin_flags)
		pinyin_flags->clear();
	if (exact_match_flags)
		exact_match_flags->clear();
	// 转换宽字符编码为窄字符
	std::string narrow_code = wstring_to_utf8(code);

	if (narrow_code.empty())
		return false;

	if (!m_use_file_dict)
		return false;

	std::vector<Candidate> file_candidates;
	bool ret = get_more(narrow_code, file_candidates);
	if (narrow_code.length() < 4 && ret)		//只对4编码以下的编码进行扩展查找
	{
		for (char i = 'a'; i <= 'z'; ++i)
		{
			std::string ext_code = narrow_code + i;
			if (!get_more(ext_code, file_candidates, std::wstring(1, static_cast<wchar_t>(i))))
			{
				break;
			}
		}
	}

	// 去重：优先保留用户词，再按配置排序。
	std::vector<Candidate> merged;
	merged.reserve(file_candidates.size());
	std::unordered_map<std::wstring, size_t> seen;
	for (const auto& cand : file_candidates)
	{
		auto it = seen.find(cand.text);
		if (it == seen.end())
		{
			seen.emplace(cand.text, merged.size());
			merged.push_back(cand);
			continue;
		}
		Candidate& existing = merged[it->second];
		if (!existing.user_defined && cand.user_defined)
		{
			const bool uncommon = existing.uncommon || cand.uncommon;
			existing = cand;
			existing.uncommon = uncommon;
		}
		else if (!existing.uncommon && cand.uncommon)
		{
			existing.uncommon = true;
		}
	}

	struct RankedCandidate
	{
		Candidate candidate;
		int64_t use_count = 0;
		int64_t last_used_at = 0;
	};
	std::vector<RankedCandidate> ranked;
	ranked.reserve(merged.size());
	for (auto& cand : merged)
	{
		RankedCandidate item;
		item.candidate = std::move(cand);
		if (m_candidate_sort_mode != candidate_sort_mode::fixed_order)
		{
			const std::string stat_key = make_candidate_stat_key(narrow_code, wstring_to_utf8(item.candidate.text));
			const auto stat_it = m_candidate_usage_stats.find(stat_key);
			if (stat_it != m_candidate_usage_stats.end())
			{
				item.use_count = stat_it->second.use_count;
				item.last_used_at = stat_it->second.last_used_at;
			}
		}
		ranked.push_back(std::move(item));
	}

	// 排序硬约束：
	// 全码匹配（无扩展提示字母）的五笔候选，必须整体排在最前面；
	// 即使拼音候选词频/最近使用更高，也不能越过这组候选。
	auto hard_group_rank = [](const RankedCandidate& item) {
		const bool is_full_match = item.candidate.prompt.empty();
		const bool is_wubi = !item.candidate.is_pinyin();
		return (is_full_match && is_wubi) ? 0 : 1;
	};

	if (m_candidate_sort_mode == candidate_sort_mode::fixed_order)
	{
		std::stable_sort(ranked.begin(), ranked.end(),
			[hard_group_rank](const RankedCandidate& lhs, const RankedCandidate& rhs) {
				const int lhs_group = hard_group_rank(lhs);
				const int rhs_group = hard_group_rank(rhs);
				if (lhs_group != rhs_group)
					return lhs_group < rhs_group;
				return lhs.candidate.user_defined && !rhs.candidate.user_defined;
			});
	}
	else if (m_candidate_sort_mode == candidate_sort_mode::recent)
	{
		std::stable_sort(ranked.begin(), ranked.end(),
			[hard_group_rank](const RankedCandidate& lhs, const RankedCandidate& rhs) {
				const int lhs_group = hard_group_rank(lhs);
				const int rhs_group = hard_group_rank(rhs);
				if (lhs_group != rhs_group)
					return lhs_group < rhs_group;
				if (lhs.last_used_at != rhs.last_used_at)
					return lhs.last_used_at > rhs.last_used_at;
				if (lhs.use_count != rhs.use_count)
					return lhs.use_count > rhs.use_count;
				if (lhs.candidate.user_defined != rhs.candidate.user_defined)
					return lhs.candidate.user_defined && !rhs.candidate.user_defined;
				return false;
			});
	}
	else
	{
		std::stable_sort(ranked.begin(), ranked.end(),
			[hard_group_rank](const RankedCandidate& lhs, const RankedCandidate& rhs) {
				const int lhs_group = hard_group_rank(lhs);
				const int rhs_group = hard_group_rank(rhs);
				if (lhs_group != rhs_group)
					return lhs_group < rhs_group;
				if (lhs.use_count != rhs.use_count)
					return lhs.use_count > rhs.use_count;
				if (lhs.last_used_at != rhs.last_used_at)
					return lhs.last_used_at > rhs.last_used_at;
				if (lhs.candidate.user_defined != rhs.candidate.user_defined)
					return lhs.candidate.user_defined && !rhs.candidate.user_defined;
				return false;
			});
	}

	// 生成候选词和显示文本
	candidates.reserve(ranked.size());
	view_texts.reserve(ranked.size());
	if (pinyin_flags)
		pinyin_flags->reserve(ranked.size());
	if (exact_match_flags)
		exact_match_flags->reserve(ranked.size());

	for (const RankedCandidate& ranked_cand : ranked)
	{
		const Candidate& cand = ranked_cand.candidate;
		candidates.push_back(cand.text);
		view_texts.push_back(generate_view_text(cand));
		if (pinyin_flags)
			pinyin_flags->push_back(cand.is_pinyin());
		if (exact_match_flags)
			exact_match_flags->push_back(cand.prompt.empty());
	}

	return !candidates.empty();
}

uint32_t ime_dict::get_or_create_wubi_code_index(const std::string& wubi_code)
{
	// 线性查找（编码池很小，通常<5000项）
	for (size_t i = 1; i < m_wubi_code_pool.size(); ++i)
	{
		if (m_wubi_code_pool[i] == wubi_code)
		{
			return static_cast<uint32_t>(i);
		}
	}

	// 未找到，创建新索引
	uint32_t new_index = static_cast<uint32_t>(m_wubi_code_pool.size());
	m_wubi_code_pool.push_back(wubi_code);
	return new_index;
}

std::wstring ime_dict::generate_view_text(const Candidate& candidate) const
{
	std::wstring view_text = candidate.text;
	if (!candidate.prompt.empty())
	{
		view_text += candidate.prompt;
	}
	else if (candidate.is_pinyin())
	{
		// 拼音候选词：显示为"字词(五笔编码)"
		if (candidate.wubi_code_index >= m_wubi_code_pool.size())
			return view_text;
		const std::string& wubi_code = m_wubi_code_pool[candidate.wubi_code_index];
		view_text += L"(";
		view_text += utf8_to_wstring(wubi_code);
		view_text += L")";
	}

	// 五笔候选词：直接显示字词
	return view_text;
}

std::wstring ime_dict::utf8_to_wstring(const std::string& utf8_str)
{
	if (utf8_str.empty())
		return {};

	const int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
	                                            static_cast<int>(utf8_str.size()), nullptr, 0);
	if (size_needed <= 0)
		return {};

	std::wstring wide_str(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
	                    static_cast<int>(utf8_str.size()), wide_str.data(), size_needed);

	return wide_str;
}

std::string ime_dict::wstring_to_utf8(const std::wstring& wide_str)
{
	if (wide_str.empty())
		return {};

	const int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(),
	                                            static_cast<int>(wide_str.size()), nullptr, 0, nullptr, nullptr);
	if (size_needed <= 0)
		return {};

	std::string utf8_str(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(),
	                    static_cast<int>(wide_str.size()), utf8_str.data(), size_needed, nullptr, nullptr);

	return utf8_str;
}
