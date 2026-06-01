#pragma once

#include "globals.h"
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

// 候选词结构（简洁版）
struct Candidate
{
	std::wstring text;        // 候选词文本
	uint32_t wubi_code_index; // 五笔编码索引（0表示非拼音候选词）
	std::wstring prompt;
	bool user_defined;
	bool uncommon;

	Candidate(std::wstring t = L"", uint32_t code_idx = 0, bool from_user = false, bool is_uncommon = false);

	bool is_pinyin() const;
};

// 编码索引项（内存中）
struct CodeIndexEntry
{
	uint16_t candidate_count; // 候选词数量
	uint32_t file_offset;     // 文件中的偏移量（uint32_t足够，节省内存）
};

class ime_dict
{
public:
	enum class candidate_sort_mode
	{
		fixed_order = 0, // 固定词序
		recent = 1,      // 按最近输入排序
		frequency = 2    // 按输入次数排序
	};

	ime_dict();
	~ime_dict();

	// 初始化词库（仅加载dict.idx）
	bool init();
	void unload();

	bool get_more(const std::string& narrow_code, std::vector<Candidate>& file_candidates, const std::wstring& ext = L"");

	// 获取候选词
	bool get_candidates(const std::wstring& code,
	                    std::vector<std::wstring>& candidates,
	                    std::vector<std::wstring>& view_texts,
	                    std::vector<bool>* pinyin_flags = nullptr);
	bool add_custom_word(const std::wstring& word, const std::string& code, std::string& error);
	bool delete_candidate(const std::wstring& code, const std::wstring& candidate, std::string& error);
	bool mark_candidate_uncommon(const std::wstring& code, const std::wstring& candidate, std::string& error);
	bool export_raw_dictionary(const std::filesystem::path& output_path, std::string& error);
	void set_show_uncommon_candidates(bool show) { m_show_uncommon_candidates = show; }
	[[nodiscard]] bool show_uncommon_candidates() const { return m_show_uncommon_candidates; }
	void set_candidate_sort_mode(candidate_sort_mode mode) { m_candidate_sort_mode = mode; }
	[[nodiscard]] candidate_sort_mode get_candidate_sort_mode() const { return m_candidate_sort_mode; }
	void record_candidate_selected(const std::wstring& code, const std::wstring& candidate);

private:
	// 词库文件句柄（保持打开以便按需读取）
	std::ifstream m_dict_file;
	uint32_t m_index_format_version;
	uint64_t m_dict_file_size;

	bool m_use_file_dict;  // 是否使用文件词库

	// 五笔编码字符串池（去重存储，索引0保留为"无编码"）
	std::vector<std::string> m_wubi_code_pool;

	// 编码到文件偏移的映射（使用string支持任意长度编码）
	std::map<std::string, CodeIndexEntry> m_code_index;
	std::map<std::string, std::vector<Candidate>> m_user_code_to_candidates;
	std::map<std::string, std::unordered_set<std::wstring>> m_blocked_candidates;
	std::map<std::string, std::unordered_set<std::wstring>> m_runtime_uncommon_candidates;
	bool m_show_uncommon_candidates;
	candidate_sort_mode m_candidate_sort_mode;

	struct CandidateUsageStat
	{
		int64_t use_count = 0;
		int64_t last_used_at = 0;
	};
	std::unordered_map<std::string, CandidateUsageStat> m_candidate_usage_stats;

	// 辅助函数：UTF-8转宽字符
	static std::wstring utf8_to_wstring(const std::string& utf8_str);

	// 辅助函数：宽字符转UTF-8
	static std::string wstring_to_utf8(const std::wstring& wide_str);

	// 获取或创建五笔编码索引（线性查找，适合小集合）
	uint32_t get_or_create_wubi_code_index(const std::string& wubi_code);

	// 生成候选词显示文本
	std::wstring generate_view_text(const Candidate& candidate) const;

	// 从索引文件加载（推荐）
	bool load_from_index(const char* filename);
	bool load_user_dict();
	bool load_blocked_dict();
	bool load_candidate_stats();
	std::filesystem::path get_user_db_path() const;
	void refresh_overlay_if_changed();
	static std::string make_candidate_stat_key(const std::string& code, const std::string& candidate_utf8);

	// 从文件读取候选词
	bool read_candidates_from_file(uint32_t offset, uint16_t count, std::vector<Candidate>& candidates, const std::wstring& ext = L"");

	std::filesystem::file_time_type m_overlay_last_write_time;
	bool m_overlay_time_initialized;
};
