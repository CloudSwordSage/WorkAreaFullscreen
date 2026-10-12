// config.h - 应用配置模型：全局设置 + 多条目标注入规则。
//
// 与 YAML 文本互转（Parse / Dump），供常驻宿主与 UI 共用。hook 组件的参数以原始
// YAML 子树保留（HookSetting::params），因此新增组件参数不需要改动本文件。
#pragma once
#include <string>
#include <vector>

#include "waf/yamlmini.h"

namespace waf {

/** 单个 hook 组件的配置。 */
struct HookSetting {
    std::string    name;                 ///< 组件名，与 HkComponent::name 一致
    bool           enabled = true;
    std::string    scope = "callsite";   ///< "callsite"（仅策略已知调用点）| "global"
    yamlmini::Node params;               ///< 除 enabled/scope 外的组件参数，原样保留
};

/** 一条目标应用的注入规则。 */
struct TargetRule {
    std::string              executable;              ///< 进程映像名，如 stellaris.exe
    bool                     enabled = true;
    std::string              strategy = "sdl2-gdi";   ///< 适配策略（提供调用点 RVA 表）
    std::string              geometry = "work_area";  ///< "work_area" | "monitor"
    int                      injectDelayMs = 400;     ///< 目标启动后多少毫秒再注入（0 = 尽可能早）
    bool                     verifyCallsite = true;   ///< 注入前校验调用点字节
    std::vector<HookSetting> hooks;
};

/** 全局配置。 */
struct AppConfig {
    bool                    enabled = true;   ///< 总开关（HookLauncher 注入时读）
    std::vector<TargetRule> targets;

    /**
     * 解析 YAML 文本。
     *
     * @param text UTF-8 配置文本。
     * @param out  解析结果（成功时覆盖）。
     * @param err  失败原因。
     * @return     成功为 true。
     */
    static bool Parse(const std::string& text, AppConfig& out, std::string& err);

    /**
     * 序列化为 YAML 文本。
     *
     * @param header 原样置于文件开头，用于写入注释头。
     * @return YAML 文本，可被 Parse() 读回。
     */
    std::string Dump(const std::string& header = std::string()) const;

    /**
     * 按进程名查找启用中的规则（不区分大小写）。
     *
     * @return 命中的规则；未命中或规则被禁用时为 nullptr。
     */
    const TargetRule* FindRule(const std::string& exeName) const;
};

/** 新建一条规则的模板：含 GetMonitorInfoW(rcMonitor->rcWork) 与 GetDeviceCaps(HORZRES/VERTRES)。 */
TargetRule NewDefaultRule(const std::string& executable);

/** 首次运行时的默认配置：一条 stellaris.exe 规则。 */
AppConfig DefaultConfig();

} // namespace waf
