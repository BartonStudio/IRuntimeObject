#include <iobject/Runtime.hpp>

#include <iostream>
#include <string>

namespace {

class ICharacter {
public:
    virtual ~ICharacter() = default;
    virtual const char* GetName() const = 0;
};

class IWeapon {
public:
    virtual ~IWeapon() = default;
    virtual int GetDamage() const = 0;
};

class IEquipment {
public:
    virtual ~IEquipment() = default;
    virtual const char* GetEquipmentName() const = 0;
};

class Sword final : public IWeapon, public IEquipment {
public:
    explicit Sword(int damage) : damage_(damage) {}

    int GetDamage() const override {
        return damage_;
    }

    const char* GetEquipmentName() const override {
        return "长剑";
    }

private:
    int damage_;
};

class Shield final : public IEquipment {
public:
    const char* GetEquipmentName() const override {
        return "盾牌";
    }
};

class Hero final : public ICharacter {
public:
    Hero(std::string name, IEquipment* equipment)
        : name_(std::move(name)), weapon_(25), equipment_(equipment) {}

    const char* GetName() const override {
        return name_.c_str();
    }

    static void RegisterTypes(iobject::TypeBuilder<Hero>& types) {
        types.As<ICharacter>();
        types.As<IWeapon>(&Hero::weapon_);
        types.As<IEquipment>([](const Hero& hero) -> IEquipment* {
            return hero.equipment_;
        });
    }

private:
    std::string name_;
    Sword weapon_;
    IEquipment* equipment_ = nullptr;
};

class Counter {
public:
    explicit Counter(int value) : value(value) {}

    int value;
};

void PrintResult(const char* title, bool success) {
    std::cout << "  " << title << "：" << (success ? "成功" : "失败") << '\n';
}

} // namespace

int main() {
    using iobject::IRuntimeObject;
    using iobject::Runtime;

    std::cout << "IRuntimeObject As<T> 类型转换演示\n";

    Shield shield;
    IRuntimeObject* heroObject = Runtime::make<Hero>("艾琳", &shield);

    std::cout << "\n1. 默认支持原包装类型\n";
    Hero* hero = heroObject->As<Hero>();
    PrintResult("As<Hero>()", hero != nullptr);

    std::cout << "\n2. RegisterTypes 中登记的公开基类\n";
    ICharacter* character = heroObject->As<ICharacter>();
    PrintResult("As<ICharacter>()", character != nullptr);
    if (character != nullptr) {
        std::cout << "  角色名称：" << character->GetName() << '\n';
    }

    std::cout << "\n3. 经由组合成员的转换规则\n";
    IWeapon* weapon = heroObject->As<IWeapon>();
    PrintResult("As<IWeapon>()", weapon != nullptr);
    if (weapon != nullptr) {
        std::cout << "  武器伤害：" << weapon->GetDamage() << '\n';
    }

    std::cout << "\n4. 经由自定义逻辑的转换规则\n";
    IEquipment* equipment = heroObject->As<IEquipment>();
    PrintResult("As<IEquipment>()", equipment != nullptr);
    if (equipment != nullptr) {
        std::cout << "  聚合装备：" << equipment->GetEquipmentName() << '\n';
    }

    std::cout << "\n5. 未登记的目标类型\n";
    PrintResult("As<Shield>()", heroObject->As<Shield>() != nullptr);

    std::cout << "\n6. const IRuntimeObject 查询\n";
    const IRuntimeObject* constHeroObject = heroObject;
    const ICharacter* constCharacter = constHeroObject->As<ICharacter>();
    PrintResult("const As<ICharacter>()", constCharacter != nullptr);

    std::cout << "\n7. Release 后不再暴露原生对象\n";
    heroObject->Release();
    PrintResult("Release 后 As<Hero>()", heroObject->As<Hero>() != nullptr);
    delete heroObject;

    std::cout << "\n8. 未声明 RegisterTypes 的普通类型\n";
    IRuntimeObject* counterObject = Runtime::make<Counter>(42);
    Counter* counter = counterObject->As<Counter>();
    PrintResult("As<Counter>()", counter != nullptr);
    if (counter != nullptr) {
        std::cout << "  计数值：" << counter->value << '\n';
    }
    delete counterObject;

    std::cout << "\n演示结束。\n";
    return 0;
}
