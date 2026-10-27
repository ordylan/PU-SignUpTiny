#include "http.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace pu {

#ifdef _WIN32
namespace {
std::string win_error_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0 || buffer == nullptr) return {};
    std::wstring message(buffer, size);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        message.pop_back();
    return std::string(message.begin(), message.end());
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct Url {
    std::wstring host;
    std::wstring path;
    std::wstring extra;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool parse_url(const std::string& value, Url& result) {
    const auto wide = utf8_to_wide(value);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) return false;
    result.host.assign(host, components.dwHostNameLength);
    result.path.assign(path, components.dwUrlPathLength);
    result.extra.assign(extra, components.dwExtraInfoLength);
    result.port = components.nPort;
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}
} // namespace

namespace {
HttpResponse request(const char* method, const std::string& url, const std::vector<std::string>& headers,
                     const std::string* body, long timeout_seconds) {
    Url parsed;
    HttpResponse response;
    if (!parse_url(url, parsed)) { response.status = -1; response.body = "invalid url"; return response; }
    HINTERNET session = WinHttpOpen(L"OD-PU-SignUpTiny/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) {
        response.status = -1;
        response.body = "WinHttpOpen failed: " + std::to_string(GetLastError());
        return response;
    }
    const DWORD timeout = static_cast<DWORD>(timeout_seconds * 1000);
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (!connection) {
        response.status = -1;
        response.body = "WinHttpConnect failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(session);
        return response;
    }
    const auto wide_method = utf8_to_wide(method);
    const auto request_path = parsed.path + parsed.extra;
    HINTERNET request = WinHttpOpenRequest(connection, wide_method.c_str(), request_path.c_str(), nullptr, nullptr, nullptr, parsed.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        response.status = -1;
        response.body = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
    std::string raw_headers;
    for (const auto& header : headers) raw_headers += header + "\r\n";
    const auto wide_headers = utf8_to_wide(raw_headers);
    const void* data = body ? body->data() : nullptr;
    const DWORD data_size = body ? static_cast<DWORD>(body->size()) : 0;
    const DWORD header_size = static_cast<DWORD>(wide_headers.size());
    BOOL ok = WinHttpSendRequest(request, wide_headers.empty() ? nullptr : wide_headers.c_str(), header_size, const_cast<void*>(data), data_size, data_size, 0);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) {
        response.status = -1;
        response.body = "WinHttpSendRequest failed: " + std::to_string(error);
        const auto message = win_error_message(error);
        if (!message.empty()) response.body += " (" + message + ")";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
        if (!ok) error = GetLastError();
    }
    if (ok) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
        response.status = static_cast<long>(code);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::string buffer(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read)) break;
            response.body.append(buffer.data(), read);
        }
    } else {
        response.status = -1;
        response.body = "WinHTTP request failed: " + std::to_string(error);
        const auto message = win_error_message(error);
        if (!message.empty()) response.body += " (" + message + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}
} // namespace

HttpResponse HttpSession::get(const std::string& url, const std::vector<std::string>& headers, long timeout_seconds) const { return request("GET", url, headers, nullptr, timeout_seconds); }
HttpResponse HttpSession::post(const std::string& url, const std::vector<std::string>& headers, const std::string& body, long timeout_seconds) const { return request("POST", url, headers, &body, timeout_seconds); }
#else
namespace {
size_t write_body(char* data, size_t size, size_t count, void* target) {
    auto* body = static_cast<std::string*>(target);
    body->append(data, size * count);
    return size * count;
}

HttpResponse request(const char* method, const std::string& url, const std::vector<std::string>& headers, const std::string* body, long timeout_seconds) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) { response.status = -1; response.body = "curl init failed"; return response; }
    curl_slist* list = nullptr;
    for (const auto& header : headers) list = curl_slist_append(list, header.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body->size());
    }
    const auto result = curl_easy_perform(curl);
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    else { response.status = -1; response.body = std::string("curl error ") + curl_easy_strerror(result); }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return response;
}
} // namespace

HttpResponse HttpSession::get(const std::string& url, const std::vector<std::string>& headers, long timeout_seconds) const { return request("GET", url, headers, nullptr, timeout_seconds); }
HttpResponse HttpSession::post(const std::string& url, const std::vector<std::string>& headers, const std::string& body, long timeout_seconds) const { return request("POST", url, headers, &body, timeout_seconds); }
#endif

} // namespace pu
