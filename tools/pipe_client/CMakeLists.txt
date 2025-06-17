add_executable(pipe_client main.cpp)

target_compile_features(pipe_client PUBLIC cxx_std_20)
set_target_properties(pipe_client PROPERTIES CXX_EXTENSIONS NO)

enable_warnings(pipe_client)
setup_debug_info(pipe_client)

if(MSVC)
    target_link_libraries(pipe_client ws2_32)
endif()
