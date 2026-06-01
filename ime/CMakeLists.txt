cmake_minimum_required(VERSION 3.23)

project(casual_ime)

file(GLOB_RECURSE current CONFIGURE_DEPENDS *.h *.cpp *.hpp *.cc *.c)
list(APPEND current
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite_tool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3.c
)

# 添加 .def 文件
set(DEF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/casual_ime.def)

add_library(casual_ime SHARED ${current} ${DEF_FILE})

# 链接必要的库
target_link_libraries(casual_ime PRIVATE 
    user32
    gdi32
    ole32
    oleaut32
    uuid
    shlwapi
    advapi32
    comctl32
    gdiplus
)

set_property(TARGET casual_ime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
