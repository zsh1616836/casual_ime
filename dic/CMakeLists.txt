cmake_minimum_required(VERSION 3.23)

project(dic_compiler)

add_executable(dict_compiler dict_compiler_v2.cpp)

set_property(TARGET dict_compiler PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
