add_executable(zime_registrar zime_registrar.cpp)

target_link_libraries(zime_registrar PRIVATE ole32)

set_property(TARGET zime_registrar PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
