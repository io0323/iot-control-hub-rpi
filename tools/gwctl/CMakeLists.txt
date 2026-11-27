add_executable(gwctl main.cpp)
target_link_libraries(gwctl PRIVATE nlohmann_json::nlohmann_json gw_options)
