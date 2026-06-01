// analyze_memory.cpp - 内存占用分析工具
// 编译: 在build目录运行 cl ../dic/analyze_memory.cpp /EHsc /O2
// 运行: analyze_memory.exe

#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <set>
#include <Windows.h>

int main()
{
	std::cout << "Reading dict.dic..." << std::endl;
	
	std::ifstream fin("dict.dic", std::ios::binary);
	if (!fin)
	{
		std::cerr << "Cannot open dict.dic" << std::endl;
		return 1;
	}

	// 跳过BOM
	char bom[3];
	fin.read(bom, 3);
	if (!(bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF))
	{
		fin.seekg(0);
	}

	std::map<std::string, int> code_map;  // 模拟实际的map
	std::set<int> code_lengths;
	size_t max_code_len = 0;
	size_t total_code_chars = 0;

	std::string line;
	while (std::getline(fin, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;

		size_t comma = line.find(',');
		if (comma == std::string::npos || comma == 0)
			continue;

		std::string code = line.substr(0, comma);
		code_map[code] = 1;  // 模拟存储
		
		code_lengths.insert(code.length());
		if (code.length() > max_code_len)
			max_code_len = code.length();
		total_code_chars += code.length();
	}
	fin.close();

	// 统计结果
	std::cout << "\n=== 编码统计 ===" << std::endl;
	std::cout << "编码总数: " << code_map.size() << std::endl;
	std::cout << "最长编码: " << max_code_len << " 字符" << std::endl;
	std::cout << "平均长度: " << (total_code_chars / (double)code_map.size()) << " 字符" << std::endl;
	
	std::cout << "\n编码长度分布: ";
	for (int len : code_lengths)
		std::cout << len << " ";
	std::cout << std::endl;

	// 统计长度分布
	std::map<size_t, int> length_dist;
	for (const auto& kv : code_map)
	{
		length_dist[kv.first.length()]++;
	}

	std::cout << "\n=== 详细分布 ===" << std::endl;
	for (const auto& kv : length_dist)
	{
		std::cout << "长度 " << kv.first << ": " << kv.second << " 个编码 ("
			<< (kv.second * 100.0 / code_map.size()) << "%)" << std::endl;
	}

	// 内存估算
	std::cout << "\n=== 内存占用估算 ===" << std::endl;
	
	// std::map 节点开销
	// 每个节点: key(string) + value(CodeIndexEntry) + 红黑树指针(3个) + 其他
	size_t map_node_overhead = sizeof(void*) * 3 + 16;  // 红黑树节点
	size_t value_size = sizeof(uint32_t) + sizeof(uint16_t);  // CodeIndexEntry
	
	size_t total_map_memory = 0;
	for (const auto& kv : code_map)
	{
		// std::string内存: 
		// SSO阈值通常是15字节，小于等于15字节不分配堆内存
		size_t string_mem = 0;
		if (kv.first.length() <= 15)
		{
			string_mem = sizeof(std::string);  // 通常24字节（包含inline buffer）
		}
		else
		{
			string_mem = sizeof(std::string) + kv.first.length() + 1;
		}
		
		total_map_memory += string_mem + value_size + map_node_overhead;
	}

	std::cout << "std::map节点数: " << code_map.size() << std::endl;
	std::cout << "每个节点平均: " << (total_map_memory / code_map.size()) << " 字节" << std::endl;
	std::cout << "总内存: " << (total_map_memory / 1024 / 1024.0) << " MB" << std::endl;
	
	std::cout << "\n=== 组成分析 ===" << std::endl;
	std::cout << "- 字符串数据: " << (total_code_chars / 1024.0) << " KB" << std::endl;
	std::cout << "- std::string对象: " << (code_map.size() * sizeof(std::string) / 1024.0) << " KB" << std::endl;
	std::cout << "- CodeIndexEntry: " << (code_map.size() * value_size / 1024.0) << " KB" << std::endl;
	std::cout << "- 红黑树开销: " << (code_map.size() * map_node_overhead / 1024.0) << " KB" << std::endl;

	// 优化建议
	std::cout << "\n=== 优化潜力 ===" << std::endl;
	
	// 统计<=4字符的编码
	int short_codes = 0;
	for (const auto& kv : code_map)
	{
		if (kv.first.length() <= 4)
			short_codes++;
	}
	
	std::cout << "编码<=4字符: " << short_codes << " 个 (" 
		<< (short_codes * 100.0 / code_map.size()) << "%)" << std::endl;
	
	if (short_codes > code_map.size() * 0.95)
	{
		std::cout << "\n💡 建议: 95%+的编码都<=4字符，可以优化！" << std::endl;
		std::cout << "   方案: 使用uint32_t存储<=4字符编码，std::string存储长编码" << std::endl;
		std::cout << "   节省: ~" << (short_codes * (sizeof(std::string) - 4) / 1024 / 1024.0) << " MB" << std::endl;
	}
	else
	{
		std::cout << "\n⚠️  长编码占比较高，当前方案已是最优" << std::endl;
	}

	return 0;
}

