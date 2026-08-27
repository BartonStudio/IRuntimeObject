#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

// 业务对象：暴露命名方法（命令/动作）。args/result 均为不透明字节。
class Service final {
public:
    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        if (method == "Echo") {
            result(args);  // 回显参数
            return true;
        }
        if (method == "Ping") {
            result(iobject::ByteView{});  // 无返回值：空字节
            return true;
        }
        return false;  // 未知方法
    }
};

} // namespace

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* service = iobject::Runtime::make<Service>();
    TEST_CHECK(domain.RootAnchor()->Connect("Service", service));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle handle = session->ResolveRootChild("Service");
    TEST_CHECK(handle != 0);

    // 带参数、带返回值：Echo 原样回显。
    const std::array<std::uint8_t, 3> args{1, 2, 3};
    std::vector<std::uint8_t> echoed;
    std::size_t echoCalls = 0;
    TEST_CHECK(session->Invoke(handle, "Echo", args, [&](iobject::ByteView bytes) {
        ++echoCalls;
        echoed.assign(bytes.begin(), bytes.end());
    }));
    TEST_CHECK(echoCalls == 1);
    TEST_CHECK(echoed.size() == 3);
    TEST_CHECK(echoed[0] == 1 && echoed[1] == 2 && echoed[2] == 3);

    // 无返回值：receiver 仍恰好一次回调，空字节。
    std::size_t pingCalls = 0;
    TEST_CHECK(session->Invoke(handle, "Ping", {}, [&](iobject::ByteView bytes) {
        ++pingCalls;
        TEST_CHECK(bytes.empty());
    }));
    TEST_CHECK(pingCalls == 1);

    // 未知方法、空方法、无效句柄均失败。
    TEST_CHECK(!session->Invoke(handle, "Missing", {}, [](iobject::ByteView) {}));
    TEST_CHECK(!session->Invoke(handle, "", {}, [](iobject::ByteView) {}));
    TEST_CHECK(!session->Invoke(9999, "Echo", args, [](iobject::ByteView) {}));

    // 关闭会话后失败。
    session->Close();
    TEST_CHECK(!session->Invoke(handle, "Echo", args, [](iobject::ByteView) {}));

    delete service;
    return 0;
}
