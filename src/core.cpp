// SplitMeter - 数据层实现
// 许可：MIT

#include "core.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>

namespace sm {
namespace {

// ---------------------------------------------------------------- JSON

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* find(const char* key) const {
        for (auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class Parser {
public:
    Parser(const char* p, size_t n) : p_(p), n_(n) {}

    bool parse(JVal& v) {
        skipWs();
        if (!parseValue(v, true, 0)) return false;
        return true;
    }

private:
    void skipWs() {
        while (i_ < n_) {
            char c = p_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* lit) {
        size_t len = std::strlen(lit);
        if (i_ + len > n_ || std::memcmp(p_ + i_, lit, len) != 0) return false;
        i_ += len;
        return true;
    }

    bool parseString(std::string& out, bool keep) {
        if (i_ >= n_ || p_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < n_) {
            char c = p_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= n_) return false;
                char e = p_[i_++];
                char decoded = 0;
                switch (e) {
                    case '"': decoded = '"'; break;
                    case '\\': decoded = '\\'; break;
                    case '/': decoded = '/'; break;
                    case 'b': decoded = '\b'; break;
                    case 'f': decoded = '\f'; break;
                    case 'n': decoded = '\n'; break;
                    case 'r': decoded = '\r'; break;
                    case 't': decoded = '\t'; break;
                    case 'u': {
                        // 只用于 id / 节点名，遇到非 ASCII 直接跳过四个十六进制位
                        if (i_ + 4 > n_) return false;
                        i_ += 4;
                        if (keep) out += '?';
                        continue;
                    }
                    default: return false;
                }
                if (keep) out += decoded;
            } else if (keep) {
                out += c;
            }
        }
        return false;
    }

    bool parseNumber(double& out) {
        size_t start = i_;
        if (i_ < n_ && (p_[i_] == '-' || p_[i_] == '+')) ++i_;
        while (i_ < n_ && ((p_[i_] >= '0' && p_[i_] <= '9') || p_[i_] == '.' || p_[i_] == 'e' ||
                           p_[i_] == 'E' || p_[i_] == '-' || p_[i_] == '+')) {
            ++i_;
        }
        if (i_ == start) return false;
        out = std::strtod(std::string(p_ + start, i_ - start).c_str(), nullptr);
        return true;
    }

    bool parseValue(JVal& v, bool keep, int depth) {
        if (depth > 32) return false;
        skipWs();
        if (i_ >= n_) return false;
        char c = p_[i_];
        if (c == '{') return parseObject(v, keep, depth);
        if (c == '[') return parseArray(v, keep, depth);
        if (c == '"') {
            std::string s;
            if (!parseString(s, keep)) return false;
            if (keep) {
                v.t = JVal::STR;
                v.str = std::move(s);
            }
            return true;
        }
        if (literal("true")) {
            if (keep) { v.t = JVal::BOOL; v.num = 1; }
            return true;
        }
        if (literal("false")) {
            if (keep) { v.t = JVal::BOOL; v.num = 0; }
            return true;
        }
        if (literal("null")) {
            if (keep) v.t = JVal::NUL;
            return true;
        }
        double d = 0;
        if (!parseNumber(d)) return false;
        if (keep) { v.t = JVal::NUM; v.num = d; }
        return true;
    }

    bool parseObject(JVal& v, bool keep, int depth) {
        ++i_;  // '{'
        if (keep) v.t = JVal::OBJ;
        skipWs();
        if (i_ < n_ && p_[i_] == '}') { ++i_; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key, true)) return false;
            skipWs();
            if (i_ >= n_ || p_[i_] != ':') return false;
            ++i_;
            skipWs();

            // metadata / providerChains 体积大且用不上，跳过以省掉分配
            bool childKeep = keep && key != "metadata" && key != "providerChains";
            JVal child;
            if (!parseValue(child, childKeep, depth + 1)) return false;
            if (keep && childKeep) v.obj.emplace_back(std::move(key), std::move(child));

            skipWs();
            if (i_ >= n_) return false;
            if (p_[i_] == ',') { ++i_; continue; }
            if (p_[i_] == '}') { ++i_; return true; }
            return false;
        }
    }

    bool parseArray(JVal& v, bool keep, int depth) {
        ++i_;  // '['
        if (keep) v.t = JVal::ARR;
        skipWs();
        if (i_ < n_ && p_[i_] == ']') { ++i_; return true; }
        for (;;) {
            JVal child;
            if (!parseValue(child, keep, depth + 1)) return false;
            if (keep) v.arr.push_back(std::move(child));
            skipWs();
            if (i_ >= n_) return false;
            if (p_[i_] == ',') { ++i_; continue; }
            if (p_[i_] == ']') { ++i_; return true; }
            return false;
        }
    }

    const char* p_;
    size_t n_;
    size_t i_ = 0;
};

// ---------------------------------------------------------------- 管道 HTTP

bool OverlappedWrite(HANDLE h, const char* data, DWORD len, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0;
        if (ok) ok = GetOverlappedResult(h, &ov, &written, FALSE);
    }
    CloseHandle(ov.hEvent);
    return ok && written == len;
}

bool OverlappedRead(HANDLE h, std::string& out, DWORD totalTimeoutMs) {
    auto deadline = GetTickCount64() + totalTimeoutMs;
    char buf[16384];
    for (;;) {
        DWORD remaining = 0;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return !out.empty();
        remaining = static_cast<DWORD>(deadline - now);

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, remaining) != WAIT_OBJECT_0) {
                    CancelIoEx(h, &ov);
                    CloseHandle(ov.hEvent);
                    return !out.empty();
                }
                ok = GetOverlappedResult(h, &ov, &got, FALSE);
            }
            if (!ok) {
                CloseHandle(ov.hEvent);
                DWORD e2 = GetLastError();
                if (e2 == ERROR_BROKEN_PIPE || e2 == ERROR_PIPE_NOT_CONNECTED || e2 == ERROR_HANDLE_EOF) {
                    return !out.empty();
                }
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        if (got == 0) return !out.empty();  // 服务端关闭
        out.append(buf, got);
    }
}

std::string DecodeChunked(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        size_t size = std::strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (size == 0) break;
        size_t start = eol + 2;
        if (start + size > body.size()) break;
        out.append(body, start, size);
        pos = start + size + 2;
    }
    return out;
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---------------------------------------------------------------- 管道发现

std::wstring FindCorePipe() {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::wstring best;
    int bestRank = 99;
    do {
        std::wstring name = fd.cFileName;
        std::wstring lower = name;
        for (auto& c : lower) c = static_cast<wchar_t>(::towlower(c));

        int rank = 99;
        // Clash Verge Rev 生产模式 / 其他 mihomo 内核
        if (lower.rfind(L"verge-mihomo-production", 0) == 0) rank = 0;
        else if (lower.rfind(L"verge-mihomo", 0) == 0) rank = 1;
        else if (lower.rfind(L"mihomo", 0) == 0) rank = 2;
        else if (lower.rfind(L"clash", 0) == 0) rank = 3;

        if (rank < bestRank) {
            bestRank = rank;
            best = L"\\\\.\\pipe\\" + name;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// ---------------------------------------------------------------- 抓取

bool PollCore(const std::wstring& pipe, CoreSnapshot& out, std::string* err) {
    out = CoreSnapshot{};
    if (pipe.empty()) {
        if (err) *err = "no pipe";
        return false;
    }

    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "open failed";
        return false;
    }

    const char* req =
        "GET /connections HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n"
        "Connection: close\r\n\r\n";
    bool ok = OverlappedWrite(h, req, static_cast<DWORD>(std::strlen(req)), 1500);
    std::string raw;
    if (ok) ok = OverlappedRead(h, raw, 2000);
    CloseHandle(h);

    if (!ok || raw.empty()) {
        if (err) *err = "io failed";
        return false;
    }

    size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        if (err) *err = "bad http";
        return false;
    }
    std::string head = ToLower(raw.substr(0, sep));
    std::string body = raw.substr(sep + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
        body = DecodeChunked(body);
    }

    JVal root;
    Parser parser(body.data(), body.size());
    if (!parser.parse(root)) {
        if (err) *err = "bad json";
        return false;
    }

    if (const JVal* v = root.find("uploadTotal")) out.upTotal = static_cast<uint64_t>(v->num);
    if (const JVal* v = root.find("downloadTotal")) out.downTotal = static_cast<uint64_t>(v->num);

    if (const JVal* list = root.find("connections")) {
        out.conns.reserve(list->arr.size());
        for (auto& c : list->arr) {
            ConnInfo info;
            if (const JVal* id = c.find("id")) info.id = id->str;
            if (const JVal* up = c.find("upload")) info.up = static_cast<uint64_t>(up->num);
            if (const JVal* dn = c.find("download")) info.down = static_cast<uint64_t>(dn->num);
            bool proxied = false;
            if (const JVal* ch = c.find("chains")) {
                for (auto& item : ch->arr) {
                    if (item.t == JVal::STR && item.str != "DIRECT") {
                        proxied = true;
                        break;
                    }
                }
            }
            info.proxied = proxied;
            out.conns.push_back(std::move(info));
        }
    }

    out.ok = true;
    return true;
}

// ---------------------------------------------------------------- 分流累计

void Splitter::reset() {
    seen_.clear();
    acc_ = Totals{};
    lastCoreUp_ = lastCoreDown_ = 0;
    hasBaseline_ = false;
    warmed_ = false;
    lastTick_ = std::chrono::steady_clock::time_point{};
    pxUpRate_ = pxDownRate_ = dxUpRate_ = dxDownRate_ = 0;
}

bool Splitter::feed(const CoreSnapshot& s) {
    if (!s.ok) return true;

    auto now = std::chrono::steady_clock::now();
    double dt = lastTick_.time_since_epoch().count() == 0
                    ? 0.0
                    : std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;

    // 内核重启：累计量倒退 → 清空逐连接基线（已累计的总量保留）
    if (hasBaseline_ && (s.upTotal < lastCoreUp_ || s.downTotal < lastCoreDown_)) {
        seen_.clear();
    }
    hasBaseline_ = true;
    lastCoreUp_ = s.upTotal;
    lastCoreDown_ = s.downTotal;

    uint64_t dPxUp = 0, dPxDown = 0, dDxUp = 0, dDxDown = 0;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> next;
    next.reserve(s.conns.size());

    const bool warmup = !warmed_;
    warmed_ = true;

    for (auto& c : s.conns) {
        next[c.id] = {c.up, c.down};
        if (warmup) continue;  // 种子已包含这些连接的历史字节，不再重复累加
        uint64_t pu = 0, pd = 0;
        auto it = seen_.find(c.id);
        if (it != seen_.end()) {
            pu = c.up > it->second.first ? c.up - it->second.first : 0;
            pd = c.down > it->second.second ? c.down - it->second.second : 0;
        } else {
            // 首次见到该连接：按当前累计值计入（可能是刚建立的连接，也可能遗漏了早期字节）
            pu = c.up;
            pd = c.down;
        }
        if (c.proxied) {
            dPxUp += pu;
            dPxDown += pd;
        } else {
            dDxUp += pu;
            dDxDown += pd;
        }
    }
    seen_.swap(next);

    acc_.pxUp += dPxUp;
    acc_.pxDown += dPxDown;
    acc_.dxUp += dDxUp;
    acc_.dxDown += dDxDown;

    dt_ = dt;

    // 首次采样或长时间挂起（休眠恢复）时不做平滑，避免尖峰
    if (dt <= 0.01 || dt > 10.0) {
        pxUpRate_ = pxDownRate_ = dxUpRate_ = dxDownRate_ = 0;
        return true;
    }

    auto smooth = [dt](double prev, uint64_t delta) {
        double inst = static_cast<double>(delta) / dt;
        return prev * 0.55 + inst * 0.45;
    };
    pxUpRate_ = smooth(pxUpRate_, dPxUp);
    pxDownRate_ = smooth(pxDownRate_, dPxDown);
    dxUpRate_ = smooth(dxUpRate_, dDxUp);
    dxDownRate_ = smooth(dxDownRate_, dDxDown);
    return true;
}

// ---------------------------------------------------------------- 持久化

uint64_t CurrentBootId() {
    uint64_t ticks = GetTickCount64() / 1000;  // 开机至今秒数
    uint64_t now = static_cast<uint64_t>(time(nullptr));
    return now > ticks ? now - ticks : 0;
}

uint64_t CoreProcessStartTime() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint64_t newest = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            for (auto& c : name) c = static_cast<wchar_t>(::towlower(c));
            if (name.find(L"mihomo") == std::wstring::npos &&
                name.find(L"clash") == std::wstring::npos) {
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
                uint64_t t = ((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                              created.dwLowDateTime) /
                                 10000000ULL -
                             11644473600ULL;
                if (t > newest) newest = t;
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return newest;
}

static std::wstring StatePath() {
    wchar_t dir[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    std::wstring base(dir, n);
    base += L"\\SplitMeter";
    CreateDirectoryW(base.c_str(), nullptr);
    return base + L"\\state.ini";
}

bool LoadPersist(Persist& p) {
    FILE* f = _wfopen(StatePath().c_str(), L"rb");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long a = 0;
        int b = 0;
        if (sscanf(line, "boot=%llu", &a) == 1) p.bootId = a;
        else if (sscanf(line, "pxup=%llu", &a) == 1) p.totals.pxUp = a;
        else if (sscanf(line, "pxdn=%llu", &a) == 1) p.totals.pxDown = a;
        else if (sscanf(line, "dxup=%llu", &a) == 1) p.totals.dxUp = a;
        else if (sscanf(line, "dxdn=%llu", &a) == 1) p.totals.dxDown = a;
        else if (sscanf(line, "x=%d", &b) == 1) p.x = b;
        else if (sscanf(line, "y=%d", &b) == 1) p.y = b;
        else if (sscanf(line, "autostart=%d", &b) == 1) p.autostart = b != 0;
    }
    fclose(f);
    return true;
}

void SavePersist(const Persist& p) {
    FILE* f = _wfopen(StatePath().c_str(), L"wb");
    if (!f) return;
    fprintf(f, "boot=%llu\n", static_cast<unsigned long long>(p.bootId));
    fprintf(f, "pxup=%llu\n", static_cast<unsigned long long>(p.totals.pxUp));
    fprintf(f, "pxdn=%llu\n", static_cast<unsigned long long>(p.totals.pxDown));
    fprintf(f, "dxup=%llu\n", static_cast<unsigned long long>(p.totals.dxUp));
    fprintf(f, "dxdn=%llu\n", static_cast<unsigned long long>(p.totals.dxDown));
    fprintf(f, "x=%d\n", p.x);
    fprintf(f, "y=%d\n", p.y);
    fprintf(f, "autostart=%d\n", p.autostart ? 1 : 0);
    fclose(f);
}

// ---------------------------------------------------------------- 格式化

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0) {
        swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (v < 10) {
        swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    } else {
        swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    }
    return buf;
}

std::wstring FormatRate(double bps) {
    if (bps < 1.0) return L"0";
    static const wchar_t* units[] = {L"B/s", L"K/s", L"M/s", L"G/s"};
    double v = bps;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (v < 10) {
        swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    } else if (v < 100) {
        swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    } else {
        swprintf(buf, 64, L"%.0f %ls", v, units[u]);
    }
    return buf;
}

}  // namespace sm
