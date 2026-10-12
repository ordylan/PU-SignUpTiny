#include <windows.h>
#include <winhttp.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "libs/json.hpp"

using json = nlohmann::ordered_json;
using namespace std::chrono;

static std::string USERNAME;
static std::string PASSWORD;
static long long   SCHOOL_ID;
static std::string ACTIVITY_ID;
static const int   ADVANCE_MS  = 150;

static const std::string LOGIN_URL = "https://apis.pocketuni.net/uc/user/login";
static const std::string INFO_URL  = "https://apis.pocketuni.net/apis/activity/info";
static const std::string JOIN_URL  = "https://apis.pocketuni.net/apis/activity/join";

static const uint8_t PSK[16] = {
    121, 121, 0, 19, 5, 49, 2, 43, 13, 17, 11, 9, 4, 29, 60, 11
};

static std::mutex g_log_mtx;

static std::string now_str() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

template <typename... Args>
static void log_line(const char* tag, Args&&... args) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << "[" << now_str() << "] " << tag << " ";
    ((std::cout << std::forward<Args>(args)), ...);
    std::cout << std::endl;
}

#define LOG_INFO(...) log_line("INFO ", __VA_ARGS__)
#define LOG_OK(...)   log_line("OK   ", __VA_ARGS__)
#define LOG_WARN(...) log_line("WARN ", __VA_ARGS__)
#define LOG_ERR(...)  log_line("ERROR", __VA_ARGS__)
#define LOG_DBG(...)  log_line("DEBUG", __VA_ARGS__)

// ==================== AES-128-CBC + PKCS7 ====================
namespace aes {

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t Rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static void key_expansion(const uint8_t* key, uint8_t* rk) {
    for (int i = 0; i < 16; ++i) rk[i] = key[i];
    for (int i = 4; i < 44; ++i) {
        uint8_t t[4];
        for (int j = 0; j < 4; ++j) t[j] = rk[(i - 1) * 4 + j];
        if (i % 4 == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ Rcon[i / 4]);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[tmp];
        }
        for (int j = 0; j < 4; ++j)
            rk[i * 4 + j] = (uint8_t)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static inline void add_round_key(uint8_t* s, const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

static inline void sub_bytes(uint8_t* s) {
    for (int i = 0; i < 16; ++i) s[i] = sbox[s[i]];
}

static inline void shift_rows(uint8_t* s) {
    uint8_t t;
    t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
    t = s[2];  s[2]  = s[10]; s[10] = t;
    t = s[6];  s[6]  = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7]  = s[3];  s[3]  = t;
}

static void mix_columns(uint8_t* s) {
    for (int c = 0; c < 4; ++c) {
        uint8_t a = s[c * 4 + 0];
        uint8_t b = s[c * 4 + 1];
        uint8_t d = s[c * 4 + 2];
        uint8_t e = s[c * 4 + 3];
        s[c * 4 + 0] = (uint8_t)(gmul(a, 2) ^ gmul(b, 3) ^ d ^ e);
        s[c * 4 + 1] = (uint8_t)(a ^ gmul(b, 2) ^ gmul(d, 3) ^ e);
        s[c * 4 + 2] = (uint8_t)(a ^ b ^ gmul(d, 2) ^ gmul(e, 3));
        s[c * 4 + 3] = (uint8_t)(gmul(a, 3) ^ b ^ d ^ gmul(e, 2));
    }
}

static void encrypt_block(const uint8_t* rk, const uint8_t* in, uint8_t* out) {
    uint8_t st[16];
    std::memcpy(st, in, 16);
    add_round_key(st, rk);
    for (int r = 1; r < 10; ++r) {
        sub_bytes(st);
        shift_rows(st);
        mix_columns(st);
        add_round_key(st, rk + r * 16);
    }
    sub_bytes(st);
    shift_rows(st);
    add_round_key(st, rk + 10 * 16);
    std::memcpy(out, st, 16);
}

static std::vector<uint8_t> cbc_encrypt(const uint8_t* key,
                                        const uint8_t* iv,
                                        const std::vector<uint8_t>& pt) {
    uint8_t rk[176];
    key_expansion(key, rk);

    size_t pad = 16 - (pt.size() % 16);
    std::vector<uint8_t> padded = pt;
    padded.insert(padded.end(), pad, (uint8_t)pad);

    std::vector<uint8_t> out(padded.size());
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);

    for (size_t i = 0; i < padded.size(); i += 16) {
        uint8_t block[16];
        for (int j = 0; j < 16; ++j) block[j] = padded[i + j] ^ prev[j];
        encrypt_block(rk, block, out.data() + i);
        std::memcpy(prev, out.data() + i, 16);
    }
    return out;
}

} 
static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
    }
    if (i < data.size()) {
        unsigned int n = data[i] << 16;
        bool two = (i + 1 < data.size());
        if (two) n |= data[i + 1] << 8;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(two ? tbl[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

static std::string random_string(size_t len) {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937_64 gen(
        ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}());
    std::uniform_int_distribution<int> dis(0, 61);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) s.push_back(chars[dis(gen)]);
    return s;
}
static void secure_random(uint8_t* buf, size_t n) {
    static thread_local std::mt19937_64 gen(
        ((uint64_t)std::random_device{}() << 32)
        ^ (uint64_t)std::random_device{}()
        ^ (uint64_t)std::chrono::high_resolution_clock::now()
              .time_since_epoch().count());
    std::uniform_int_distribution<int> dis(0, 255);
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)dis(gen);
}
static std::string make_x_sign(const std::string& client = "web") {
    std::string echo = random_string(16);
    long long ts = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();

    json payload;
    payload["echo"]      = echo;
    payload["timestamp"] = std::to_string(ts);
    payload["client"]    = client;

    std::string pt_str = payload.dump();
    std::vector<uint8_t> pt(pt_str.begin(), pt_str.end());

    uint8_t iv[16];
    secure_random(iv, 16);

    auto ct = aes::cbc_encrypt(PSK, iv, pt);

    std::vector<uint8_t> combined;
    combined.reserve(16 + ct.size());
    combined.insert(combined.end(), iv, iv + 16);
    combined.insert(combined.end(), ct.begin(), ct.end());

    return base64_encode(combined);
}

struct HttpResponse {
    long        status = 0;
    std::string body;
};

static std::wstring s2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
    bool ok = false;
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl r;
    std::wstring wurl = s2w(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return r;

    r.host  = std::wstring(host, uc.dwHostNameLength);
    r.path  = std::wstring(path, uc.dwUrlPathLength);
    r.port  = uc.nPort;
    r.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    r.ok    = true;
    return r;
}

class Session {
public:
    HttpResponse post(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      long timeout_sec = 10)
    {
        HttpResponse resp;
        auto u = parse_url(url);
        if (!u.ok) { resp.status = -1; resp.body = "invalid url"; return resp; }

        HINTERNET hSession = WinHttpOpen(
            L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            L"(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { resp.status = -1; resp.body = "WinHttpOpen fail"; return resp; }

        DWORD to = (DWORD)(timeout_sec * 1000);
        WinHttpSetTimeouts(hSession, to, to, to, to);

        HINTERNET hConnect = WinHttpConnect(hSession, u.host.c_str(), u.port, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            resp.status = -1; resp.body = "WinHttpConnect fail";
            return resp;
        }

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"POST", u.path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            u.https ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            resp.status = -1; resp.body = "WinHttpOpenRequest fail";
            return resp;
        }

        std::string hdrs;
        for (auto& h : headers) { hdrs += h; hdrs += "\r\n"; }
        std::wstring whdrs = s2w(hdrs);

        BOOL ok = WinHttpSendRequest(
            hRequest,
            whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
            whdrs.empty() ? 0 : (DWORD)-1L,
            (LPVOID)body.data(), (DWORD)body.size(),
            (DWORD)body.size(), 0);

        if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

        if (!ok) {
            DWORD e = GetLastError();
            resp.status = -1;
            resp.body   = "http error " + std::to_string(e);
        } else {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(
                hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                WINHTTP_NO_HEADER_INDEX);
            resp.status = (long)code;

            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
                resp.body.append(buf.data(), read);
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }
};

static const std::string UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

static std::vector<std::string> base_headers() {
    return {
        "User-Agent: " + UA,
        "Accept: application/json, text/plain, */*",
        "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8",
        "Content-Type: application/json",
        "Origin: https://class.pocketuni.net",
        "Referer: https://class.pocketuni.net/",
        "Sec-Fetch-Dest: empty",
        "Sec-Fetch-Mode: cors",
        "Sec-Fetch-Site: same-site",
    };
}

static std::vector<std::string> auth_headers(const std::string& token) {
    auto h = base_headers();
    h.push_back("Authorization: Bearer " + token + ":" + std::to_string(SCHOOL_ID));
    return h;
}

static std::string login(Session& session) {
    LOG_INFO("登录中: ", USERNAME);

    json body;
    body["userName"] = USERNAME;
    body["password"] = PASSWORD;
    body["sid"]      = SCHOOL_ID;
    body["device"]   = "pc";

    auto r = session.post(LOGIN_URL, base_headers(), body.dump(), 10);
    if (r.status != 200) {
        LOG_ERR("登录 HTTP 失败, status=", r.status, " body=", r.body);
        std::exit(1);
    }

    std::string token;
    try {
        auto j = json::parse(r.body);
        if (j.contains("data") && j["data"].contains("token"))
            token = j["data"]["token"].get<std::string>();
    } catch (...) {}

    if (token.empty()) {
        LOG_ERR("登录失败: ", r.body);
        std::exit(1);
    }
    LOG_OK("登录成功");
    return token;
}

struct ActivityInfo {
    std::string name;
    std::string joinStartTime;
};

static ActivityInfo get_activity(Session& session,
                                 const std::string& token,
                                 const std::string& activity_id)
{
    json body;
    body["id"] = std::stoll(activity_id);

    auto r = session.post(INFO_URL, auth_headers(token), body.dump(), 10);

    ActivityInfo info;
    try {
        auto j = json::parse(r.body);
        if (j.contains("data") && j["data"].contains("baseInfo")) {
            auto& b = j["data"]["baseInfo"];
            if (b.contains("name"))          info.name          = b["name"].get<std::string>();
            if (b.contains("joinStartTime")) info.joinStartTime = b["joinStartTime"].get<std::string>();
        }
    } catch (const std::exception& e) {
        LOG_ERR("解析活动信息失败: ", e.what(), " 原文=", r.body);
    }
    return info;
}

static system_clock::time_point parse_dt(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    std::time_t t = std::mktime(&tm);
    return system_clock::from_time_t(t);
}

static void wait_until(system_clock::time_point target, int advance_ms) {
    while (true) {
        auto now = system_clock::now();
        double remain_ms = duration<double, std::milli>(target - now).count();
        if (remain_ms <= advance_ms) return;

        if (remain_ms > 1000) {
            std::this_thread::sleep_for(milliseconds((long long)(remain_ms - 500)));
        } else if (remain_ms > 100) {
            std::this_thread::sleep_for(milliseconds(50));
        } else {
            std::this_thread::sleep_for(milliseconds(2));
        }
    }
}

static bool fire(Session& session,
                 const std::string& token,
                 const std::string& activity_id,
                 double duration = 15.0)
{
    auto deadline = system_clock::now()
                  + milliseconds((long long)(duration * 1000));
    int attempt = 0;

    json body;
    body["activityId"] = std::stoll(activity_id);
    std::string body_str = body.dump();

    while (system_clock::now() < deadline) {
        ++attempt;
        auto h = auth_headers(token);
        h.push_back("X-Sign: " + make_x_sign());

        auto r = session.post(JOIN_URL, h, body_str, 4);
        const std::string& text = r.body;

        LOG_DBG("[", attempt, "] ",
                text.substr(0, std::min<size_t>(200, text.size())));

        if (text.find("报名成功") != std::string::npos) {
            LOG_OK("报名成功! (第 ", attempt, " 次)");
            return true;
        }
        if (text.find("您已报名") != std::string::npos) {
            LOG_OK("已报名过 (第 ", attempt, " 次)");
            return true;
        }
        try {
            auto j = json::parse(text);
            if (j.contains("code")) {
                long long code = j["code"].get<long long>();
                if (code == 0) {
                    LOG_OK("报名成功! (第 ", attempt, " 次) ",
                           text.substr(0, std::min<size_t>(200, text.size())));
                    return true;
                }
                if (code == 9405) {
                    LOG_OK("已报名过 (第 ", attempt, " 次)");
                    return true;
                }
            }
        } catch (...) {}

        std::this_thread::sleep_for(milliseconds(80));
    }

    LOG_ERR("报名失败, 共尝试 ", attempt, " 次");
    return false;
}

// ==================== usage ====================
static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <username> <password> <sid> <activity_id>\n"
              << "示例: " << prog
              << " 084026100 password(若含特殊字符请半角直引号包裹) 243205312479232(njucm的sid) 375177924706305(活动id)\n";
}

// ==================== main ====================
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc != 5) {
        print_usage(argv[0]);
        return 1;
    }

    USERNAME    = argv[1];
    PASSWORD    = argv[2];
    try {
        SCHOOL_ID = std::stoll(argv[3]);
    } catch (...) {
        LOG_ERR("SCHOOL_ID 不合理: ", argv[3]);
        return 1;
    }
    ACTIVITY_ID = argv[4];

    Session session;

    std::string token = login(session);

    ActivityInfo info = get_activity(session, token, ACTIVITY_ID);
    if (info.joinStartTime.empty()) {
        LOG_ERR("未拿到 joinStartTime");
        return 1;
    }

    auto start_tp = parse_dt(info.joinStartTime);
    LOG_INFO("活动名称: ", info.name);
    LOG_INFO("报名开始: ", info.joinStartTime);

    auto now = system_clock::now();
    if (start_tp <= now) {
        LOG_INFO("报名已经开始, 立即尝试");
    } else {
        double wait_s = duration<double>(start_tp - now).count();
        LOG_INFO("距开始还有 ", wait_s, "s, 等待中...");

        if (wait_s > 300) {
            std::this_thread::sleep_for(
                milliseconds((long long)((wait_s - 300) * 1000)));
            token = login(session);
        }

        wait_until(start_tp, ADVANCE_MS);
        LOG_INFO("开始抢报名");
    }

    fire(session, token, ACTIVITY_ID);
    return 0;
}