#pragma once
#include <algorithm>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ─── FIX field tags ────────────────────────────────────────────────────────
namespace tag {
constexpr int BeginString    =  8;
constexpr int BodyLength     =  9;
constexpr int MsgType        = 35;
constexpr int SenderCompID   = 49;
constexpr int TargetCompID   = 56;
constexpr int MsgSeqNum      = 34;
constexpr int SendingTime    = 52;
constexpr int HeartBtInt     = 108;
constexpr int EncryptMethod  = 98;
constexpr int TestReqID      = 112;
constexpr int CheckSum       = 10;
} // namespace tag

// ─── FIX message type mnemonics ────────────────────────────────────────────
namespace msgtype {
constexpr const char* Logon     = "A";
constexpr const char* Heartbeat = "0";
constexpr const char* Logout    = "5";
} // namespace msgtype

// SOH delimiter used between fields
static const char SOH = '\x01';

// ─── FIX message representation ────────────────────────────────────────────
class FixMessage {
public:
    // Ordered list of (tag, value) pairs so we can rebuild raw bytes faithfully
    using Fields = std::vector<std::pair<int, std::string>>;

    // ── Builders ──────────────────────────────────────────────────────────
    void set(int t, const std::string& v) {
        // Update existing or append
        for (auto& f : fields_) {
            if (f.first == t) { f.second = v; return; }
        }
        fields_.push_back({t, v});
    }
    void set(int t, int v)        { set(t, std::to_string(v)); }

    std::string get(int t, const std::string& def = "") const {
        for (const auto& f : fields_)
            if (f.first == t) return f.second;
        return def;
    }

    bool has(int t) const {
        return std::any_of(fields_.begin(), fields_.end(),
                           [t](const auto& f){ return f.first == t; });
    }

    // Serialise to a raw FIX string (calculates BodyLength + CheckSum)
    std::string serialize() const {
        // Build body (everything except BeginString, BodyLength, CheckSum)
        std::string body;
        for (const auto& f : fields_) {
            if (f.first == tag::BeginString ||
                f.first == tag::BodyLength  ||
                f.first == tag::CheckSum)   continue;
            body += std::to_string(f.first) + '=' + f.second + SOH;
        }

        std::string header = std::to_string(tag::BeginString) + '=' +
                             get(tag::BeginString, "FIX.4.4") + SOH +
                             std::to_string(tag::BodyLength) + '=' +
                             std::to_string(body.size()) + SOH;

        std::string raw = header + body;

        // CheckSum = sum of all bytes mod 256, formatted as 3-digit string
        int cs = 0;
        for (unsigned char c : raw) cs += c;
        cs %= 256;
        char csbuf[4];
        snprintf(csbuf, sizeof(csbuf), "%03d", cs);
        raw += std::to_string(tag::CheckSum) + '=' + csbuf + SOH;
        return raw;
    }

    // Parse a raw FIX string into a FixMessage (permissive: ignores checksum)
    static FixMessage parse(const std::string& raw) {
        FixMessage msg;
        std::string token;
        for (char c : raw) {
            if (c == SOH) {
                if (!token.empty()) {
                    auto eq = token.find('=');
                    if (eq != std::string::npos) {
                        int t = std::stoi(token.substr(0, eq));
                        std::string v = token.substr(eq + 1);
                        msg.fields_.push_back({t, v});
                    }
                    token.clear();
                }
            } else {
                token += c;
            }
        }
        return msg;
    }

    const Fields& fields() const { return fields_; }

    // Pretty-print for logging
    std::string pretty() const {
        std::string out;
        for (const auto& f : fields_) {
            out += std::to_string(f.first) + '=' + f.second + " | ";
        }
        return out;
    }

private:
    Fields fields_;
};

// ─── Helpers ───────────────────────────────────────────────────────────────

// Returns current UTC time as FIX SendingTime (YYYYMMDD-HH:MM:SS)
inline std::string fixTimestamp() {
    time_t now = time(nullptr);
    struct tm* t = gmtime(&now);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", t);
    return buf;
}

// Build a Logon message
inline FixMessage buildLogon(const std::string& sender,
                             const std::string& target,
                             int seqNum,
                             int heartBtInt = 30) {
    FixMessage m;
    m.set(tag::BeginString,   "FIX.4.4");
    m.set(tag::MsgType,       msgtype::Logon);
    m.set(tag::SenderCompID,  sender);
    m.set(tag::TargetCompID,  target);
    m.set(tag::MsgSeqNum,     seqNum);
    m.set(tag::SendingTime,   fixTimestamp());
    m.set(tag::EncryptMethod, 0);
    m.set(tag::HeartBtInt,    heartBtInt);
    return m;
}

// Build a Heartbeat message
inline FixMessage buildHeartbeat(const std::string& sender,
                                 const std::string& target,
                                 int seqNum,
                                 const std::string& testReqID = "") {
    FixMessage m;
    m.set(tag::BeginString,  "FIX.4.4");
    m.set(tag::MsgType,      msgtype::Heartbeat);
    m.set(tag::SenderCompID, sender);
    m.set(tag::TargetCompID, target);
    m.set(tag::MsgSeqNum,    seqNum);
    m.set(tag::SendingTime,  fixTimestamp());
    if (!testReqID.empty()) m.set(tag::TestReqID, testReqID);
    return m;
}
