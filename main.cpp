#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "libs/json.hpp"
#include "src/http.hpp"

using json = nlohmann::ordered_json;
using namespace std::chrono;

static std::string USERNAME;
static std::string PASSWORD;
static long long   SCHOOL_ID;
static std::string ACTIVITY_ID;
static int         ADVANCE_MS  = 150;
static constexpr int LIST_PAGE_SIZE = 10;
static constexpr int LIST_PAGE_DELAY_MS = 800;
static constexpr int DETAIL_REQUEST_DELAY_MS = 1200;
static const std::string LOGIN_URL = "https://apis.pocketuni.net/uc/user/login";
static const std::string INFO_URL  = "https://apis.pocketuni.net/activity/basic/info/";
static const std::string JOIN_URL  = "https://apis.pocketuni.net/apis/activity/join";
static const std::string LIST_URL  = "https://apis.pocketuni.net/apis/activity/list";
static const std::string VERSION_URL = "https://class.pocketuni.net/version.json?t=";

#define PROGRAM_VERSION "V_12731792034892744"

static const uint8_t PSK[16] = {
    121, 121, 0, 19, 5, 49, 2, 43, 13, 17, 11, 9, 4, 29, 60, 11
};

static std::mutex g_log_mtx;
static std::mutex g_joined_mtx;
static std::set<std::string> g_pending_activities;

static std::string now_str() {
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
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

// AES-CBC, PKCS7 padding, random bytes, and Base64 are provided by Windows APIs.
static std::string base64_encode(const std::vector<uint8_t>& data) {
#ifdef _WIN32
    DWORD size = 0;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &size)) return {};
    std::string out(size, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &size)) return {};
    out.resize(size);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
#else
    if (data.empty()) return {};
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    const auto size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), data.data(),
        static_cast<int>(data.size()));
    if (size < 0) return {};
    out.resize(static_cast<size_t>(size));
    return out;
#endif
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
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, iv, sizeof(iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0
        || BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                             reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                             sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0
        || BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                             &result_size, 0) != 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<uint8_t> key_object(object_size);
    if (BCryptGenerateSymmetricKey(alg, &key, key_object.data(), object_size,
                                   const_cast<PUCHAR>(PSK), sizeof(PSK), 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<uint8_t> encryption_iv(iv, iv + sizeof(iv));
    std::vector<uint8_t> ct(pt.size() + 16);
    ULONG ct_size = 0;
    if (BCryptEncrypt(key, pt.data(), static_cast<ULONG>(pt.size()), nullptr,
                      encryption_iv.data(), static_cast<ULONG>(encryption_iv.size()),
                      ct.data(), static_cast<ULONG>(ct.size()),
                      &ct_size, BCRYPT_BLOCK_PADDING) != 0) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    ct.resize(ct_size);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);

    std::vector<uint8_t> combined;
    combined.reserve(16 + ct.size());
    combined.insert(combined.end(), iv, iv + 16);
    combined.insert(combined.end(), ct.begin(), ct.end());

    return base64_encode(combined);
#else
    if (RAND_bytes(iv, sizeof(iv)) != 1) return {};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    std::vector<uint8_t> ct(pt.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int final_written = 0;
    const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, PSK, iv) == 1
        && EVP_EncryptUpdate(ctx, ct.data(), &written, pt.data(), static_cast<int>(pt.size())) == 1
        && EVP_EncryptFinal_ex(ctx, ct.data() + written, &final_written) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    ct.resize(static_cast<size_t>(written + final_written));
    std::vector<uint8_t> combined;
    combined.reserve(sizeof(iv) + ct.size());
    combined.insert(combined.end(), iv, iv + sizeof(iv));
    combined.insert(combined.end(), ct.begin(), ct.end());
    return base64_encode(combined);
#endif
}

using HttpResponse = pu::HttpResponse;
using Session = pu::HttpSession;

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

struct Config {
    std::string username;
    std::string password;
    long long school_id = 0;
    std::string college;
    std::vector<std::string> excluded_credits;
    int advance_ms = 150;
    int service_refresh_minutes = 28;
    std::string service_rest_time;
    bool service = false;
    std::string ntfy_server;
    std::string ntfy_topic;
    std::string ntfy_username;
    std::string ntfy_password;
};

static bool parse_clock_minutes(const std::string& value, int& minutes) {
    const auto separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size())
        return false;
    try {
        size_t hour_length = 0;
        size_t minute_length = 0;
        const int hour = std::stoi(value.substr(0, separator), &hour_length);
        const int minute = std::stoi(value.substr(separator + 1), &minute_length);
        if (hour_length != separator || minute_length != value.size() - separator - 1 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59)
            return false;
        minutes = hour * 60 + minute;
        return true;
    } catch (...) {
        return false;
    }
}

static bool is_service_rest_time(const Config& cfg) {
    if (cfg.service_rest_time.empty()) return false;
    const auto separator = cfg.service_rest_time.find('-');
    if (separator == std::string::npos) {
        LOG_WARN("service_rest_time 格式无效, 应为 HH:MM-HH:MM: ", cfg.service_rest_time);
        return false;
    }
    int start = 0;
    int end = 0;
    if (!parse_clock_minutes(cfg.service_rest_time.substr(0, separator), start) ||
        !parse_clock_minutes(cfg.service_rest_time.substr(separator + 1), end)) {
        LOG_WARN("service_rest_time 格式无效, 应为 HH:MM-HH:MM: ", cfg.service_rest_time);
        return false;
    }
    auto now = system_clock::to_time_t(system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const int current = local.tm_hour * 60 + local.tm_min;
    if (start == end) return false;
    if (start < end) return current >= start && current < end;
    return current >= start || current < end;
}

static std::string ntfy_url(const Config& cfg) {
    if (cfg.ntfy_server.empty() || cfg.ntfy_topic.empty()) return {};
    auto server = cfg.ntfy_server;
    while (!server.empty() && server.back() == '/') server.pop_back();
    return server + "/" + cfg.ntfy_topic;
}

static std::string url_encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : value) {
        if ((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' || character == '~') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0F]);
        }
    }
    return encoded;
}

static void notify_ntfy(Session& session, const Config& cfg,
                        const std::string& title, const std::string& message) {
    const auto base_url = ntfy_url(cfg);
    if (base_url.empty()) {
        LOG_WARN("未发送 ntfy 通知: 未配置 ntfy_server 和 ntfy_topic");
        return;
    }
    const auto url = base_url + "?title=" + url_encode(title) + "&priority=3";//你可以改 我就用默认3了
    auto headers = base_headers();
    if (!cfg.ntfy_username.empty()) {
        const auto credentials = cfg.ntfy_username + ":" + cfg.ntfy_password;
        headers.push_back("Authorization: Basic " + base64_encode(
            std::vector<uint8_t>(credentials.begin(), credentials.end())));
    }
    LOG_INFO("发送 ntfy 通知");
    const auto response = session.post(url, headers, message, 10);
    if (response.status < 200 || response.status >= 300)
        LOG_WARN("ntfy 通知失败, status=", response.status, " body=", response.body);
    else
        LOG_INFO("ntfy 通知发送成功, status=", response.status);
}

static bool is_joined_cached_unlocked(const std::string& activity_id) {
    std::ifstream in("joined_activities.json");
    if (!in) {
        LOG_INFO("未找到本地报名记录: joined_activities.json");
        return false;
    }
    try {
        json cache;
        in >> cache;
        const bool joined = cache.contains(activity_id);
        LOG_INFO(joined ? "本地报名记录命中: " : "本地报名记录未命中: ", activity_id);
        return joined;
    } catch (...) {
        LOG_WARN("读取本地报名记录失败, 按未报名处理: joined_activities.json");
        return false;
    }
}

static bool is_joined_cached(const std::string& activity_id) {
    std::lock_guard<std::mutex> lock(g_joined_mtx);
    return is_joined_cached_unlocked(activity_id);
}

static void mark_joined(const std::string& activity_id, const std::string& result) {
    std::lock_guard<std::mutex> lock(g_joined_mtx);
    json cache = json::object();
    std::ifstream in("joined_activities.json");
    try { if (in) in >> cache; } catch (...) { cache = json::object(); }
    cache[activity_id] = json{{"result", result}, {"saved_at", std::time(nullptr)}};
    std::ofstream out("joined_activities.json", std::ios::trunc);
    if (!out) {
        LOG_ERR("无法写入本地报名记录: joined_activities.json");
        return;
    }
    out << cache.dump(2);
    if (!out) {
        LOG_ERR("写入本地报名记录失败: joined_activities.json");
        return;
    }
    try {
        LOG_INFO("本地报名记录已写入: ",
                 (std::filesystem::current_path() / "joined_activities.json").string());
    } catch (...) {
        LOG_INFO("本地报名记录已写入: joined_activities.json");
    }
}

struct CourseEntry {
    int day = 0;
    int start_minute = 0;
    int end_minute = 0;
    std::vector<int> weeks;
};

struct CourseSchedule {
    std::time_t semester_start = 0;
    std::vector<CourseEntry> courses;
    bool valid = false;
};

static std::time_t add_days(std::time_t value, int days);
static CourseSchedule load_course_schedule(bool enabled);

static std::string login(Session& session);

static Config load_config() {
    Config cfg;
    std::ifstream in("config.json");
    if (!in) {
#ifdef _WIN32
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(sizeof(cwd), cwd);
        LOG_ERR("找不到 config.json, 当前工作目录: ", cwd);
#else
        char cwd[4096] = {};
        if (getcwd(cwd, sizeof(cwd)))
            LOG_ERR("找不到 config.json, 当前工作目录: ", cwd);
        else
            LOG_ERR("找不到 config.json");
#endif
        return cfg;
    }
    try {
        json j = json::parse(in);
        cfg.username = j.value("username", "");
        cfg.password = j.value("password", "");
        cfg.school_id = j.value("sid", 0LL);
        cfg.college = j.value("college", "");
        cfg.advance_ms = j.value("advance_ms", 150);
        cfg.service_refresh_minutes = j.value("service_refresh_minutes", 28);
        cfg.service_rest_time = j.value("service_rest_time", "");
        cfg.service = j.value("service", false);
        cfg.ntfy_server = j.value("ntfy_server", "");
        cfg.ntfy_topic = j.value("ntfy_topic", "");
        cfg.ntfy_username = j.value("ntfy_username", "");
        cfg.ntfy_password = j.value("ntfy_password", "");
        std::string excluded = j.value("excluded_credits", "");
        std::stringstream ss(excluded);
        std::string item;
        while (std::getline(ss, item, '|')) {
            if (!item.empty() && (ss.peek() == '|')) ss.get();
            if (!item.empty()) cfg.excluded_credits.push_back(item);
        }
    } catch (const std::exception& e) {
        LOG_WARN("读取 config.json 失败: ", e.what());
    }
    if (cfg.username.empty() || cfg.password.empty() || cfg.school_id == 0)
        LOG_WARN("配置不完整: username/password/sid 必须非空");
    return cfg;
}

static bool contains_name(const std::vector<std::string>& names, const std::string& value) {
    return std::find(names.begin(), names.end(), value) != names.end();
}

static bool is_joinable_status(const std::string& status) {
    return status == "报名未开始" || status == "报名进行中";
}

static bool is_activity_full(const json& activity) {
    const auto capacity = activity.value("allowUserCount", 0LL);
    const auto joined = activity.value("joinUserCount", 0LL);
    return capacity > 0 && joined >= capacity;
}

static std::time_t parse_saved_at(const json& value) {
    try {
        if (value.is_number_integer()) return value.get<std::time_t>();
        if (value.is_string()) {
            std::tm tm{};
            std::istringstream input(value.get<std::string>());
            input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            if (!input.fail()) return std::mktime(&tm);
        }
    } catch (...) {}
    return 0;
}

static std::string load_saved_token(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    try {
        json j = json::parse(in);
        const auto token = j.value("token", "");
        const auto saved = parse_saved_at(j.value("saved_at", json()));
        const auto age = std::time(nullptr) - saved;
        if (!token.empty() && saved > 0 && age >= 0 && age < 7 * 24 * 60 * 60)
            return token;
    } catch (...) {}
    return {};
}

static std::string load_saved_token() {
    const auto token = load_saved_token("session.json");
    if (!token.empty()) {
        LOG_INFO("发现本地登录状态: session.json");
        return token;
    }
    return {};
}

static void save_token(const std::string& token) {
    const char* path = "session.json";
    std::ofstream out(path, std::ios::trunc);
    out << json{{"token", token}, {"saved_at", std::time(nullptr)}}.dump(2);
}

static std::string get_token(Session& session, const Config& cfg) {
    auto token = load_saved_token();
    if (!token.empty()) {
        LOG_INFO("复用本地登录状态");
        return token;
    }
    USERNAME = cfg.username;
    PASSWORD = cfg.password;
    SCHOOL_ID = cfg.school_id;
    token = login(session);
    save_token(token);
    return token;
}

static std::string login(Session& session) {
    LOG_INFO("登录中");

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
        LOG_INFO("登录接口返回 code=", j.value("code", -1),
                 ", message=", j.value("message", ""));
        if (j.contains("data") && j["data"].contains("token"))
            token = j["data"]["token"].get<std::string>();
    } catch (const std::exception& e) {
        LOG_ERR("登录响应不是有效 JSON: ", e.what(), " body=", r.body);
    }

    if (token.empty()) {
        LOG_ERR("登录失败, 服务端响应: ", r.body);
        std::exit(1);
    }
    LOG_OK("登录成功");
    return token;
}

struct ActivityInfo {
    long long id = 0;
    std::string name;
    std::string joinStartTime;
    std::string startTime;
    std::string endTime;
    std::string address;
    std::string creditName;
    double credit = 0;
    std::string category;
    bool eligible = false;
    std::vector<std::string> allowed_colleges;
};

static std::string activity_notification_message(const ActivityInfo& activity) {
    return "活动时间: " + activity.startTime + " - " + activity.endTime +
           "\n活动地点: " + activity.address;
}

static ActivityInfo get_activity(Session& session,
                                 const std::string& token,
                                 const std::string& activity_id)
{
    auto r = session.get(INFO_URL + activity_id, auth_headers(token), 10);

    ActivityInfo info;
    try {
        auto j = json::parse(r.body);
        if (j.contains("data")) {
            auto& b = j["data"];
            info.id = b.value("id", std::stoll(activity_id));
            info.name = b.value("name", "");
            info.joinStartTime = b.value("joinStartTime", "");
            info.startTime = b.value("startTime", "");
            info.endTime = b.value("endTime", "");
            info.address = b.value("address", "");
            info.credit = b.value("credit", 0.0);
            info.category = b.value("category", json::object()).value("name", "");
            info.creditName = info.category;
            for (const auto& college : b.value("allowCollege", json::array()))
                info.allowed_colleges.push_back(college.value("name", ""));
        }
    } catch (const std::exception& e) {
        LOG_ERR("解析活动信息失败: ", e.what(), " 原文=", r.body);
    }
    return info;
}

static system_clock::time_point parse_dt(const std::string& s);

static bool overlaps_course(const ActivityInfo& activity, const CourseSchedule& schedule) {
    if (!schedule.valid || activity.startTime.empty() || activity.endTime.empty()) return false;
    const auto activity_start = parse_dt(activity.startTime);
    const auto activity_end = parse_dt(activity.endTime);
    if (activity_start == system_clock::time_point{} || activity_end <= activity_start) return false;
    const auto activity_start_time = system_clock::to_time_t(activity_start);
    const auto activity_end_time = system_clock::to_time_t(activity_end);

    std::tm activity_day{};
#ifdef _WIN32
    localtime_s(&activity_day, &activity_start_time);
#else
    localtime_r(&activity_start_time, &activity_day);
#endif
    const auto days_since_semester = static_cast<int>(
        std::difftime(activity_start_time, schedule.semester_start) / (24 * 60 * 60));
    if (days_since_semester < 0) return false;
    const int week = days_since_semester / 7 + 1;
    const int day = activity_day.tm_wday == 0 ? 7 : activity_day.tm_wday;
    for (const auto& course : schedule.courses) {
        if (course.day != day) continue;
        if (std::find(course.weeks.begin(), course.weeks.end(), week) == course.weeks.end())
            continue;
        const auto course_day = add_days(schedule.semester_start, (week - 1) * 7 + day - 1);
        const auto course_start = course_day + course.start_minute * 60;
        const auto course_end = course_day + course.end_minute * 60;
        if (activity_start_time < course_end && course_start < activity_end_time)
            return true;
    }
    return false;
}

static void cache_activity(const ActivityInfo& info) {
    json cache = json::object();
    std::ifstream in("activity_cache.json");
    try { if (in) in >> cache; } catch (...) { cache = json::object(); }
    cache[std::to_string(info.id)] = {
        {"id", info.id}, {"name", info.name}, {"joinStartTime", info.joinStartTime},
        {"startTime", info.startTime}, {"endTime", info.endTime}, {"address", info.address},
        {"creditName", info.creditName}, {"credit", info.credit}, {"category", info.category},
        {"allowed_colleges", info.allowed_colleges}
    };
    std::ofstream out("activity_cache.json", std::ios::trunc);
    if (!out) {
        LOG_ERR("无法写入活动详情缓存: activity_cache.json");
        return;
    }
    out << cache.dump(2);
    if (!out) {
        LOG_ERR("写入活动详情缓存失败: activity_cache.json");
        return;
    }
    try {
        LOG_INFO("活动详情缓存已写入: ",
                 (std::filesystem::current_path() / "activity_cache.json").string());
    } catch (...) {
        LOG_INFO("活动详情缓存已写入: activity_cache.json");
    }
}

static bool load_cached_activity(const std::string& id, ActivityInfo& info) {
    std::ifstream in("activity_cache.json");
    if (!in) {
        LOG_INFO("未找到活动详情缓存: activity_cache.json");
        return false;
    }
    try {
        json cache; in >> cache;
        if (!cache.contains(id)) {
            LOG_INFO("活动详情缓存未命中: ", id);
            return false;
        }
        auto& b = cache[id];
        info.id = b.value("id", std::stoll(id));
        info.name = b.value("name", "");
        info.joinStartTime = b.value("joinStartTime", "");
        info.startTime = b.value("startTime", "");
        info.endTime = b.value("endTime", "");
        info.address = b.value("address", "");
        info.creditName = b.value("creditName", "");
        info.credit = b.value("credit", 0.0);
        info.category = b.value("category", "");
        for (const auto& college : b.value("allowed_colleges", json::array()))
            info.allowed_colleges.push_back(college.get<std::string>());
        LOG_INFO("活动详情缓存命中: ", id);
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("读取活动详情缓存失败, 按未命中处理: ", e.what());
        return false;
    }
}

static std::vector<ActivityInfo> list_possible_activities(Session& session,
                                                            const std::string& token,
                                                            const Config& cfg,
                                                            const CourseSchedule& schedule,
                                                            bool* auth_failed = nullptr) {
    if (auth_failed) *auth_failed = false;
    std::vector<ActivityInfo> result;
    std::vector<std::string> activity_ids;
    int page = 1;
    int total_pages = 1;
    while (true) {
        json body{{"sort", 1}, {"page", page}, {"limit", LIST_PAGE_SIZE},
                  {"puType", 0}, {"isAudit", {0}}};
        auto r = session.post(LIST_URL, auth_headers(token), body.dump(), 10);
        json j;
        try { j = json::parse(r.body); } catch (...) { LOG_ERR("活动列表解析失败: ", r.body); break; }
        const auto response_code = j.value("code", 0LL);
        if (r.status == 401 || r.status == 403 || response_code == 401 || response_code == 403) {
            if (auth_failed) *auth_failed = true;
            LOG_WARN("登录状态已失效, 将重新登录");
            break;
        }
        if (response_code != 0) {
            LOG_ERR("活动列表请求失败: code=", response_code,
                    ", message=", j.value("message", ""));
            break;
        }
        auto list = j.value("data", json::object()).value("list", json::array());
        if (list.empty()) break;
        auto page_info = j.value("data", json::object()).value("pageInfo", json::object());
        total_pages = std::max(1, page_info.value("total", page));
        LOG_INFO("扫描活动列表: 第 ", page, "/", total_pages, " 页");
        for (const auto& item : list) {
            if (!is_joinable_status(item.value("startTimeValue", ""))) continue;
            if (is_activity_full(item)) continue;
            activity_ids.push_back(std::to_string(item.value("id", 0LL)));
        }
        if (page >= total_pages) break;
        ++page;
        std::this_thread::sleep_for(milliseconds(LIST_PAGE_DELAY_MS));
    }

    const size_t total_details = activity_ids.size();
    LOG_INFO("共发现 ", total_details, " 个可报名活动, 开始获取详情");
    for (size_t index = 0; index < total_details; ++index) {
        const auto& id = activity_ids[index];
        ActivityInfo info;
        bool cached = load_cached_activity(id, info);
        if (!cached) {
            info = get_activity(session, token, id);
            if (info.id != 0) cache_activity(info);
        }
        bool category_ok = !contains_name(cfg.excluded_credits, info.category);
        bool college_ok = !cfg.college.empty() &&
            contains_name(info.allowed_colleges, cfg.college);
        const bool schedule_ok = !overlaps_course(info, schedule);
        info.eligible = category_ok && college_ok && schedule_ok;
        LOG_INFO("详情进度 [", index + 1, "/", total_details, "]",
                 cached ? " 使用缓存" : " 已请求",
                 info.eligible ? ",符合筛选条件" : ",已过滤");
        if (info.eligible) result.push_back(info);
        if (!cached && index + 1 < total_details)
            std::this_thread::sleep_for(milliseconds(DETAIL_REQUEST_DELAY_MS));
    }
    return result;
}

static void print_possible_activities(Session& session, const std::string& token,
                                      const Config& cfg,
                                      const CourseSchedule& schedule) {
    auto activities = list_possible_activities(session, token, cfg, schedule);
    if (activities.empty()) { LOG_INFO("没有符合条件的可报名活动"); return; }
    for (const auto& a : activities) {
        LOG_INFO("活动: ", a.name, " | 时间: ", a.startTime, " - ", a.endTime,
                 " | 地点: ", a.address, " | 学时: ", a.creditName, " ", a.credit);
    }
}

static void check_version(Session& session, bool bypass) {
    if (bypass) return;
    auto r = session.get(VERSION_URL + random_string(12), base_headers(), 5);
    try {
        auto remote = json::parse(r.body).value("version", "");
        if (!remote.empty() && remote != PROGRAM_VERSION)
            LOG_WARN("远程版本为 ", remote, ", 当前版本为 ", PROGRAM_VERSION,
                     ": API 可能改变, 程序可能失效!!");
    } catch (...) {
        LOG_WARN("版本检查失败, 继续执行");
    }
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

enum class SignupResult { success, already_joined, failed };

static SignupResult fire(Session& session,
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

        try {
            auto j = json::parse(text);
            const auto code = j.value("code", -1LL);
            const auto message = j.value("message", "");
            const auto data = j.value("data", json::object());
            const auto detail = data.value("msg", "");
            if (code == 0 && detail.find("报名成功") != std::string::npos) {
                LOG_OK("报名成功! (第 ", attempt, " 次): ", detail);
                return SignupResult::success;
            }
            if (code == 9405 && (message.find("已报名") != std::string::npos ||
                                 detail.find("已报名") != std::string::npos)) {
                LOG_OK("活动已报名, 停止重试");
                return SignupResult::already_joined;
            }
            if (code == 500 || code == 9405) {
                LOG_ERR("报名终止: code=", code, ", message=", message);
                return SignupResult::failed;
            }
            LOG_DBG("[", attempt, "] 报名未成功: code=", code,
                    ", message=", message, ", detail=", detail);
        } catch (...) {}

        std::this_thread::sleep_for(milliseconds(80));
    }

    LOG_ERR("报名失败, 共尝试 ", attempt, " 次");
    return SignupResult::failed;
}

static void signup_and_notify(Session& session, const std::string& token,
                              const Config& cfg, const ActivityInfo& activity) {
    const auto id = std::to_string(activity.id);
    LOG_INFO("报名任务开始: ", activity.name, " (", id, ")");
    {
        std::lock_guard<std::mutex> lock(g_joined_mtx);
        if (is_joined_cached_unlocked(id)) {
            g_pending_activities.erase(id);
            LOG_INFO("跳过已处理活动: ", activity.name, " (", id, ")");
            return;
        }
    }

    auto result = fire(session, token, id);
    {
        std::lock_guard<std::mutex> lock(g_joined_mtx);
        g_pending_activities.erase(id);
    }

    std::string result_text;
    std::string title_suffix;
    if (result == SignupResult::success) {
        result_text = "报名成功";
        title_suffix = "报名成功";
        mark_joined(id, "success");
    } else if (result == SignupResult::already_joined) {
        result_text = "已报名, 已加入本地记录";
        title_suffix = "已报名";
        mark_joined(id, "already_joined");
    } else {
        result_text = "报名失败或服务端拒绝";
        title_suffix = "报名失败";
    }
    LOG_INFO("报名任务结束: ", activity.name, " (", id, ") -> ", result_text);
    const auto message = activity_notification_message(activity);
    const auto title = activity.name + title_suffix + "! ";
    notify_ntfy(session, cfg, title, message);
}

struct ServiceTokenState {
    std::mutex mutex;
};

static std::string ensure_service_token(Session& session, const Config& cfg,
                                        ServiceTokenState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto token = load_saved_token();
    if (!token.empty()) return token;

    USERNAME = cfg.username;
    PASSWORD = cfg.password;
    SCHOOL_ID = cfg.school_id;
    token = login(session);
    save_token(token);
    return token;
}

static std::string refresh_service_token(Session& session, const Config& cfg,
                                         ServiceTokenState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    USERNAME = cfg.username;
    PASSWORD = cfg.password;
    SCHOOL_ID = cfg.school_id;
    const auto token = login(session);
    save_token(token);
    return token;
}

static void run_service(Session& session, Config cfg, bool ignore_version,
                        bool course_filter) {
    check_version(session, ignore_version);
    auto token_state = std::make_shared<ServiceTokenState>();
    std::string token = ensure_service_token(session, cfg, *token_state);
    int round = 0;
    while (true) {
        ++round;
        cfg = load_config();
        USERNAME = cfg.username;
        PASSWORD = cfg.password;
        SCHOOL_ID = cfg.school_id;
        ADVANCE_MS = cfg.advance_ms;
        if (cfg.username.empty() || cfg.password.empty() || cfg.school_id == 0) {
            LOG_ERR("服务刷新跳过: config.json 的 username/password/sid 不完整");
        } else if (is_service_rest_time(cfg)) {
            LOG_INFO("当前处于休息时段 ", cfg.service_rest_time, ", 跳过活动列表请求");
        } else {
            token = ensure_service_token(session, cfg, *token_state);
            LOG_INFO("服务模式第 ", round, " 轮开始扫描");
            bool auth_failed = false;
            const auto schedule = load_course_schedule(course_filter);
            auto activities = list_possible_activities(session, token, cfg, schedule, &auth_failed);
            if (auth_failed) {
                LOG_WARN("活动列表认证失败, 重新登录后重试本轮");
                token = refresh_service_token(session, cfg, *token_state);
                activities = list_possible_activities(session, token, cfg, schedule);
            }
            std::stable_sort(activities.begin(), activities.end(),
                [](const ActivityInfo& left, const ActivityInfo& right) {
                    if (left.joinStartTime != right.joinStartTime)
                        return left.joinStartTime < right.joinStartTime;
                    return left.credit > right.credit;
                });
            LOG_INFO("本轮发现 ", activities.size(), " 个可报名活动");
            for (size_t activity_index = 0; activity_index < activities.size(); ++activity_index) {
                const auto& activity = activities[activity_index];
                const auto activity_id = std::to_string(activity.id);
                {
                    std::lock_guard<std::mutex> lock(g_joined_mtx);
                    if (is_joined_cached_unlocked(activity_id) ||
                        !g_pending_activities.insert(activity_id).second) {
                        LOG_INFO("跳过已处理或已有任务的活动: ", activity.name,
                                 " (", activity.id, ")");
                        continue;
                    }
                }
                const auto start = parse_dt(activity.joinStartTime);
                size_t same_start_rank = 0;
                for (size_t previous = 0; previous < activity_index; ++previous) {
                    if (activities[previous].joinStartTime == activity.joinStartTime)
                        ++same_start_rank;
                }
                if (start != system_clock::time_point{} && start > system_clock::now()) {
                    const auto task_start = start - minutes(5);
                    const auto wait_ms = duration_cast<milliseconds>(
                        task_start - system_clock::now()).count();
                    LOG_OK("创建定时报名任务: ", activity.name, " (", activity.id,
                             "), 提前 5 分钟启动, 等待 ", std::max<int64_t>(0, wait_ms),
                             " ms, 报名序号 ", same_start_rank + 1);
                    std::thread([&session, token_state, cfg, activity, start, wait_ms,
                                 same_start_rank]() {
                        if (wait_ms > 0) std::this_thread::sleep_for(milliseconds(wait_ms));
                        ensure_service_token(session, cfg, *token_state);
                        wait_until(start, 0);
                        if (same_start_rank > 0)
                            std::this_thread::sleep_for(seconds(static_cast<int>(same_start_rank)));
                        const auto task_token = ensure_service_token(session, cfg, *token_state);
                        signup_and_notify(session, task_token, cfg, activity);
                    }).detach();
                } else {
                    LOG_OK("创建立即报名任务: ", activity.name, " (", activity.id,
                             "), 报名序号 ", same_start_rank + 1);
                    std::thread([&session, token_state, cfg, activity, same_start_rank]() {
                        if (same_start_rank > 0)
                            std::this_thread::sleep_for(seconds(static_cast<int>(same_start_rank)));
                        const auto task_token = ensure_service_token(session, cfg, *token_state);
                        signup_and_notify(session, task_token, cfg, activity);
                    }).detach();
                }
            }
        }
        const int refresh_minutes = std::max(1, cfg.service_refresh_minutes);
        LOG_INFO("服务模式将在 ", refresh_minutes,
             " 分钟后重新读取配置并刷新报名表");
        std::this_thread::sleep_for(minutes(refresh_minutes));
    }
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --list [--no-course] [--ignore-version]\n"
              << "       " << prog << " --signup <activity_id> [--force] [--ignore-version]\n"
              << "       " << prog << " --service [--no-course] [--ignore-version]\n"
              << "--list: 登录并列出符合条件的报名未开始活动\n"
              << "--service: 无限运行! 每指定分钟刷新配置和报名表\n"
              << "--no-course: 列表模式指定不使用课表筛选\n"
              << "--signup <activity_id>: 报名指定活动\n"
              << "--force: 忽略本地已报名记录并实际发送报名请求\n"
              << "--ignore-version: 跳过版本检查(无用)\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    bool list_mode = false;
    bool service_mode = false;
    bool ignore_version = false;
    bool force_signup = false;
    bool course_filter = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") list_mode = true;
        else if (arg == "--service") service_mode = true;
        else if (arg == "--force") force_signup = true;
        else if (arg == "--course") course_filter = true;
        else if (arg == "--no-course") course_filter = false;
        else if (arg == "--signup") {
            if (i + 1 >= argc || std::string(argv[i + 1]).compare(0, 2, "--") == 0) {
                LOG_ERR("--signup 必须提供 activity_id");
                print_usage(argv[0]);
                return 2;
            }
            ACTIVITY_ID = argv[++i];
            try {
                if (std::stoll(ACTIVITY_ID) <= 0) throw std::invalid_argument("non-positive");
            } catch (...) {
                LOG_ERR("activity_id 错误: ", ACTIVITY_ID);
                return 2;
            }
        }
        else if (arg == "--ignore-version") ignore_version = true;
        else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            LOG_ERR("未知参数: ", arg);
            print_usage(argv[0]);
            return 2;
        }
    }

    Config cfg = load_config();
    service_mode = service_mode || cfg.service;
    if (cfg.username.empty() || cfg.password.empty() || cfg.school_id == 0) {
        LOG_ERR("请在 config.json 填写 username、password、sid");
        print_usage(argv[0]);
        return 1;
    }
    if (list_mode && !ACTIVITY_ID.empty()) {
        LOG_ERR("--list 不能与 --signup 同时使用");
        return 2;
    }
    if (service_mode && (list_mode || !ACTIVITY_ID.empty())) {
        LOG_ERR("--service 不能与 --list 或 --signup 同时使用");
        return 2;
    }
    if (!list_mode && ACTIVITY_ID.empty()) {
        if (service_mode) {
            USERNAME = cfg.username;
            PASSWORD = cfg.password;
            SCHOOL_ID = cfg.school_id;
            ADVANCE_MS = cfg.advance_ms;
        } else {
        LOG_ERR("请使用 --signup <activity_id> 报名, 或使用 --list");
        return 1;
        }
    }

    USERNAME = cfg.username;
    PASSWORD = cfg.password;
    SCHOOL_ID = cfg.school_id;
    ADVANCE_MS = cfg.advance_ms;

    Session session;
    if (service_mode) {
        run_service(session, cfg, ignore_version, course_filter);
        return 0;
    }
    check_version(session, ignore_version);
    std::string token = get_token(session, cfg);
    if (list_mode) {
        const auto schedule = load_course_schedule(course_filter);
        print_possible_activities(session, token, cfg, schedule);
        return 0;
    }

    ActivityInfo info;
    if (!load_cached_activity(ACTIVITY_ID, info)) {
        info = get_activity(session, token, ACTIVITY_ID);
        if (info.id != 0) cache_activity(info);
    }
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

    if (!force_signup && is_joined_cached(ACTIVITY_ID)) {
        LOG_INFO("本地记录显示活动已报名, 跳过重复操作");
        notify_ntfy(session, cfg, info.name + "已报名! ",
                   activity_notification_message(info));
        return 0;
    }
    const auto result = fire(session, token, ACTIVITY_ID);
    if (result == SignupResult::success) {
        mark_joined(ACTIVITY_ID, "success");
        notify_ntfy(session, cfg, info.name + "报名成功! ",
                   activity_notification_message(info));
    } else if (result == SignupResult::already_joined) {
        mark_joined(ACTIVITY_ID, "already_joined");
        notify_ntfy(session, cfg, info.name + "已报名! ",
                   activity_notification_message(info));
    } else {
        notify_ntfy(session, cfg, info.name + "报名失败! ",
                   activity_notification_message(info));
    }
    return 0;
}

static std::time_t parse_date(const std::string& value) {
    std::tm tm{};
    std::istringstream input(value);
    input >> std::get_time(&tm, "%Y-%m-%d");
    if (input.fail()) return 0;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return std::mktime(&tm);
}

static int parse_clock_minutes(const std::string& value) {
    int hour = 0;
    int minute = 0;
    char separator = 0;
    std::istringstream input(value);
    input >> hour >> separator >> minute;
    if (input.fail() || separator != ':' || hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return -1;
    return hour * 60 + minute;
}

static std::time_t add_days(std::time_t value, int days) {
    return value + static_cast<std::time_t>(days) * 24 * 60 * 60;
}

static CourseSchedule load_course_schedule(bool enabled) {
    CourseSchedule schedule;
    if (!enabled) return schedule;
    std::ifstream in("mycourse.json");
    if (!in) {
        LOG_INFO("未找到 mycourse.json, 关闭课表冲突筛选");
        return schedule;
    }
    try {
        json j = json::parse(in);
        schedule.semester_start = parse_date(j.value("startDate", ""));
        if (schedule.semester_start == 0) throw std::runtime_error("startDate 无效");
        for (const auto& item : j.value("courses", json::array())) {
            CourseEntry course;
            course.day = item.value("day", 0);
            course.start_minute = parse_clock_minutes(item.value("start", ""));
            course.end_minute = parse_clock_minutes(item.value("end", ""));
            for (const auto& week : item.value("weeks", json::array()))
                course.weeks.push_back(week.get<int>());
            if (course.day >= 1 && course.day <= 7 &&
                course.start_minute >= 0 && course.end_minute > course.start_minute &&
                !course.weeks.empty())
                schedule.courses.push_back(course);
        }
        schedule.valid = !schedule.courses.empty();
        LOG_INFO(schedule.valid ? "已启用课表冲突筛选" : "课表没有有效课程,关闭冲突筛选");
    } catch (const std::exception& e) {
        LOG_WARN("读取 mycourse.json 失败,关闭课表冲突筛选: ", e.what());
    }
    return schedule;
}