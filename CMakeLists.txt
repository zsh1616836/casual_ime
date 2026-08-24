cmake_minimum_required(VERSION 3.23)


project(casual_ime)


set(CMAKE_CXX_STANDARD 17)


add_definitions(-DUNICODE -D_UNICODE)

option(ZIME_PERF_DIAGNOSTIC "Build in-memory input performance tracing" OFF)
if(ZIME_PERF_DIAGNOSTIC)
    add_compile_definitions(ZIME_PERF_DIAGNOSTIC=1)
endif()

add_subdirectory(broker)
add_subdirectory(ime)
add_subdirectory(dic)
add_subdirectory(installer)

option(ZIME_BUILD_TESTS "Build ZIme smoke tests" ON)
if(ZIME_BUILD_TESTS)
    add_subdirectory(test)
endif()
