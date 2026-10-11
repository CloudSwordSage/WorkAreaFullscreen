// component.h - hook 组件的统一接口与静态注册表。
//
// 新增一个组件：
//   1. 在 components/ 下新建 .cpp，实现 detour 与 configure/verify；
//   2. 定义静态 HkComponent 实例；
//   3. 用 HK_REGISTER_COMPONENT 注册。
// 宿主 hookkit.cpp 只遍历注册表并读配置，无需改动。
#pragma once
#include <windows.h>

#include <vector>

#include "waf/config.h"
#include "waf/runtime.h"

namespace waf {

/**
 * 一个 hook 组件：按配置改写某个系统 API 的返回值。
 *
 * 组件的全部可调行为来自它在 `hooks:` 下的配置块；宿主负责启用/校验/安装。
 */
struct HkComponent {
    const char*    name;        ///< 配置键名，同时用作策略表与日志中的组件标识
    const wchar_t* dll;         ///< 目标 DLL
    const char*    proc;        ///< 目标导出函数
    void*          detour;      ///< 替换函数
    void**         trampoline;  ///< 接收 MinHook 填回的原函数指针

    /**
     * 界面把本组件新加到某条规则时使用的默认配置（YAML 片段，可为 nullptr）。
     *
     * 由 injector.dll 通过 HkComponentDefaultConfig 提供给宿主，界面因此不必硬编码
     * 任何组件的参数；新增组件时也只需在这里写一份默认值。
     */
    const char*    defaultConfig;

    /**
     * 解析自身配置块。
     *
     * @param setting 该组件在 `hooks:` 下的配置（含 enabled/scope 与组件参数）。
     * @param env     运行环境（策略、几何）。
     * @return        false 表示该组件不应安装（宿主按 fallback 放行）。
     */
    bool (*configure)(const HookSetting& setting, const HkEnv& env);

    /**
     * 安装前的调用点校验；仅在 safety.verify_callsite 为真时被调用。
     *
     * @return false 表示目标版本与策略表不符，宿主将跳过安装。
     */
    bool (*verify)(const HkEnv& env);
};

/** 组件注册表（首次调用时构造，避免跨 TU 静态初始化顺序问题）。 */
inline std::vector<const HkComponent*>& HkRegistry()
{
    static std::vector<const HkComponent*> s;
    return s;
}

/** 把一个静态 HkComponent 登记到注册表；须在命名空间作用域使用。 */
#define HK_REGISTER_COMPONENT(var)                                             \
    namespace {                                                                \
    struct var##_registrar {                                                   \
        var##_registrar() { ::waf::HkRegistry().push_back(&var); }             \
    };                                                                         \
    const var##_registrar var##_registrar_instance;                            \
    }

} // namespace waf
