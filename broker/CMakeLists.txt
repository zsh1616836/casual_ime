add_executable(zime_broker WIN32
    main.cpp
    broker_log.cpp
    broker_ui.cpp
    broker_storage.cpp
    ${CMAKE_SOURCE_DIR}/common/perf_trace.cpp
    ${CMAKE_SOURCE_DIR}/ime/candidate_form.cpp
    ${CMAKE_SOURCE_DIR}/ime/globals.cpp
    ${CMAKE_SOURCE_DIR}/ime/status_window.cpp
    ${CMAKE_SOURCE_DIR}/ime/tool.cpp
    ${CMAKE_SOURCE_DIR}/ime/ime_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/ime/3rd/sqlite_tool.cpp
    ${CMAKE_SOURCE_DIR}/ime/3rd/sqlite3.c
)

target_include_directories(zime_broker PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/ime
)

target_link_libraries(zime_broker PRIVATE
    advapi32
    bcrypt
    gdi32
    gdiplus
    comctl32
    comdlg32
    shell32
    user32
)

set_property(TARGET zime_broker PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
