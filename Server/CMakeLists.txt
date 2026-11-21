add_executable(
    Server
    ${CMAKE_CURRENT_SOURCE_DIR}/src/weyvelength_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_include_directories(
    Server
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Shared/thirdparty/spdlog)
target_link_libraries(Server PRIVATE weyvelength_shared)

if(MSVC)
    target_compile_options(Server PRIVATE /utf-8) # fmt 11 (bundled in spdlog) requires utf-8 literals
elseif(MINGW)
    target_compile_options(Server PRIVATE -Wa,-mbig-obj) # asio's coroutines blow past GNU as's section limit otherwise
endif()
