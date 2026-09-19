add_executable(
    Tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_marshal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_link_recovery.cpp)
target_include_directories(
    Tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Client/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Client/include/private)
target_link_libraries(Tests PRIVATE weyvelength_shared)

include(CTest)
add_test(NAME Tests COMMAND Tests)
