#ifndef TOOL_H
#define TOOL_H
#include <filesystem>

class tool
{
public:
	static std::filesystem::path get_current_dll_path();
	static std::filesystem::path get_user_data_path();
};


#endif
