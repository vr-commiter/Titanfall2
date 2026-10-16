#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tg {

// Protocol serialization is kept separate for offline verification.
std::string TrueGearRequest(std::string_view method, std::string_view content,
    std::string_view request_id);
bool IsTrueGearEventName(std::string_view value);

struct TrueGearResponse {
    std::string method;
    std::string request_id;
    std::string result;
};

// Call public methods on the native output worker only. WinHTTP completion
// callbacks retain their own state until the last handle notification.
class TrueGearTransport {
public:
    TrueGearTransport();
    ~TrueGearTransport();
    TrueGearTransport(const TrueGearTransport&) = delete;
    TrueGearTransport& operator=(const TrueGearTransport&) = delete;

    // True means local WebSocket connected and registration request sent.
    // Neither this nor Connected() establishes physical device availability.
    bool Connect(const std::string& app_id, const std::string& api_key);
    bool Register();
    bool PlayByUuid(const std::string& effect_uuid);
    bool PlayContent(const std::string& effect_json);
    bool SeekByUuid(const std::string& effect_uuid);
    bool Stop(const std::string& effect_uuid);
    bool StopAll();
    bool Connected();
    std::vector<TrueGearResponse> DrainResponses();
    // Normal worker shutdown waits at most 500 ms for the peer close frame so
    // earlier stop messages are not discarded by immediate cancellation.
    void Disconnect();
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tg
