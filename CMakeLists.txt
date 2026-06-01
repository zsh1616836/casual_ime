cmake_minimum_required(VERSION 3.23)


project(casual_ime)


set(CMAKE_CXX_STANDARD 17)


add_definitions(-DUNICODE -D_UNICODE)


add_subdirectory(ime)
add_subdirectory(dic)