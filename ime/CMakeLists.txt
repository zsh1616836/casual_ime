cmake_minimum_required(VERSION 3.23)

project(casual_ime)

file(GLOB_RECURSE current CONFIGURE_DEPENDS *.h *.cpp *.hpp *.cc *.c)
list(REMOVE_ITEM current
    ${CMAKE_CURRENT_SOURCE_DIR}/ime_dictionary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite_tool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3.c
)

# 添加 .def 文件
set(DEF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/casual_ime.def)

add_library(casual_ime SHARED ${current} ${DEF_FILE})
target_sources(casual_ime PRIVATE ${CMAKE_SOURCE_DIR}/common/perf_trace.cpp)
target_include_directories(casual_ime PRIVATE ${CMAKE_SOURCE_DIR}/common)

add_dependencies(casual_ime zime_broker)
add_custom_command(TARGET casual_ime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:zime_broker>
        $<TARGET_FILE_DIR:casual_ime>/zime_broker.exe
)

# 链接必要的库
target_link_libraries(casual_ime PRIVATE 
    user32
    gdi32
    ole32
    oleaut32
    uuid
    shlwapi
    shell32
    advapi32
    bcrypt
    comctl32
    gdiplus
)

set_property(TARGET casual_ime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
