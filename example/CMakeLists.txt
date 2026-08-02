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
