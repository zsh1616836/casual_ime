#ifndef TOOL_H
#define TOOL_H
#include <filesystem>

class tool
{
public:
	static std::filesystem::path get_current_dll_path();
};


#endif
