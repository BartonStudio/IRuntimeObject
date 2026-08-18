add_executable(test_smoke SmokeTest.cpp)
target_link_libraries(test_smoke PRIVATE IObject::IObject)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_runtime_domain RuntimeDomainTest.cpp)
target_link_libraries(test_runtime_domain PRIVATE IObject::IObject)
add_test(NAME runtime_domain COMMAND test_runtime_domain)
