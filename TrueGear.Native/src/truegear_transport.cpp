#include "truegear_transport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>

namespace tg {
namespace {
constexpr auto operation_timeout = std::chrono::milliseconds(1200);
constexpr auto graceful_close_timeout = std::chrono::milliseconds(500);
constexpr size_t max_message_bytes = 64 * 1024;
#ifdef TrueGear_TRANSPORT_TEST_PORT
constexpr INTERNET_PORT service_port = TrueGear_TRANSPORT_TEST_PORT;
#else
constexpr INTERNET_PORT service_port = 18233;
#endif

std::string Quote(std::string_view value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}

std::string Base64(std::string_view content) {
    if (content.empty()) return {};
    if (content.size() > max_message_bytes) throw std::length_error("TrueGear request body exceeds limit");
    DWORD length = 0;
    const auto* data = reinterpret_cast<const BYTE*>(content.data());
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(content.size()), flags, nullptr, &length))
        throw std::runtime_error("TrueGear base64 sizing failed");
    std::string result(length, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(content.size()), flags, result.data(), &length))
        throw std::runtime_error("TrueGear base64 conversion failed");
    result.resize(length); // Conversion reports the count excluding its final NUL.
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::string DecodeBase64(const std::string& content) {
    if (content.empty()) return {};
    if (content.size() > max_message_bytes) throw std::length_error("TrueGear response exceeds limit");
    // Reject non-alphabet content rather than allowing the Windows decoder to
    // skip an arbitrary header or trailing characters.
    if (content.size() % 4) throw std::invalid_argument("Invalid TrueGear response base64 length");
    size_t padding = 0;
    for (unsigned char c : content) {
        if (c == '=') { if (++padding > 2) throw std::invalid_argument("Invalid base64 padding"); }
        else if (padding || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/'))
            throw std::invalid_argument("Invalid TrueGear response base64 alphabet");
    }
    DWORD length = 0;
    if (!CryptStringToBinaryA(content.data(), static_cast<DWORD>(content.size()), CRYPT_STRING_BASE64,
        nullptr, &length, nullptr, nullptr)) throw std::invalid_argument("Invalid TrueGear response base64");
    std::string result(length, '\0');
    if (!CryptStringToBinaryA(content.data(), static_cast<DWORD>(content.size()), CRYPT_STRING_BASE64,
        reinterpret_cast<BYTE*>(result.data()), &length, nullptr, nullptr))
        throw std::invalid_argument("Invalid TrueGear response base64");
    result.resize(length);
    if (!result.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        result.data(), static_cast<int>(result.size()), nullptr, 0))
        throw std::invalid_argument("Invalid TrueGear response UTF-8");
    return result;
}

struct AsyncState {
    std::mutex mutex;
    std::condition_variable changed;
    DWORD error = ERROR_SUCCESS;
    bool headers_ready = false;
    bool request_sent = false;
    bool write_complete = false;
    bool read_complete = false;
    bool read_pending = false;
    bool closed = false;
    bool shutdown_complete = false;
    DWORD read_bytes = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE read_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    std::array<char, 8192> receive{};
    std::string send_buffer;
};

// One token belongs to each handle. It owns the buffers through cancellation;
// WinHTTP documents HANDLE_CLOSING as that handle's final callback.
using CallbackToken = std::shared_ptr<AsyncState>;

void CALLBACK Completion(HINTERNET, DWORD_PTR context, DWORD status,
                        void* information, DWORD information_length) {
    if (!context) return;
    auto* token = reinterpret_cast<CallbackToken*>(context);
    auto state = *token;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        delete token;
        return;
    }
    std::lock_guard<std::mutex> guard(state->mutex);
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && information &&
        information_length >= sizeof(WINHTTP_ASYNC_RESULT)) {
        state->error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
    } else if (status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) state->headers_ready = true;
    else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) state->request_sent = true;
    else if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) state->write_complete = true;
    else if (status == WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE) state->shutdown_complete = true;
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE && information &&
             information_length >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
        const auto& read = *static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information);
        state->read_bytes = read.dwBytesTransferred;
        state->read_type = read.eBufferType;
        state->read_complete = true;
        state->read_pending = false;
        if (read.eBufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) state->closed = true;
    }
    state->changed.notify_all();
}

void CloseHandle(HINTERNET& handle) {
    if (!handle) return;
    HINTERNET current = std::exchange(handle, nullptr);
    WinHttpCloseHandle(current);
}
} // namespace

std::string TrueGearRequest(std::string_view method, std::string_view content,
                           std::string_view request_id) {
    return "{\"Method\":" + Quote(method) + ",\"Body\":" + Quote(Base64(content)) +
           ",\"ReqId\":" + Quote(request_id) + "}";
}

bool IsTrueGearEventName(std::string_view value) {
    if (value.empty() || value.size() > 256) return false;
    for (unsigned char c : value)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

struct TrueGearTransport::Impl {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    HINTERNET socket = nullptr;
    std::shared_ptr<AsyncState> state;
    std::string app_id;
    std::string api_key;
    std::string error;
    size_t incoming_size = 0;
    std::string incoming;
    std::vector<TrueGearResponse> responses;
    uint64_t next_request = 0;

    void Abort() {
        // Async close cancels pending work. Callback tokens keep their buffers
        // alive; no blocked receive thread and no network wait during teardown.
        CloseHandle(socket); CloseHandle(request);
        CloseHandle(connection); CloseHandle(session);
        state.reset(); incoming_size = 0; incoming.clear(); responses.clear();
    }

    bool Fail(const char* operation, DWORD code) {
        error = std::string(operation) + " failed (Win32 " + std::to_string(code) + ")";
        Abort();
        return false;
    }

    bool Wait(bool AsyncState::*flag, const char* operation) {
        auto pending = state;
        DWORD code = ERROR_SUCCESS;
        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            if (!pending->changed.wait_for(lock, operation_timeout, [&] {
                return pending.get()->*flag || pending->error || pending->closed;
            })) code = ERROR_TIMEOUT;
            else if (pending->error) code = pending->error;
            else if (pending->closed) code = ERROR_CONNECTION_ABORTED;
        }
        return code == ERROR_SUCCESS || Fail(operation, code);
    }

    bool ArmReceive() {
        if (!socket || !state) return false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->read_pending || state->read_complete) return true;
            state->read_pending = true;
        }
        DWORD result = WinHttpWebSocketReceive(socket, state->receive.data(),
            static_cast<DWORD>(state->receive.size()), nullptr, nullptr);
        return result == NO_ERROR || Fail("WebSocket receive", result);
    }

    bool Pump() {
        if (!socket || !state) return false;
        DWORD code = ERROR_SUCCESS;
        std::string complete_message;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->error) code = state->error;
            else if (state->closed) code = ERROR_CONNECTION_ABORTED;
            else if (state->read_complete) {
                if (state->read_bytes > state->receive.size()) code = ERROR_INVALID_DATA;
                incoming_size += state->read_bytes;
                if (incoming_size > max_message_bytes) code = ERROR_BUFFER_OVERFLOW;
                if (code == ERROR_SUCCESS) incoming.append(state->receive.data(), state->read_bytes);
                if (state->read_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    state->read_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                    if (state->read_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                        complete_message = std::move(incoming);
                    incoming.clear();
                    incoming_size = 0;
                }
                state->read_complete = false;
            }
        }
        if (code) return Fail("WebSocket state", code);
        if (!complete_message.empty()) {
            try {
                auto depth_guard = [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                    if (depth > 24) throw std::invalid_argument("TrueGear response JSON too deeply nested");
                    return true;
                };
                const auto message = nlohmann::json::parse(complete_message, depth_guard);
                if (!message.is_object()) throw std::invalid_argument("TrueGear response is not an object");
                TrueGearResponse response{message.value("Method", ""), message.value("ReqId", "0"),
                    DecodeBase64(message.value("Result", ""))};
                if (!response.method.empty()) {
                    if (responses.size() == 128) responses.erase(responses.begin());
                    responses.push_back(std::move(response));
                }
            } catch (const std::exception&) {
                // A malformed response must not poison the effect cache or
                // terminate an otherwise usable local connection.
                error = "Ignored malformed TrueGear response";
            }
        }
        return ArmReceive();
    }

    bool Send(std::string_view method, std::string_view content) {
        if (!Pump()) return false;
        std::string message;
        try {
            const auto id = "tg-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()) + "-" + std::to_string(++next_request);
            message = TrueGearRequest(method, content, id);
        } catch (const std::exception& exception) {
            error = exception.what(); return false;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->write_complete = false;
            state->send_buffer = std::move(message);
        }
        DWORD result = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            state->send_buffer.data(), static_cast<DWORD>(state->send_buffer.size()));
        if (result != NO_ERROR) return Fail("WebSocket send", result);
        return Wait(&AsyncState::write_complete, "WebSocket send completion");
    }

    void Disconnect() {
        if (!socket || !state) { Abort(); return; }
        auto closing = state;
        bool failed;
        {
            std::lock_guard<std::mutex> lock(closing->mutex);
            failed = closing->error != ERROR_SUCCESS;
        }
        if (failed) { Abort(); return; }

        // Preserve wire ordering for the final stop requests: Shutdown places
        // a close frame after previous sends and leaves reception operational.
        // Unlike closing the handle immediately, consuming the peer close also
        // proves the peer read our preceding frames. This is a worker-only,
        // bounded wait; a failed or silent peer falls back to cancellation.
        const auto deadline = std::chrono::steady_clock::now() + graceful_close_timeout;
        DWORD result = WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        if (result != NO_ERROR && result != ERROR_IO_PENDING) { Abort(); return; }
        while (socket && std::chrono::steady_clock::now() < deadline) {
            bool arm_receive = false;
            bool observed_shutdown, observed_closed;
            {
                std::lock_guard<std::mutex> lock(closing->mutex);
                if (closing->error || (closing->shutdown_complete && closing->closed)) break;
                if (closing->read_complete) {
                    if (closing->read_bytes > closing->receive.size()) break;
                    incoming_size += closing->read_bytes;
                    if (incoming_size > max_message_bytes) break;
                    if (closing->read_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                        closing->read_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
                        incoming_size = 0;
                    closing->read_complete = false;
                }
                arm_receive = !closing->closed && !closing->read_pending;
                observed_shutdown = closing->shutdown_complete;
                observed_closed = closing->closed;
            }
            if (arm_receive && !ArmReceive()) break;
            std::unique_lock<std::mutex> lock(closing->mutex);
            closing->changed.wait_until(lock, deadline, [&] {
                return closing->error || closing->read_complete ||
                    closing->shutdown_complete != observed_shutdown || closing->closed != observed_closed;
            });
        }
        Abort();
    }
};

TrueGearTransport::TrueGearTransport() : impl_(std::make_unique<Impl>()) {}
TrueGearTransport::~TrueGearTransport() { impl_->Abort(); }

bool TrueGearTransport::Connect(const std::string& app_id, const std::string& api_key) {
    auto& data = *impl_;
    data.Abort();
    data.error.clear();
    if (app_id.empty() || api_key.empty() || app_id.size() > 256 || api_key.size() > 256 ||
        app_id.find(';') != std::string::npos || api_key.find(';') != std::string::npos) {
        data.error = "Registration values must be nonempty, at most 256 bytes and contain no semicolon";
        return false;
    }
    data.app_id = app_id;
    data.api_key = api_key;
    data.state = std::make_shared<AsyncState>();
    data.session = WinHttpOpen(L"Titanfall2.TrueGear/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!data.session) return data.Fail("WinHttpOpen", GetLastError());
    if (!WinHttpSetTimeouts(data.session, 800, 800, 800, 800))
        return data.Fail("WinHttpSetTimeouts", GetLastError());
    if (WinHttpSetStatusCallback(data.session, Completion,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES |
        WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return data.Fail("WinHttpSetStatusCallback", GetLastError());
    data.connection = WinHttpConnect(data.session, L"127.0.0.1", service_port, 0);
    if (!data.connection) return data.Fail("WinHttpConnect", GetLastError());
    data.request = WinHttpOpenRequest(data.connection, L"GET", L"/v1/tact/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!data.request) return data.Fail("WinHttpOpenRequest", GetLastError());
    auto* request_token = new CallbackToken(data.state);
    const auto request_context = reinterpret_cast<DWORD_PTR>(request_token);
    if (!WinHttpSetOption(data.request, WINHTTP_OPTION_CONTEXT_VALUE,
        const_cast<DWORD_PTR*>(&request_context), sizeof(request_context))) {
        delete request_token;
        return data.Fail("Set request context", GetLastError());
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(data.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy)) ||
        !WinHttpSetOption(data.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return data.Fail("Set WebSocket options", GetLastError());
    if (!WinHttpSendRequest(data.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, request_context))
        return data.Fail("WinHttpSendRequest", GetLastError());
    if (!data.Wait(&AsyncState::request_sent, "HTTP send completion")) return false;
    if (!WinHttpReceiveResponse(data.request, nullptr)) return data.Fail("WinHttpReceiveResponse", GetLastError());
    if (!data.Wait(&AsyncState::headers_ready, "HTTP response")) return false;
    DWORD status = 0, status_length = sizeof(status);
    if (!WinHttpQueryHeaders(data.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_length, WINHTTP_NO_HEADER_INDEX))
        return data.Fail("Read HTTP status", GetLastError());
    if (status != 101) return data.Fail("HTTP WebSocket upgrade", status);
    auto* socket_token = new CallbackToken(data.state);
    data.socket = WinHttpWebSocketCompleteUpgrade(data.request, reinterpret_cast<DWORD_PTR>(socket_token));
    if (!data.socket) {
        delete socket_token;
        return data.Fail("WinHttpWebSocketCompleteUpgrade", GetLastError());
    }
    CloseHandle(data.request);
    return data.Send("register_app", app_id + ";" + api_key);
}

bool TrueGearTransport::Register() { return impl_->Send("register_app", impl_->app_id + ";" + impl_->api_key); }
bool TrueGearTransport::PlayByUuid(const std::string& uuid) {
    if (!IsTrueGearEventName(uuid)) { impl_->error = "Invalid TrueGear event name/UUID"; return false; }
    return impl_->Send("play_effect_by_uuid", impl_->app_id + ";" + uuid);
}
bool TrueGearTransport::PlayContent(const std::string& content) {
    try {
        if (content.size() > max_message_bytes) throw std::invalid_argument("Effect content exceeds limit");
        auto depth_guard = [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 24) throw std::invalid_argument("Effect content is too deeply nested");
            return true;
        };
        auto effect = nlohmann::json::parse(content, depth_guard);
        if (!effect.is_object() || !effect.contains("name") || !effect["name"].is_string() ||
            !IsTrueGearEventName(effect["name"].get<std::string>()) ||
            !effect.contains("uuid") || effect["uuid"] != effect["name"] ||
            !effect.contains("tracks") || !effect["tracks"].is_array())
            throw std::invalid_argument("Effect requires matching valid name/UUID and tracks array");
        for (const auto& track : effect["tracks"]) {
            if (!track.is_object()) throw std::invalid_argument("Effect tracks must be objects");
            if (track.contains("stop_name") && (!track["stop_name"].is_string() ||
                (!track["stop_name"].get<std::string>().empty() &&
                 !IsTrueGearEventName(track["stop_name"].get<std::string>()))))
                throw std::invalid_argument("Invalid effect stop_name");
        }
        return impl_->Send("play_effect_by_content", content);
    } catch (const std::exception& exception) {
        impl_->error = exception.what(); return false;
    }
}
bool TrueGearTransport::SeekByUuid(const std::string& uuid) {
    if (!IsTrueGearEventName(uuid)) { impl_->error = "Invalid TrueGear event name/UUID"; return false; }
    return impl_->Send("seek_by_uuid", impl_->app_id + ";" + uuid);
}
bool TrueGearTransport::Stop(const std::string& uuid) {
    if (!IsTrueGearEventName(uuid)) { impl_->error = "Invalid TrueGear event name/UUID"; return false; }
    return impl_->Send("stop", uuid);
}
bool TrueGearTransport::StopAll() { return impl_->Send("stop_all", ""); }
bool TrueGearTransport::Connected() { return impl_->Pump(); }
std::vector<TrueGearResponse> TrueGearTransport::DrainResponses() {
    impl_->Pump();
    std::vector<TrueGearResponse> result;
    result.swap(impl_->responses);
    return result;
}
void TrueGearTransport::Disconnect() { impl_->Disconnect(); }
const std::string& TrueGearTransport::last_error() const { return impl_->error; }

} // namespace tg
