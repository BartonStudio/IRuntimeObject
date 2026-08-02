#include <iobject/Runtime.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

class DeviceState {
public:
    DeviceState(std::int32_t health, std::string name)
        : health_(health), name_(std::move(name)) {}

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel == "State") {
            const std::array<std::uint8_t, 4> bytes = {
                static_cast<std::uint8_t>(health_ & 0xff),
                static_cast<std::uint8_t>((health_ >> 8) & 0xff),
                static_cast<std::uint8_t>((health_ >> 16) & 0xff),
                static_cast<std::uint8_t>((health_ >> 24) & 0xff),
            };
            receiver(iobject::ByteView(bytes.data(), bytes.size()));
            return true;
        }
        if (channel == "Empty") {
            receiver(iobject::ByteView());
            return true;
        }
        return false;
    }

    const std::string& GetName() const {
        return name_;
    }

private:
    std::int32_t health_ = 0;
    std::string name_;
};

class PlainObject {
public:
    explicit PlainObject(int value) : value_(value) {}

private:
    int value_ = 0;
};

void PrintBytes(iobject::ByteView bytes) {
    std::cout << "  收到字节（" << bytes.size() << " 字节）：";
    for (std::uint8_t byte : bytes) {
        std::cout << ' ' << static_cast<unsigned int>(byte);
    }
    std::cout << '\n';
}

void PrintResult(const char* title, bool success) {
    std::cout << "  " << title << "：" << (success ? "成功" : "失败") << '\n';
}

} // namespace

int main() {
    using iobject::IRuntimeObject;
    using iobject::Runtime;

    std::cout << "IRuntimeObject 只读数据通道演示\n";

    IRuntimeObject* deviceObject = Runtime::make<DeviceState>(275, "控制台");

    std::cout << "\n1. 原生类提供 State 通道\n";
    std::vector<std::uint8_t> copiedBytes;
    const bool readState = deviceObject->ReadData("State", [&copiedBytes](iobject::ByteView bytes) {
        copiedBytes.assign(bytes.begin(), bytes.end());
        PrintBytes(bytes);
    });
    PrintResult("读取 State", readState);
    std::cout << "  回调内已复制 " << copiedBytes.size() << " 个字节，未保留 ByteView。\n";

    std::cout << "\n2. 空数据仍是成功结果\n";
    const bool readEmpty = deviceObject->ReadData("Empty", [](iobject::ByteView bytes) {
        PrintBytes(bytes);
    });
    PrintResult("读取 Empty", readEmpty);

    std::cout << "\n3. 未知通道\n";
    const bool readUnknown = deviceObject->ReadData("Unknown", [](iobject::ByteView bytes) {
        PrintBytes(bytes);
    });
    PrintResult("读取 Unknown", readUnknown);

    std::cout << "\n4. 未实现 ReadData 的原生类\n";
    IRuntimeObject* plainObject = Runtime::make<PlainObject>(42);
    const bool readPlain = plainObject->ReadData("State", [](iobject::ByteView bytes) {
        PrintBytes(bytes);
    });
    PrintResult("读取 PlainObject", readPlain);

    std::cout << "\n5. 纯运行时节点\n";
    IRuntimeObject* runtimeNode = Runtime::make();
    const bool readNode = runtimeNode->ReadData("State", [](iobject::ByteView bytes) {
        PrintBytes(bytes);
    });
    PrintResult("读取纯运行时节点", readNode);

    std::cout << "\n6. Release 后不可读取\n";
    deviceObject->Release();
    const bool readReleased = deviceObject->ReadData("State", [](iobject::ByteView bytes) {
        PrintBytes(bytes);
    });
    PrintResult("Release 后读取 State", readReleased);

    delete runtimeNode;
    delete plainObject;
    delete deviceObject;

    std::cout << "\n演示结束。\n";
    return 0;
}
