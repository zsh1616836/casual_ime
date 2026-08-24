#pragma once

#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
#include "ime_config.h"

// GUID字符串长度 (包括{}和终止符)
#define CLSID_STRLEN 39

// 注意：输入法名称、描述、GUID 等配置已迁移到 ime_config.h
// 如需修改这些信息，请编辑 ime_config.h 文件

// 全局变量
extern HINSTANCE g_hInst;
extern LONG g_cRefDll;

// 词库条目
struct DictEntry {
    std::wstring code;
    std::vector<std::wstring> candidates;
};

// 函数声明
BOOL register_server();
BOOL unregister_server();
BOOL register_categories();
BOOL unregister_categories();
BOOL register_profile();
BOOL unregister_profile();
BOOL register_broker_installation();
BOOL unregister_broker_installation();
void registration_trace(const wchar_t* stage, HRESULT result);

// 工具函数
void dll_add_ref();
void dll_release();
