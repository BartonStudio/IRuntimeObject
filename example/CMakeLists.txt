add_executable(test_01_wrapping 01_WrappingTest.cpp)
target_link_libraries(test_01_wrapping PRIVATE IObject::IObject)

add_executable(test_02_topology 02_TopologyTest.cpp)
target_link_libraries(test_02_topology PRIVATE IObject::IObject)

add_executable(test_03_event 03_EventTest.cpp)
target_link_libraries(test_03_event PRIVATE IObject::IObject)

add_executable(test_04_type_conversion 04_TypeConversionTest.cpp)
target_link_libraries(test_04_type_conversion PRIVATE IObject::IObject)

add_executable(test_05_data_channel 05_DataChannelTest.cpp)
target_link_libraries(test_05_data_channel PRIVATE IObject::IObject)

add_executable(test_06_data_channel_change 06_DataChannelChangeTest.cpp)
target_link_libraries(test_06_data_channel_change PRIVATE IObject::IObject)

add_executable(test_07_pointer 07_PointerTest.cpp)
target_link_libraries(test_07_pointer PRIVATE IObject::IObject)

add_executable(test_08_subscribe_channel 08_SubscribeChannelTest.cpp)
target_link_libraries(test_08_subscribe_channel PRIVATE IObject::IObject)

add_executable(test_09_remote_bridge 09_RemoteBridgeTest.cpp)
target_link_libraries(test_09_remote_bridge PRIVATE IObject::IObject)

add_executable(test_10_messagepack_protocol 10_MessagePackProtocolTest.cpp)
target_link_libraries(test_10_messagepack_protocol PRIVATE IObject::IObject)
target_include_directories(test_10_messagepack_protocol PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)

add_executable(test_11_invoke 11_InvokeTest.cpp)
target_link_libraries(test_11_invoke PRIVATE IObject::IObject)
target_include_directories(test_11_invoke PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)

add_executable(test_12_executor 12_ExecutorTest.cpp)
target_link_libraries(test_12_executor PRIVATE IObject::IObject)
