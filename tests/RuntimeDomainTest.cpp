#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeDomain.hpp>

int main() {
    {
        iobject::RuntimeDomain domain;
        iobject::IRuntimeObject* anchor = domain.RootAnchor();
        TEST_CHECK(anchor != nullptr);
        TEST_CHECK(&domain.BridgeRoot() != nullptr);

        // 业务对象接入根锚点子树后可沿拓扑查询。
        iobject::IRuntimeObject* child = iobject::Runtime::make();
        TEST_CHECK(anchor->Connect("Player", child));
        TEST_CHECK(anchor->GetChildItem("Player") == child);
        delete child;
    }
    // 域析构后进程正常退出即说明根锚点销毁无误。
    return 0;
}
