#pragma once

#include <windows.h>

/*
 * 输入法配置文件
 * 
 * 所有输入法的名称、描述、GUID 等配置信息都集中在这里
 * 修改输入法名称时，只需修改此文件即可
 */

// ============================================================
// 输入法基本信息（修改名称时只需改这里）
// ============================================================
#define IME_NAME            L"随意五笔输入法"
#define IME_DESCRIPTION     L"随意五笔输入法"
#define IME_VERSION         L"1.1"
#define IME_AUTHOR          L""

// ============================================================
// GUID 定义（注册后不要修改，除非完全重装）
// ============================================================
// {8F8B8F8F-8F8B-8F8B-8F8B-8F8B8F8B8F8B}
static const GUID c_clsidTextService = 
{ 0x8f8b8f8f, 0x8f8b, 0x8f8b, { 0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x8b } };

// {8F8B8F90-8F8B-8F8B-8F8B-8F8B8F8B8F90}
static const GUID c_guidProfile = 
{ 0x8f8b8f90, 0x8f8b, 0x8f8b, { 0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x90 } };

// ============================================================
// 文件路径配置
// ============================================================
#define DICTIONARY_PATH     "dict.db"
#define DICTIONARY_PATH_W   L"dict.db"
#define CONFIG_FILE         L"config.ini"
#define LOG_FILE            L"ime_log.txt"

// ============================================================
// COM 配置
// ============================================================
#define COM_MODEL           L"Apartment"
#define ICON_INDEX          0
#define LANGUAGE_ID         0x0804  // 中文（简体，中国）

// ============================================================
// 向后兼容的宏定义
// ============================================================
#define TEXTSERVICE_NAME        IME_NAME
#define TEXTSERVICE_DESC        IME_DESCRIPTION
#define TEXTSERVICE_MODEL       COM_MODEL
#define TEXTSERVICE_ICON_INDEX  ICON_INDEX

