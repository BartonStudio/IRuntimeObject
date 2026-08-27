#include <iobject/Logger.hpp>

#include <cstdio>
#include <span>

namespace iobject {

namespace {

const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

void appendU32BigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

std::uint32_t readU32BigEndian(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

} // namespace

// =============================================================================
// 核心：日志级别
// =============================================================================
void Logger::SetLevel(LogLevel level) {
    m_level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() const {
    return m_level.load(std::memory_order_relaxed);
}

// =============================================================================
// 核心：日志输出实现
// =============================================================================
void Logger::Log(LogLevel level, const std::string& tag, const std::string& message) {
    if (level < m_level.load(std::memory_order_relaxed)) {
        return;  // 低于当前阈值，丢弃
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fprintf(stderr, "[%s] [%s] %s\n", levelToString(level), tag.c_str(), message.c_str());
}

Logger::Stream::Stream(Logger* logger, LogLevel level, std::string tag)
    : m_logger(logger), m_level(level), m_tag(std::move(tag)) {}

Logger::Stream::Stream(Stream&& other) noexcept
    : m_logger(other.m_logger),
      m_level(other.m_level),
      m_tag(std::move(other.m_tag)),
      m_buf(std::move(other.m_buf)) {
    other.m_logger = nullptr;
}

Logger::Stream::~Stream() {
    if (m_logger) {
        m_logger->Log(m_level, m_tag, m_buf.str());
    }
}

Logger::Stream Logger::Trace(const std::string& tag) { return Stream(this, LogLevel::Trace, tag); }
Logger::Stream Logger::Debug(const std::string& tag) { return Stream(this, LogLevel::Debug, tag); }
Logger::Stream Logger::Info(const std::string& tag) { return Stream(this, LogLevel::Info, tag); }
Logger::Stream Logger::Warn(const std::string& tag) { return Stream(this, LogLevel::Warning, tag); }
Logger::Stream Logger::Error(const std::string& tag) { return Stream(this, LogLevel::Error, tag); }

// =============================================================================
// IObject 集成实现：BindRuntime
// =============================================================================
void Logger::BindRuntime(IRuntimeObject* self) {
    // 框架在节点构造完成后回调；Logger 只记录自身节点，挂载由调用方负责。
    m_self = self;
}

// =============================================================================
// IObject 集成实现：状态（日志等级）走 data channel
// =============================================================================
bool Logger::WriteData(DataChannelView channel, ByteInput data) {
    if (channel == "Level") {
        if (data.size() < 1) {
            return false;
        }
        const std::uint8_t byte = data[0];
        if (byte > static_cast<std::uint8_t>(LogLevel::Error)) {
            return false;  // 越界级别
        }
        SetLevel(static_cast<LogLevel>(byte));
        return true;
    }
    return false;
}

bool Logger::ReadData(DataChannelView channel, DataReceiver receiver) const {
    if (channel == "Level") {
        const std::uint8_t byte = static_cast<std::uint8_t>(GetLevel());
        receiver(std::span<const std::uint8_t>(&byte, 1));
        return true;
    }
    return false;
}

// =============================================================================
// IObject 集成实现：动作（写一条日志）走 Invoke
// =============================================================================
bool Logger::Invoke(MethodView method, ByteInput args, DataReceiver result) {
    if (method == "Log") {
        LogLevel level = LogLevel::Info;
        std::string tag;
        std::string msg;
        if (!DecodeLogMessage(args, level, tag, msg)) {
            return false;
        }
        Log(level, tag, msg);
        result(ByteView{});  // 空返回表示无返回值
        return true;
    }
    return false;
}

// =============================================================================
// 协议辅助实现
// =============================================================================
std::vector<std::uint8_t> EncodeLogMessage(LogLevel level, std::string_view tag, std::string_view msg) {
    std::vector<std::uint8_t> out;
    out.reserve(1 + 4 + tag.size() + 4 + msg.size());
    out.push_back(static_cast<std::uint8_t>(level));
    appendU32BigEndian(out, static_cast<std::uint32_t>(tag.size()));
    out.insert(out.end(), tag.begin(), tag.end());
    appendU32BigEndian(out, static_cast<std::uint32_t>(msg.size()));
    out.insert(out.end(), msg.begin(), msg.end());
    return out;
}

bool DecodeLogMessage(ByteView data, LogLevel& level, std::string& tag, std::string& msg) {
    if (data.size() < 9) {
        return false;  // 至少 1 + 4 + 4 字节
    }

    const std::uint8_t levelByte = data[0];
    if (levelByte > static_cast<std::uint8_t>(LogLevel::Error)) {
        return false;  // 越界级别
    }
    level = static_cast<LogLevel>(levelByte);

    const std::uint32_t tagLen = readU32BigEndian(data.data() + 1);
    if (data.size() < 1 + 4 + tagLen + 4) {
        return false;
    }
    tag.assign(reinterpret_cast<const char*>(data.data() + 5), tagLen);

    const std::uint32_t msgLen = readU32BigEndian(data.data() + 5 + tagLen);
    if (data.size() < 1 + 4 + tagLen + 4 + msgLen) {
        return false;
    }
    msg.assign(reinterpret_cast<const char*>(data.data() + 9 + tagLen), msgLen);
    return true;
}

} // namespace iobject
