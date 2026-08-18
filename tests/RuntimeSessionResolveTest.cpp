#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <memory>

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* player = iobject::Runtime::make();
    iobject::IRuntimeObject* decoder = iobject::Runtime::make();
    TEST_CHECK(domain.RootAnchor()->Connect("Player", player));
    TEST_CHECK(player->Connect("Decoder", decoder));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    TEST_CHECK(session != nullptr);
    TEST_CHECK(session->IsOpen());

    // 单层与多级路径解析。
    const iobject::RemoteObjectHandle playerHandle = session->ResolveRootChild("Player");
    TEST_CHECK(playerHandle != 0);
    const iobject::RemoteObjectHandle decoderHandle = session->ResolveRootChild("Player.Decoder");
    TEST_CHECK(decoderHandle != 0);
    TEST_CHECK(decoderHandle != playerHandle);

    // 同一对象经不同路径到达返回同一句柄。
    const iobject::RemoteObjectHandle decoderAgain = session->ResolveChild(playerHandle, "Decoder");
    TEST_CHECK(decoderAgain == decoderHandle);

    // 未命中路径与无效句柄返回 0。
    TEST_CHECK(session->ResolveRootChild("Missing") == 0);
    TEST_CHECK(session->ResolveRootChild("Player.Missing") == 0);
    TEST_CHECK(session->ResolveRootChild("") == 0);
    TEST_CHECK(session->ResolveChild(9999, "Decoder") == 0);

    // 不同会话的句柄互相独立。
    std::unique_ptr<iobject::RuntimeSession> other = domain.BridgeRoot().OpenSession();
    TEST_CHECK(other != nullptr);
    TEST_CHECK(other->ResolveChild(playerHandle, "Decoder") == 0);

    session->Close();
    TEST_CHECK(!session->IsOpen());
    TEST_CHECK(session->ResolveRootChild("Player") == 0);

    delete decoder;
    delete player;
    return 0;
}
