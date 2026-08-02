#include <iobject/Runtime.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

struct Tracked {
    static inline int nextId = 1;

    explicit Tracked(std::string name) : id(nextId++), name(std::move(name)) {
        std::cout << "  [被跟踪对象 #" << id << "] 构造：" << this->name << '\n';
    }

    ~Tracked() {
        std::cout << "  [被跟踪对象 #" << id << "] 析构：" << name << '\n';
    }

    int id;
    std::string name;
};

void printCase(const char* title, const char* expected) {
    std::cout << "\n=== " << title << " ===\n";
    std::cout << "预期：" << expected << "\n";
}

} // namespace

int main() {
    using iobject::Runtime;

    std::cout << "Runtime 原生对象包装与生命周期演示\n";

    printCase("1. make() 创建纯运行时节点",
              "节点不承载原生对象；调用方 delete 节点。");
    {
        iobject::IRuntimeObject* node = Runtime::make();
        std::cout << "  已创建纯运行时节点：" << node << '\n';
        delete node;
    }

    printCase("2. make<Tracked>(参数) 的值拥有",
              "节点持有新建对象；调用方 delete 节点时对象析构。");
    {
        iobject::IRuntimeObject* object = Runtime::make<Tracked>("按值创建的对象");
        std::cout << "  包装节点：" << object << '\n';
        delete object;
    }

    printCase("3. ref(T&) 的借用语义",
              "节点不拥有原对象；原对象由调用方作用域决定生命周期。");
    {
        Tracked original("被借用的原对象");
        iobject::IRuntimeObject* object = Runtime::ref(original);
        std::cout << "  包装节点：" << object << "，原对象仍由当前作用域管理。\n";
        delete object;
        std::cout << "  包装节点已删除，原对象仍存活。\n";
    }

    printCase("4. share(shared_ptr<T>) 的共享持有",
              "节点与调用方共享同一个控制块。");
    {
        std::shared_ptr<Tracked> shared = std::make_shared<Tracked>("共享对象");
        std::cout << "  包装前 use_count：" << shared.use_count() << '\n';
        iobject::IRuntimeObject* object = Runtime::share(shared);
        std::cout << "  包装期间 use_count：" << shared.use_count() << '\n';
        delete object;
        std::cout << "  节点删除后 use_count：" << shared.use_count() << '\n';
    }

    printCase("5. fromPtr(raw, false) 的非拥有裸指针",
              "节点不会删除裸指针；调用方依次删除节点和原对象。");
    {
        Tracked* raw = new Tracked("手动管理的裸指针对象");
        iobject::IRuntimeObject* object = Runtime::fromPtr(raw, false);
        std::cout << "  包装节点：" << object << "，不会拥有 raw。\n";
        delete object;
        std::cout << "  现在由调用方手动删除 raw。\n";
        delete raw;
    }

    printCase("6. fromPtr(raw, true) 的托管裸指针",
              "节点持有裸指针；调用方 delete 节点时自动删除它。");
    {
        Tracked* raw = new Tracked("由节点托管的裸指针对象");
        iobject::IRuntimeObject* object = Runtime::fromPtr(raw, true);
        std::cout << "  包装节点：" << object << "，将管理 raw。\n";
        delete object;
    }

    std::cout << "\n演示结束。\n";
    return 0;
}
