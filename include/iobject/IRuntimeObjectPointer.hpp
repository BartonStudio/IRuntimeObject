#pragma once

#include "IRuntimeObject.hpp"

namespace iobject {

/// 可换绑的透明非拥有指针节点；除绑定管理外，其余操作转发给当前绑定目标。
class IRuntimeObjectPointer : public IRuntimeObject {
public:
    /// 绑定普通活动运行时节点；nullptr 等价于 Unbind。
    virtual bool Bind(IRuntimeObject* object) = 0;
    /// 解除当前绑定，不影响原目标的生命周期或关系。
    virtual void Unbind() noexcept = 0;
    /// 返回当前绑定的普通运行时节点；未绑定或节点已 Release 时返回 nullptr。
    virtual IRuntimeObject* GetBindObject() noexcept = 0;
    virtual const IRuntimeObject* GetBindObject() const noexcept = 0;
    virtual bool IsBound() const noexcept = 0;
};

} // namespace iobject
