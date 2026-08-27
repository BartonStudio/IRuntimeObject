// 11_InvokeTest：Invoke 命令原语与"命令 + 显式发布"的两通道模型，并追加异步业务演示。
// 与 10_MessagePackProtocolTest 相同，进程内回环驱动 RuntimeBridgePeer。
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using msgpack11::MsgPack;

// —— 极简事件循环回投：worker 线程把完成的任务安全地回投到循环线程执行。
// 注意：这是示例自带的实现；IObject 框架当前没有内置回投机制（见文末说明）。
class LoopPostBack final {
public:
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        ready_.notify_all();
    }

    // 循环线程调用：阻塞等待并执行一个回投任务。
    void runOne() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this]() { return !tasks_.empty(); });
        std::function<void()> task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
};

// 业务对象：命名方法（命令）。
// - 同步命令（Refresh）只改状态，不自动发布；发布由业务方显式完成。
// - 异步命令（RefreshAsync）把 I/O 丢给 worker，完成经回投 + 注入回调发布。
class WeatherService final {
public:
    explicit WeatherService(std::function<void(std::function<void()>)> postToLoop)
        : postToLoop_(std::move(postToLoop)) {}

    // 门面在节点创建后调用：把"发布 DataChannelChanged"接到节点上（供异步完成时触发）。
    void SetOnChanged(std::function<void()> onChanged) {
        onChanged_ = std::move(onChanged);
    }

    ~WeatherService() {
        if (worker_.joinable()) {
            worker_.join();  // 保证 worker 先于对象析构结束，避免悬垂。
        }
    }

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "Temperature") {
            return false;
        }
        const std::uint8_t value = temperature_;
        receiver(iobject::ByteView(&value, 1));
        return true;
    }

    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        if (method == "Echo") {
            result(args);  // 纯返回值：不涉及任何通道/事件
            return true;
        }
        if (method == "Refresh") {
            static_cast<void>(args);
            temperature_ = 25;
            result(iobject::ByteView{});  // 同步受理回执：不自动发布
            return true;
        }
        if (method == "RefreshAsync") {
            static_cast<void>(args);
            if (worker_.joinable()) {
                worker_.join();  // 简化：单飞，等待上一次完成。
            }
            worker_ = std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));  // 模拟网络 I/O
                const std::uint8_t fetched = 27;  // 在 worker 栈上算结果，不碰循环内状态
                postToLoop_([this, fetched]() {   // 回投：在循环线程提交 + 发布
                    temperature_ = fetched;
                    if (onChanged_) onChanged_();
                });
            });
            result(iobject::ByteView{});  // 受理回执：立即返回，不阻塞循环
            return true;
        }
        return false;
    }

private:
    std::function<void(std::function<void()>)> postToLoop_;
    std::function<void()> onChanged_;
    std::thread worker_;
    std::uint8_t temperature_ = 0;
};

MsgPack::binary toBinary(const char* text) {
    const std::size_t length = std::strlen(text);
    return MsgPack::binary(reinterpret_cast<const std::uint8_t*>(text),
                           reinterpret_cast<const std::uint8_t*>(text) + length);
}

std::string toText(const MsgPack& value) {
    if (!value.is_binary()) {
        return {};
    }
    const MsgPack::binary& bytes = value.binary_items();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {}

    MsgPack call(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        const std::string frame = MsgPack(std::move(object)).dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
        return lastFrame();
    }

    MsgPack lastFrame() {
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        if (!err.empty()) {
            std::printf("  解析帧失败: %s\n", err.c_str());
        }
        return frame;
    }
};

} // namespace

int main() {
    Loopback loop;

    // 1. 构造服务并注入"回投"能力；门面随后注入发布回调。
    LoopPostBack postBack;
    iobject::IRuntimeObject* weather = iobject::Runtime::make<WeatherService>(
        [&postBack](std::function<void()> task) { postBack.post(std::move(task)); });
    weather->As<WeatherService>()->SetOnChanged([weather]() {
        weather->Publish(
            iobject::RuntimeEventTypes::DataChannelChanged,
            iobject::Runtime::make<iobject::DataChannelChangedEventData>("Temperature"), true);
    });
    loop.domain.RootAnchor()->Connect("Weather", weather);

    // 2. 握手 + 发现 + 订阅。
    const MsgPack hello = loop.call({{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}});
    const std::uint64_t root = static_cast<std::uint64_t>(hello["root"].int64_value());
    const MsgPack found = loop.call(
        {{"op", MsgPack("GetChildItem")}, {"addr", MsgPack(root)}, {"childId", MsgPack("Weather")}});
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    const MsgPack sub = loop.call({{"op", MsgPack("SubscribeEvent")},
                                   {"addr", MsgPack(addr)},
                                   {"type", MsgPack("DataChannelChanged")}});
    const std::uint64_t subscription =
        static_cast<std::uint64_t>(sub["subscription"].int64_value());
    std::printf("1. 发现 Weather addr=%llu，订阅 DataChannelChanged=%llu\n",
                static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(subscription));

    // 3. 纯返回值命令（同步，不涉及通道/事件）。
    const MsgPack echoed = loop.call({{"op", MsgPack("Invoke")},
                                      {"addr", MsgPack(addr)},
                                      {"method", MsgPack("Echo")},
                                      {"args", MsgPack(toBinary("hello"))}});
    std::printf("2. Invoke Echo -> \"%s\"（纯返回值）\n", toText(echoed["result"]).c_str());

    // 4. 同步命令 + 业务方显式发布：Invoke 只改状态、返回受理回执，不自动发布。
    const MsgPack refreshed = loop.call({{"op", MsgPack("Invoke")},
                                         {"addr", MsgPack(addr)},
                                         {"method", MsgPack("Refresh")},
                                         {"args", MsgPack(MsgPack::binary{})}});
    std::printf("3. Invoke Refresh ok=%d（受理回执，未自动发布）\n",
                refreshed["ok"].bool_value() ? 1 : 0);
    // 业务方（持有节点指针）显式发布状态变化；远程收到事件 + 字节快照。
    weather->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                     iobject::Runtime::make<iobject::DataChannelChangedEventData>("Temperature"),
                     true);
    const MsgPack syncEvent = loop.lastFrame();
    std::printf("4. 业务方显式发布 → 收到事件 event=%s channel=%s data=Temperature=%u（命令与发布分离）\n",
                syncEvent["event"].string_value().c_str(),
                syncEvent["channel"].string_value().c_str(),
                static_cast<unsigned>(syncEvent["data"].binary_items()[0]));

    // 5. 异步命令：Invoke RefreshAsync 立即返回受理回执，不阻塞循环。
    const MsgPack asyncAccepted = loop.call({{"op", MsgPack("Invoke")},
                                             {"addr", MsgPack(addr)},
                                             {"method", MsgPack("RefreshAsync")},
                                             {"args", MsgPack(MsgPack::binary{})}});
    std::printf("5. Invoke RefreshAsync ok=%d（立即受理，worker 在后台模拟 I/O）\n",
                asyncAccepted["ok"].bool_value() ? 1 : 0);

    // 6. 异步窗口内，循环仍可服务其它请求（证明未被阻塞）。
    const MsgPack busy = loop.call({{"op", MsgPack("Invoke")},
                                    {"addr", MsgPack(addr)},
                                    {"method", MsgPack("Echo")},
                                    {"args", MsgPack(toBinary("busy"))}});
    std::printf("6. 异步窗口内 Invoke Echo -> \"%s\"（循环未被阻塞）\n",
                toText(busy["result"]).c_str());

    // 7. 回投：worker 完成后，循环线程执行提交 + 注入回调发布。
    postBack.runOne();
    const MsgPack asyncEvent = loop.lastFrame();
    std::printf("7. 异步完成 event=%s data=Temperature=%u（worker 经回投在循环线程发布）\n",
                asyncEvent["event"].string_value().c_str(),
                static_cast<unsigned>(asyncEvent["data"].binary_items()[0]));

    // 8. 读回最终温度，验证异步提交已落盘。
    const MsgPack read = loop.call({{"op", MsgPack("ReadData")},
                                    {"addr", MsgPack(addr)},
                                    {"channel", MsgPack("Temperature")}});
    std::printf("8. ReadData Temperature -> %u（异步结果已提交）\n",
                static_cast<unsigned>(read["data"].binary_items()[0]));

    // 9. 未知方法回 OperationFailed。
    const MsgPack missing = loop.call({{"op", MsgPack("Invoke")},
                                       {"addr", MsgPack(addr)},
                                       {"method", MsgPack("Missing")},
                                       {"args", MsgPack(MsgPack::binary{})}});
    std::printf("9. Invoke Missing -> %s\n", missing["error"]["code"].string_value().c_str());

    loop.call({{"op", MsgPack("CancelEvent")}, {"subscription", MsgPack(subscription)}});
    loop.call({{"op", MsgPack("Close")}});
    std::printf("10. Close ok\n");

    delete weather;
    return 0;
}
