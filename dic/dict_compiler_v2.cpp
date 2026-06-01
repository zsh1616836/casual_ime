// dict_compiler_v2.cpp - 优化的词库编译工具
// 编译为索引+数据分离的二进制格式
// 编译: cl dict_compiler_v2.cpp /EHsc /O2
// 使用: dict_compiler_v2.exe dict.dic dict.idx

#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <Windows.h>

// UTF-8转宽字符
std::wstring utf8_to_wstring(const std::string& utf8_str)
{
	if (utf8_str.empty())
		return {};

	int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
	                                      static_cast<int>(utf8_str.size()), nullptr, 0);
	if (size_needed <= 0)
		return {};

	std::wstring wide_str(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(),
	                    static_cast<int>(utf8_str.size()), wide_str.data(), size_needed);

	return wide_str;
}

// 编码字符串转整数
uint32_t encode_string_to_int(const std::string& code)
{
	uint32_t result = 0;
	size_t len = code.length();
	if (len > 4) len = 4;

	for (size_t i = 0; i < len; ++i)
	{
		unsigned char c = code[i];
		if (c >= 'a' && c <= 'y')
		{
			result |= static_cast<uint32_t>((c - 'a') << (i * 8));
		}
		else if (c >= 'A' && c <= 'Y')
		{
			result |= static_cast<uint32_t>((c - 'A') << (i * 8));
		}
	}

	return result;
}

// 二进制文件格式：
// [Header]
// uint32_t: magic (0x44494358 = "DIDX")
// uint32_t: version (3)
// uint32_t: wubi_code_count
// uint32_t: code_entry_count
// uint32_t: data_section_offset
//
// [Wubi Code Pool]
// for each wubi code:
//   uint16_t: length
//   char[]: utf8 string
//
// [Code Index Section]
// for each code entry:
//   uint16_t: code_length (编码长度)
//   char[]: code (编码字符串)
//   uint32_t: file_offset (在数据区的偏移)
//   uint16_t: candidate_count
//
// [Data Section]
// for each code's candidates:
//   for each candidate:
//     uint32_t: wubi_code_index
//     uint32_t: text_length
//     wchar_t[]: text

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		std::cout << "Usage: dict_compiler_v2 <input.dic> <output.idx>" << std::endl;
		return 1;
	}

	const char* input_file = argv[1];
	const char* output_file = argv[2];

	std::cout << "Reading " << input_file << "..." << std::endl;

	// 读取输入文件
	std::ifstream fin(input_file, std::ios::binary);
	if (!fin)
	{
		std::cerr << "Error: Cannot open input file" << std::endl;
		return 1;
	}

	// 跳过UTF-8 BOM
	char bom[3];
	fin.read(bom, 3);
	if (!(bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF))
	{
		fin.seekg(0);
	}

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(fin, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty())
			lines.push_back(line);
	}
	fin.close();

	std::cout << "Read " << lines.size() << " lines" << std::endl;

	// 数据结构
	struct Candidate
	{
		std::wstring text;
		uint32_t wubi_code_index;
		uint8_t flags;
	};

	std::vector<std::string> wubi_code_pool;
	wubi_code_pool.push_back("");  // 索引0保留

	// 编码到候选词列表的映射（使用字符串支持任意长度）
	std::map<std::string, std::vector<Candidate>> code_to_candidates;

	// 解析每一行
	for (const auto& line : lines)
	{
		if (line.empty())
			continue;

		size_t first_comma = line.find(',');
		if (first_comma == std::string::npos || first_comma == 0)
			continue;

		std::string code_str = line.substr(0, first_comma);

		size_t start = first_comma + 1;
		std::vector<Candidate>& candidates = code_to_candidates[code_str];

		while (start < line.length())
		{
			size_t comma_pos = line.find(',', start);
			size_t end = (comma_pos == std::string::npos) ? line.length() : comma_pos;
			
			if (end <= start)
				break;

			std::string candidate_str = line.substr(start, end - start);
			start = end + 1;

			if (candidate_str.empty())
				continue;

			std::wstring text;
			uint32_t wubi_code_index = 0;
			uint8_t flags = 0;

			if (!candidate_str.empty() && candidate_str.back() == ')')
			{
				flags |= 0x01;
				candidate_str.pop_back();
			}

			size_t paren_pos = candidate_str.find('(');
			if (paren_pos != std::string::npos && paren_pos > 0)
			{
				// 拼音候选词
				std::string text_part = candidate_str.substr(0, paren_pos);
				text = utf8_to_wstring(text_part);
				std::string wubi_code = candidate_str.substr(paren_pos + 1);
				
				// 查找或创建五笔编码索引
				bool found = false;
				for (size_t i = 1; i < wubi_code_pool.size(); ++i)
				{
					if (wubi_code_pool[i] == wubi_code)
					{
						wubi_code_index = static_cast<uint32_t>(i);
						found = true;
						break;
					}
				}
				if (!found)
				{
					wubi_code_index = static_cast<uint32_t>(wubi_code_pool.size());
					wubi_code_pool.push_back(wubi_code);
				}
			}
			else
			{
				// 纯五笔候选词
				text = utf8_to_wstring(candidate_str);
				wubi_code_index = 0;
			}

			candidates.push_back({text, wubi_code_index, flags});
		}
	}

	std::cout << "Wubi codes: " << wubi_code_pool.size() << std::endl;
	std::cout << "Code entries: " << code_to_candidates.size() << std::endl;

	// 统计候选词总数
	size_t total_candidates = 0;
	for (const auto& kv : code_to_candidates)
	{
		total_candidates += kv.second.size();
	}
	std::cout << "Total candidates: " << total_candidates << std::endl;

	// 写入二进制文件
	std::cout << "Writing " << output_file << "..." << std::endl;

	std::ofstream fout(output_file, std::ios::binary);
	if (!fout)
	{
		std::cerr << "Error: Cannot create output file" << std::endl;
		return 1;
	}

	// Header
	uint32_t magic = 0x44494458;  // "DIDX"
	uint32_t version = 4;
	uint32_t wubi_code_count = static_cast<uint32_t>(wubi_code_pool.size());
	uint32_t code_entry_count = static_cast<uint32_t>(code_to_candidates.size());
	uint32_t data_section_offset = 0;  // 稍后回填

	fout.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
	fout.write(reinterpret_cast<const char*>(&version), sizeof(version));
	fout.write(reinterpret_cast<const char*>(&wubi_code_count), sizeof(wubi_code_count));
	fout.write(reinterpret_cast<const char*>(&code_entry_count), sizeof(code_entry_count));
	
	size_t offset_pos = fout.tellp();
	fout.write(reinterpret_cast<const char*>(&data_section_offset), sizeof(data_section_offset));

	// Wubi Code Pool
	for (const auto& code : wubi_code_pool)
	{
		uint16_t len = static_cast<uint16_t>(code.length());
		fout.write(reinterpret_cast<const char*>(&len), sizeof(len));
		if (len > 0)
			fout.write(code.c_str(), len);
	}

	// Code Index Section (先占位，稍后回填)
	struct IndexEntry
	{
		std::string code;
		uint32_t file_offset;
		uint16_t candidate_count;
	};

	std::vector<IndexEntry> index_entries;
	index_entries.reserve(code_to_candidates.size());

	size_t index_start = fout.tellp();
	for (const auto& kv : code_to_candidates)
	{
		IndexEntry entry;
		entry.code = kv.first;
		entry.file_offset = 0;  // 稍后回填
		entry.candidate_count = static_cast<uint16_t>(kv.second.size());
		index_entries.push_back(entry);
		
		// 写入编码字符串
		uint16_t code_len = static_cast<uint16_t>(entry.code.length());
		fout.write(reinterpret_cast<const char*>(&code_len), sizeof(code_len));
		if (code_len > 0)
			fout.write(entry.code.c_str(), code_len);
		
		// 写入偏移和数量
		fout.write(reinterpret_cast<const char*>(&entry.file_offset), sizeof(entry.file_offset));
		fout.write(reinterpret_cast<const char*>(&entry.candidate_count), sizeof(entry.candidate_count));
	}

	// Data Section
	data_section_offset = static_cast<uint32_t>(fout.tellp());
	
	std::cout << "Data section starts at offset: " << data_section_offset << std::endl;

	size_t idx = 0;
	for (const auto& kv : code_to_candidates)
	{
		// 记录当前位置
		index_entries[idx].file_offset = static_cast<uint32_t>(fout.tellp());
		
		// 调试：打印前几个编码的信息
		if (idx < 5 || kv.first == "a") {
			std::cout << "Code '" << kv.first << "' at offset " 
				<< index_entries[idx].file_offset 
				<< " with " << kv.second.size() << " candidates" << std::endl;
		}
		
		// 写入候选词数据
		for (const auto& cand : kv.second)
		{
			fout.write(reinterpret_cast<const char*>(&cand.wubi_code_index), sizeof(cand.wubi_code_index));
			fout.write(reinterpret_cast<const char*>(&cand.flags), sizeof(cand.flags));
			uint32_t text_len = static_cast<uint32_t>(cand.text.length());
			fout.write(reinterpret_cast<const char*>(&text_len), sizeof(text_len));
			if (text_len > 0)
				fout.write(reinterpret_cast<const char*>(cand.text.c_str()), text_len * sizeof(wchar_t));
		}
		
		idx++;
	}

	// 回填data_section_offset
	fout.seekp(offset_pos);
	fout.write(reinterpret_cast<const char*>(&data_section_offset), sizeof(data_section_offset));

	// 回填索引区的文件偏移
	fout.seekp(index_start);
	for (const auto& entry : index_entries)
	{
		// 写入编码字符串
		uint16_t code_len = static_cast<uint16_t>(entry.code.length());
		fout.write(reinterpret_cast<const char*>(&code_len), sizeof(code_len));
		if (code_len > 0)
			fout.write(entry.code.c_str(), code_len);
		
		// 写入偏移和数量
		fout.write(reinterpret_cast<const char*>(&entry.file_offset), sizeof(entry.file_offset));
		fout.write(reinterpret_cast<const char*>(&entry.candidate_count), sizeof(entry.candidate_count));
	}

	fout.close();

	std::cout << "Done! File size: " << std::ifstream(output_file, std::ios::ate | std::ios::binary).tellg() << " bytes" << std::endl;
	std::cout << "\nMemory usage estimate:" << std::endl;
	std::cout << "  Index in memory: ~" << (code_entry_count * 15 / 1024) << " KB (approx)" << std::endl;
	std::cout << "  (Each entry: 2+code+4+2 bytes, avg ~15 bytes)" << std::endl;

	return 0;
}

