// config.h - 应用配置模型：全局设置 + 策略调用点表 + 多条目标注入规则。
//
// 与 YAML 文本互转（Parse / Dump），供常驻宿主与 UI 共用。hook 组件的参数以原始
// YAML 子树保留（HookSetting::params），因此新增组件参数不需要改动本文件。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "waf/yamlmini.h"

namespace waf {

    /** 一条策略内「组件名 -> 调用指令 RVA 列表」的调用点表。 */
    using StrategyCallsites = std::map<std::string, std::vector<ULONG_PTR>>;

    /**
     * 策略名 -> 该策略的调用点表。
     *
     * 从配置文件的顶层 `strategies:` 段装载，因此新增目标版本只需编辑配置，
     * 不必重新编译。
     */
    using StrategyTable = std::map<std::string, StrategyCallsites>;

    /** 单个 hook 组件的配置。 */
    struct HookSetting {
            std::string name; ///< 组件名，与 HkComponent::name 一致
            bool enabled = true;
            std::string scope =
                "callsite";        ///< "callsite"（仅策略已知调用点）| "global"
            yamlmini::Node params; ///< 除 enabled/scope 外的组件参数，原样保留
    };

    /** 一条目标应用的注入规则。 */
    struct TargetRule {
            std::string executable; ///< 进程映像名，如 stellaris.exe
            bool enabled = true;
            std::string strategy =
                "sdl2-gdi"; ///< 适配策略（提供调用点 RVA 表）
            std::string geometry = "work_area"; ///< "work_area" | "monitor"
            int injectDelayMs =
                400; ///< 目标启动后多少毫秒再注入（0 = 尽可能早）
            std::string refreshDisplays =
                "none";                 ///< 显示器缓存刷新方式："none" | "sdl"
            bool verifyCallsite = true; ///< 注入前校验调用点字节
            std::vector<HookSetting> hooks;
    };

    /** 全局配置。 */
    struct AppConfig {
            bool enabled = true; ///< 总开关（HookLauncher 注入时读）
            std::vector<TargetRule> targets;
            /** 目标规则的 strategy 引用这里的调用点表。 */
            StrategyTable strategies;

            /**
             * 解析 YAML 文本。
             *
             * @param text UTF-8 配置文本。
             * @param out  解析结果（成功时覆盖）。
             * @param err  失败原因。
             * @return     成功为 true。
             */
            static bool Parse(
                const std::string & text,
                AppConfig & out,
                std::string & err
            );

            /**
             * 序列化为 YAML 文本。
             *
             * @param header 原样置于文件开头，用于写入注释头。
             * @return YAML 文本，可被 Parse() 读回。
             */
            std::string Dump(const std::string & header = std::string()) const;

            /**
             * 按进程名查找启用中的规则（不区分大小写）。
             *
             * @return 命中的规则；未命中或规则被禁用时为 nullptr。
             */
            const TargetRule * FindRule(const std::string & exeName) const;
    };

    /** 新建一条规则的模板：含 GetMonitorInfoW(rcMonitor->rcWork) 与
     * GetDeviceCaps(HORZRES/VERTRES)。 */
    TargetRule NewDefaultRule(const std::string & executable);

    /** 首次运行时的默认配置：一条 stellaris.exe 规则。 */
    AppConfig DefaultConfig();

    /**
     * 重命名一张策略表（`std::map` 的键无法原地改，只能取出键再放回）。
     *
     * 同时把引用该策略的规则的 `strategy`
     * 改成新名，避免规则指向已经不存在的表。
     *
     * @param cfg  待修改的配置。
     * @param from 原策略名。
     * @param to   新策略名；须非空且不与已有策略重名。
     * @return     实际改名返回 true；名字为空、未变化或冲突时返回 false。
     */
    bool RenameStrategy(
        AppConfig & cfg,
        const std::string & from,
        const std::string & to
    );

} // namespace waf
