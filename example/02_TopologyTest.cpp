#include <iobject/Runtime.hpp>

#include <iostream>

namespace {

void printChildren(const iobject::IRuntimeObject& parent, const char* title) {
    iobject::RuntimeChildList children = parent.GetChildren();
    std::cout << "  " << title << "：" << children.size() << " 个\n";
    for (const iobject::RuntimeChildView& child : children) {
        std::cout << "    名称=" << child.name << "，节点=" << child.object << '\n';
    }
}

} // namespace

int main() {
    using iobject::Runtime;

    std::cout << "Runtime 节点拓扑演示\n";

    std::cout << "\n=== 1. 连接、查询、重名失败、覆盖与断开 ===\n";
    {
        iobject::IRuntimeObject* root = Runtime::make();
        iobject::IRuntimeObject* first = Runtime::make();
        iobject::IRuntimeObject* replacement = Runtime::make();

        bool connected = root->Connect("slot", first);
        std::cout << "  首次连接结果：" << connected << '\n';
        printChildren(*root, "首次连接后的子节点");
        std::cout << "  按名称查询：" << root->GetChildItem("slot") << '\n';

        bool duplicate = root->Connect("slot", replacement);
        std::cout << "  重名连接结果（默认不覆盖）：" << duplicate << '\n';
        std::cout << "  重名失败后查询仍为：" << root->GetChildItem("slot") << '\n';

        bool overwritten = root->Connect("slot", replacement, true);
        std::cout << "  覆盖连接结果：" << overwritten << '\n';
        printChildren(*root, "覆盖后的子节点");

        bool disconnected = root->Disconnect("slot");
        std::cout << "  显式断开结果：" << disconnected << '\n';
        std::cout << "  显式断开后查询：" << root->GetChildItem("slot") << '\n';

        delete replacement;
        delete first;
        delete root;
    }

    std::cout << "\n=== 2. 点分相对路径查询与单层名称限制 ===\n";
    {
        iobject::IRuntimeObject* root = Runtime::make();
        iobject::IRuntimeObject* player = Runtime::make();
        iobject::IRuntimeObject* weapon = Runtime::make();
        iobject::IRuntimeObject* invalidChild = Runtime::make();

        std::cout << "  root 到 player：" << root->Connect("player", player)
                  << "，player 到 weapon：" << player->Connect("weapon", weapon) << '\n';
        std::cout << "  单层 player 查询：" << root->GetChildItem("player")
                  << "，player 节点：" << player << '\n';
        std::cout << "  多层 player.weapon 查询：" << root->GetChildItem("player.weapon")
                  << "，weapon 节点：" << weapon << '\n';
        std::cout << "  中间层不存在 player.missing.item："
                  << root->GetChildItem("player.missing.item") << '\n';
        std::cout << "  空路径：" << root->GetChildItem("")
                  << "，首点：" << root->GetChildItem(".player")
                  << "，尾点：" << root->GetChildItem("player.")
                  << "，连续点：" << root->GetChildItem("player..weapon") << '\n';
        std::cout << "  含点名称连接结果：" << root->Connect("player.weapon", invalidChild)
                  << "，空名称连接结果：" << root->Connect("", invalidChild) << '\n';

        delete invalidChild;
        delete weapon;
        delete player;
        delete root;
    }

    std::cout << "\n=== 3. A Release 解除全部入边和出边 ===\n";
    {
        iobject::IRuntimeObject* parentB = Runtime::make();
        iobject::IRuntimeObject* parentD = Runtime::make();
        iobject::IRuntimeObject* objectA = Runtime::make();
        iobject::IRuntimeObject* objectC = Runtime::make();

        bool bConnected = parentB->Connect("A", objectA);
        bool dConnected = parentD->Connect("A", objectA);
        bool cConnected = objectA->Connect("C", objectC);
        std::cout << "  B 到 A：" << bConnected << "，D 到 A：" << dConnected
                  << "，A 到 C：" << cConnected << '\n';

        objectA->Release();
        std::cout << "  Release 后 B 查询 A：" << parentB->GetChildItem("A") << '\n';
        std::cout << "  Release 后 D 查询 A：" << parentD->GetChildItem("A") << '\n';
        std::cout << "  Release 后 A 查询 C：" << objectA->GetChildItem("C") << '\n';
        std::cout << "  C 仍由调用方管理：" << objectC << '\n';

        delete objectA;
        delete objectC;
        delete parentD;
        delete parentB;
    }

    std::cout << "\n=== 4. 多父 DAG 环检测：失败不改旧边 ===\n";
    {
        iobject::IRuntimeObject* objectA = Runtime::make();
        iobject::IRuntimeObject* objectB = Runtime::make();
        iobject::IRuntimeObject* objectC = Runtime::make();
        iobject::IRuntimeObject* existingChild = Runtime::make();

        std::cout << "  A 到 B：" << objectA->Connect("B", objectB)
                  << "，B 到 C：" << objectB->Connect("C", objectC)
                  << "，C 到 old：" << objectC->Connect("old", existingChild) << '\n';
        std::cout << "  C 到 A（间接环）结果：" << objectC->Connect("A", objectA) << '\n';
        std::cout << "  A 到 A（自环）结果：" << objectA->Connect("self", objectA) << '\n';
        std::cout << "  C 覆盖 old 为 A（间接环）结果："
                  << objectC->Connect("old", objectA, true) << '\n';
        std::cout << "  C 的 old 仍为：" << objectC->GetChildItem("old")
                  << "，原 child：" << existingChild << '\n';

        delete existingChild;
        delete objectC;
        delete objectB;
        delete objectA;
    }

    std::cout << "\n演示结束。\n";
    return 0;
}
