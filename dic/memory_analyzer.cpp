// memory_analyzer.cpp - 内存占用分析工具
// 编译: cl memory_analyzer.cpp /EHsc /O2
// 使用: memory_analyzer.exe dict.dic

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <algorithm>

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cout << "Usage: memory_analyzer.exe dict.dic" << std::endl;
		return 1;
	}

	const char* input_file = argv[1];

	std::ifstream fin(input_file, std::ios::binary);
	if (!fin)
	{
		std::cerr << "Error: Cannot open " << input_file << std::endl;
		return 1;
	}

	// 跳过UTF-8 BOM
	char bom[3];
	fin.read(bom, 3);
	if (!(bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF))
	{
		fin.seekg(0);
	}

	std::map<size_t, size_t> code_length_dist;  // 长度 -> 数量
	size_t total_codes = 0;
	size_t total_code_chars = 0;
	size_t max_length = 0;

	std::string line;
	while (std::getline(fin, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;

		size_t first_comma = line.find(',');
		if (first_comma == std::string::npos || first_comma == 0)
			continue;

		std::string code = line.substr(0, first_comma);
		size_t len = code.length();
		
		code_length_dist[len]++;
		total_codes++;
		total_code_chars += len;
		if (len > max_length)
			max_length = len;
	}
	fin.close();

	std::cout << "\n========== 编码长度分析 ==========\n" << std::endl;
	std::cout << "总编码数: " << total_codes << std::endl;
	std::cout << "最大长度: " << max_length << " 字符" << std::endl;
	std::cout << "平均长度: " << (double)total_code_chars / total_codes << " 字符" << std::endl;
	std::cout << "\n长度分布:\n" << std::endl;

	for (const auto& kv : code_length_dist)
	{
		double percentage = (double)kv.second / total_codes * 100;
		std::cout << "  长度 " << kv.first << ": " 
			<< kv.second << " 条 (" 
			<< percentage << "%)" << std::endl;
	}

	std::cout << "\n========== 内存占用估算 ==========\n" << std::endl;

	// std::map<std::string, CodeIndexEntry> 的内存占用
	// std::map 每个节点：
	//   - 红黑树指针: 3×8 = 24 字节 (父/左/右)
	//   - 颜色: 1 字节 + 7字节对齐 = 8字节
	//   - key (std::string): 32字节 (SSO: 24字节buf + 8字节size/cap)
	//   - value (CodeIndexEntry): 6字节 (uint32_t + uint16_t) + 2字节对齐 = 8字节
	// 总计: 24 + 8 + 32 + 8 = 72字节/节点
	
	size_t map_overhead = 72;  // 每个节点的固定开销
	size_t string_sso_threshold = 15;  // MSVC的SSO阈值
	
	size_t total_map_memory = 0;
	for (const auto& kv : code_length_dist)
	{
		size_t len = kv.first;
		size_t count = kv.second;
		
		size_t per_entry = map_overhead;
		if (len > string_sso_threshold)
		{
			// 超过SSO阈值，需要堆分配
			per_entry += len + 1;  // +1 for null terminator
		}
		
		total_map_memory += per_entry * count;
	}

	std::cout << "std::map<std::string, CodeIndexEntry> 内存:\n" << std::endl;
	std::cout << "  节点数: " << total_codes << std::endl;
	std::cout << "  每节点平均: " << (double)total_map_memory / total_codes << " 字节" << std::endl;
	std::cout << "  总计: " << total_map_memory / 1024 / 1024 << " MB" << std::endl;

	std::cout << "\n========== 优化建议 ==========\n" << std::endl;

	// 统计<=4字符的比例
	size_t short_codes = 0;
	for (const auto& kv : code_length_dist)
	{
		if (kv.first <= 4)
			short_codes += kv.second;
	}
	double short_percentage = (double)short_codes / total_codes * 100;

	std::cout << "≤4字符的编码占比: " << short_percentage << "%" << std::endl;

	if (short_percentage > 95)
	{
		std::cout << "\n方案1: 混合存储（推荐）" << std::endl;
		std::cout << "  - ≤4字符: 用uint32_t编码 (4字节)" << std::endl;
		std::cout << "  - >4字符: 用std::string" << std::endl;
		std::cout << "  - 预期内存: ~" << (short_codes * 4 + (total_codes - short_codes) * 72) / 1024 / 1024 << " MB" << std::endl;
	}

	if (max_length <= 8)
	{
		std::cout << "\n方案2: 固定长度数组" << std::endl;
		std::cout << "  - 用char[8]替代std::string" << std::endl;
		std::cout << "  - 预期内存: ~" << (total_codes * (24 + 8 + 8 + 8)) / 1024 / 1024 << " MB" << std::endl;
	}

	std::cout << "\n当前方案内存: ~" << total_map_memory / 1024 / 1024 << " MB" << std::endl;

	return 0;
}



