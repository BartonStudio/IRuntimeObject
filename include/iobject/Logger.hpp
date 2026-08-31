#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <iobject/IRuntimeObject.hpp>

namespace iobject {

enum class LogLevel { Trace, Debug, Info, Warning, Error };

/// 运行时内置日志组件。
/// 核心职责只有日志输出；鸭子类型钩子（BindRuntime / ReadData / WriteData / Invoke）
/// 让它能作为运行时对象被本地或远程调用。
/// 可单独包装成节点，也可组合进自定义根节点（转发这些钩子即可）。
class Logger {
public:
    /// 标记：Invoke/ReadData/WriteData 内部已线程安全，框架跳过线程亲和断言。
    static constexpr bool kThreadSafe = true;

    Logger() = default;
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // =========================================================================
    // 核心：日志级别（运行时可调阈值，默认 Info；低于阈值的日志被丢弃）
    // =========================================================================
    void SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    // =========================================================================
    // 核心：日志输出
    // =========================================================================
    void Log(LogLevel level, const std::string& tag, const std::string& message);

    class Stream {
    public:
        Stream(Logger* logger, LogLevel level, std::string tag);
        Stream(Stream&& other) noexcept;
        ~Stream();

        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;

        template <typename T>
        Stream& operator<<(const T& value) {
            m_buf << value;
            return *this;
        }

    private:
        Logger* m_logger;
        LogLevel m_level;
        std::string m_tag;
        std::ostringstream m_buf;
    };

    Stream Trace(const std::string& tag);
    Stream Debug(const std::string& tag);
    Stream Info(const std::string& tag);
    Stream Warn(const std::string& tag);
    Stream Error(const std::string& tag);

    // =========================================================================
    // IObject 集成：鸭子类型钩子
    //   状态走 data channel，动作走 Invoke：
    //   - 通道 "Level"：WriteData / ReadData，1 字节（0=Trace .. 4=Error）
    //   - 方法 "Log"：Invoke，args 为结构化编码（见 EncodeLogMessage）
    //   - BindRuntime：框架在节点构造/析构时回调（记录自身节点）
    // =========================================================================
    void BindRuntime(IRuntimeObject* self);

    bool WriteData(DataChannelView channel, ByteInput data);
    bool ReadData(DataChannelView channel, DataReceiver receiver) const;
    bool Invoke(MethodView method, ByteInput args, DataReceiver result);

private:
    // ---- 核心状态 ----
    std::atomic<LogLevel> m_level{LogLevel::Info};
    std::mutex m_mutex;

    // ---- IObject 集成状态 ----
    IRuntimeObject* m_self = nullptr;
};

// =============================================================================
// 协议辅助："Log" 方法的消息编码/解码
//   格式：[1 字节级别][4 字节 tag 长度(大端)][tag][4 字节 msg 长度(大端)][msg]
// =============================================================================
std::vector<std::uint8_t> EncodeLogMessage(LogLevel level, std::string_view tag, std::string_view msg);
bool DecodeLogMessage(ByteView data, LogLevel& level, std::string& tag, std::string& msg);

} // namespace iobject

// =============================================================================
// 便捷宏：把日志编码并调用 obj->Invoke("Log", ...)
//   obj 可为 Logger* 或 IRuntimeObject*（logger 节点）
//   示例：IOBJECT_LOG(root->GetChildItem("Logger"), iobject::LogLevel::Info, "App", "hello");
// =============================================================================
#define IOBJECT_LOG(obj, level, tag, msg) \
    (obj)->Invoke("Log", iobject::EncodeLogMessage((level), (tag), (msg)), [](iobject::ByteView) {})
