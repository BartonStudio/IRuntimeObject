add_executable(test_smoke SmokeTest.cpp)
target_link_libraries(test_smoke PRIVATE IObject::IObject)
add_test(NAME smoke COMMAND test_smoke)
