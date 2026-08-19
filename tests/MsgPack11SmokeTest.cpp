#include "TestCheck.hpp"

#include <msgpack11.hpp>

#include <cstdint>
#include <string>

int main() {
    using msgpack11::MsgPack;

    const MsgPack message = MsgPack(MsgPack::object{
        {"op", MsgPack("ReadData")},
        {"id", MsgPack(std::uint64_t(42))},
        {"data", MsgPack(MsgPack::binary{1, 2, 3, 250})}});
    const std::string frame = message.dump();

    std::string err;
    const MsgPack parsed = MsgPack::parse(frame, err);
    TEST_CHECK(err.empty());
    TEST_CHECK(parsed.is_object());
    TEST_CHECK(parsed["op"].string_value() == "ReadData");
    // 小整数会解析为窄无符号类型；int64_value 对任意整数类型可用。
    TEST_CHECK(parsed["id"].int64_value() == 42);
    TEST_CHECK(parsed["data"].is_binary());
    TEST_CHECK(parsed["data"].binary_items().size() == 4);
    TEST_CHECK(parsed["data"].binary_items()[3] == 250);
    return 0;
}
