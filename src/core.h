// SplitMeter - 代理 / 直连分流流量计
// 数据层：读取 mihomo 系内核（Clash Verge / Mihomo / sing-box）的本地控制接口，
//        按连接的 chains 字段区分「走节点」与「DIRECT」，累计速率与开机以来总量。
// 许可：MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sm {

struct ConnInfo {
    std::string id;
    uint64_t up = 0;
    uint64_t down = 0;
    bool proxied = false;
};

struct CoreSnapshot {
    bool ok = false;
    uint64_t upTotal = 0;
    uint64_t downTotal = 0;
    std::vector<ConnInfo> conns;
};

struct Totals {
    uint64_t pxUp = 0, pxDown = 0;   // 走节点
    uint64_t dxUp = 0, dxDown = 0;   // DIRECT（经过内核但不走节点）
};

// 在 \\.\pipe\ 下寻找 mihomo 系内核的控制管道；找不到返回空串
std::wstring FindCorePipe();

// 拉取 /connections，填充快照
bool PollCore(const std::wstring& pipe, CoreSnapshot& out, std::string* err = nullptr);

class Splitter {
public:
    void reset();

    // 首次启动时用内核已有累计量做种子（历史部分按当前连接比例拆分）
    void seed(const Totals& t) {
        acc_ = t;
        warmed_ = false;
    }

    // 喂入一次快照。内核重启（累计量倒退）时自动清空基线，返回 false。
    bool feed(const CoreSnapshot& s);

    const Totals& totals() const { return acc_; }

    // 最近一次采样得到的瞬时速率（字节/秒，已做指数平滑）
    double pxUpRate() const { return pxUpRate_; }
    double pxDownRate() const { return pxDownRate_; }
    double dxUpRate() const { return dxUpRate_; }
    double dxDownRate() const { return dxDownRate_; }
    double secondsSinceLast() const { return dt_; }

private:
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> seen_;
    Totals acc_{};
    uint64_t lastCoreUp_ = 0, lastCoreDown_ = 0;
    bool hasBaseline_ = false;
    bool warmed_ = false;  // 种子之后第一次采样只建基线，不累加
    std::chrono::steady_clock::time_point lastTick_{};
    double pxUpRate_ = 0, pxDownRate_ = 0, dxUpRate_ = 0, dxDownRate_ = 0;
    double dt_ = 0;
};

// 跨进程持久化：记录本次开机周期的累计量与窗口位置
struct Persist {
    uint64_t bootId = 0;      // 开机时刻（unix 秒）
    Totals totals{};
    int x = INT32_MIN;
    int y = INT32_MIN;
    bool autostart = false;
};

uint64_t CurrentBootId();

// 找正在运行的 mihomo 系内核进程的启动时刻（unix 秒）；找不到返回 0
uint64_t CoreProcessStartTime();
bool LoadPersist(Persist& p);
void SavePersist(const Persist& p);

// 把字节数格式化成 1.23 MB 这类短字符串
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatRate(double bytesPerSec);

}  // namespace sm
