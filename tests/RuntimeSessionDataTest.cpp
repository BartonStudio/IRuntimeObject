#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace {

class Device final {
public:
    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "State") {
            return false;
        }
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(state_ & 0xFF),
            static_cast<std::uint8_t>((state_ >> 8) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 16) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 24) & 0xFF)};
        receiver(bytes);
        return true;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (channel != "State" || data.size() != 4) {
            return false;
        }
        state_ = static_cast<std::uint32_t>(data[0])
               | (static_cast<std::uint32_t>(data[1]) << 8)
               | (static_cast<std::uint32_t>(data[2]) << 16)
               | (static_cast<std::uint32_t>(data[3]) << 24);
        return true;
    }

private:
    std::uint32_t state_ = 0;
};

} // namespace

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* device = iobject::Runtime::make<Device>();
    TEST_CHECK(domain.RootAnchor()->Connect("Device", device));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle handle = session->ResolveRootChild("Device");
    TEST_CHECK(handle != 0);

    // 经会话写入，再经会话读回。
    const std::array<std::uint8_t, 4> input{0x2C, 0x01, 0x00, 0x00};  // 300
    TEST_CHECK(session->WriteData(handle, "State", input));
    std::uint32_t readBack = 0;
    std::size_t receiverCalls = 0;
    TEST_CHECK(session->ReadData(handle, "State", [&](iobject::ByteView bytes) {
        ++receiverCalls;
        TEST_CHECK(bytes.size() == 4);
        readBack = static_cast<std::uint32_t>(bytes[0])
                 | (static_cast<std::uint32_t>(bytes[1]) << 8)
                 | (static_cast<std::uint32_t>(bytes[2]) << 16)
                 | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }));
    TEST_CHECK(receiverCalls == 1);
    TEST_CHECK(readBack == 300);

    // 未知通道、无效句柄均失败。
    TEST_CHECK(!session->ReadData(handle, "Missing", [](iobject::ByteView) {}));
    TEST_CHECK(!session->WriteData(9999, "State", input));

    session->Close();
    TEST_CHECK(!session->WriteData(handle, "State", input));

    delete device;
    return 0;
}
