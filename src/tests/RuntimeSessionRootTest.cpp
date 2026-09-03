#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <memory>

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* player = iobject::Runtime::make();
    TEST_CHECK(domain.RootAnchor()->Connect("Player", player));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle root = session->RootObject();
    TEST_CHECK(root != 0);
    TEST_CHECK(session->HasObject(root));
    TEST_CHECK(!session->HasObject(999999));

    // 根 addr 可以直接发现子对象。
    const iobject::RemoteObjectHandle playerHandle = session->ResolveChild(root, "Player");
    TEST_CHECK(playerHandle != 0);
    TEST_CHECK(session->HasObject(playerHandle));

    session->Close();
    TEST_CHECK(session->RootObject() == 0);
    TEST_CHECK(!session->HasObject(root));

    delete player;
    return 0;
}
