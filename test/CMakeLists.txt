add_executable(tsf_contract_smoke tsf_contract_smoke.cpp)

target_link_libraries(tsf_contract_smoke PRIVATE
    ole32
    oleaut32
    uuid
)

set_property(TARGET tsf_contract_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(broker_probe_smoke broker_probe_smoke.cpp)
target_include_directories(broker_probe_smoke PRIVATE ${CMAKE_SOURCE_DIR}/common)
set_property(TARGET broker_probe_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(broker_client_smoke
    broker_client_smoke.cpp
    ${CMAKE_SOURCE_DIR}/ime/broker_client.cpp
    ${CMAKE_SOURCE_DIR}/ime/ime_trace.cpp
    ${CMAKE_SOURCE_DIR}/ime/tool.cpp
    ${CMAKE_SOURCE_DIR}/common/perf_trace.cpp
)
target_include_directories(broker_client_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/ime
)
target_link_libraries(broker_client_smoke PRIVATE bcrypt shell32 user32)
add_dependencies(broker_client_smoke zime_broker)
add_custom_command(TARGET broker_client_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:zime_broker>
        $<TARGET_FILE_DIR:broker_client_smoke>/zime_broker.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/dict.idx
        $<TARGET_FILE_DIR:broker_client_smoke>/dict.idx
)
set_property(TARGET broker_client_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(status_window_lifecycle_smoke
    status_window_lifecycle_smoke.cpp
    ${CMAKE_SOURCE_DIR}/ime/status_window.cpp
    ${CMAKE_SOURCE_DIR}/ime/tool.cpp
    ${CMAKE_SOURCE_DIR}/ime/globals.cpp
)
target_include_directories(status_window_lifecycle_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/ime
)
target_link_libraries(status_window_lifecycle_smoke PRIVATE
    comctl32 gdiplus shell32 user32 gdi32
)
set_property(TARGET status_window_lifecycle_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(window_lifecycle_probe window_lifecycle_probe.cpp)
set_property(TARGET window_lifecycle_probe PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Exercises the real UI controller without a pipe server or initialized storage.
add_executable(broker_status_focus_smoke
    broker_status_focus_smoke.cpp
    ${CMAKE_SOURCE_DIR}/broker/broker_ui.cpp
    ${CMAKE_SOURCE_DIR}/broker/broker_log.cpp
    ${CMAKE_SOURCE_DIR}/broker/broker_storage.cpp
    ${CMAKE_SOURCE_DIR}/common/perf_trace.cpp
    ${CMAKE_SOURCE_DIR}/ime/candidate_form.cpp
    ${CMAKE_SOURCE_DIR}/ime/globals.cpp
    ${CMAKE_SOURCE_DIR}/ime/status_window.cpp
    ${CMAKE_SOURCE_DIR}/ime/tool.cpp
    ${CMAKE_SOURCE_DIR}/ime/ime_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/ime/3rd/sqlite_tool.cpp
    ${CMAKE_SOURCE_DIR}/ime/3rd/sqlite3.c
)
target_include_directories(broker_status_focus_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/broker
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/ime
)
target_link_libraries(broker_status_focus_smoke PRIVATE
    advapi32 bcrypt gdi32 gdiplus comctl32 comdlg32 shell32 user32
)
set_property(TARGET broker_status_focus_smoke PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
