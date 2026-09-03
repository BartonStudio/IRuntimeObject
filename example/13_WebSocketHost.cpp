// 13_WebSocketHost：内置 WebSocket 服务端的实测宿主。
// RuntimeDomain 构造即自动启动 WebSocket 服务端（默认 0.0.0.0:9002，domain "iobject"），
// 无需任何应用侧代码。这里再挂一个 Echo 业务对象，供远程客户端验证
// ReadData / Invoke / 事件主动推送（双向通信）。
#include <iobject/Executor.hpp>
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <cstdint>
#include <cstdio>

namespace {

// 业务对象：Echo（原样回显）、Notify（触发一次通道变更事件推送）、Version 通道。
class EchoService final {
public:
    void SetNode(iobject::IRuntimeObject* node) { node_ = node; }

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "Version") {
            return false;
        }
        static const std::uint8_t version[] = {'1', '.', '0'};
        receiver(iobject::ByteView(version, sizeof(version)));
        return true;
    }

    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        if (method == "Echo") {
            result(args);  // 原样回显
            return true;
        }
        if (method == "Notify") {
            // 服务端主动推送：向订阅者发布 DataChannelChanged("Version") 事件。
            if (node_ != nullptr) {
                node_->Publish(
                    iobject::RuntimeEventTypes::DataChannelChanged,
                    iobject::Runtime::make<iobject::DataChannelChangedEventData>("Version"),
                    true);
            }
            result(iobject::ByteView{});
            return true;
        }
        return false;
    }

private:
    iobject::IRuntimeObject* node_ = nullptr;
};

} // namespace

int main() {
    // 构造即启动内置 WebSocket 服务端（端口 9002，domain "iobject"）。
    iobject::RuntimeDomain domain;

    iobject::IRuntimeObject* echo = iobject::Runtime::make<EchoService>();
    echo->As<EchoService>()->SetNode(echo);
    domain.RootAnchor()->Connect("Echo", echo);

    std::puts("[ws-host] WebSocket listening on 9002 (domain=iobject), Echo attached. Ctrl+C to quit.");
    std::fflush(stdout);
    iobject::Run();
    return 0;
}
