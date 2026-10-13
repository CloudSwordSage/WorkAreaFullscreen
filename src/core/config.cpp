// config.cpp - 配置模型与 YAML 的互转。
#include "waf/config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace waf {
    namespace {

        yamlmini::Node Scalar(const std::string & v) {
            yamlmini::Node n;
            n.kind = yamlmini::Node::Scalar;
            n.scalar = v;
            return n;
        }

        const char * B(bool v) {
            return v ? "true" : "false";
        }

        bool BoolOf(const yamlmini::Node * n, bool def) {
            return n ? n->toBool(def) : def;
        }
        int IntOf(const yamlmini::Node * n, int def) {
            return n ? n->toInt(def) : def;
        }
        std::string StrOf(const yamlmini::Node * n, const char * def) {
            return n ? n->str(def) : std::string(def);
        }

        /** 解析 RVA 标量（`0x` 十六进制或十进制）。 @return 非法返回 false。 */
        bool RvaOf(const yamlmini::Node & n, ULONG_PTR * out) {
            const std::string s = n.str();
            if (s.empty())
                return false;
            errno = 0;
            char * end = nullptr;
            const unsigned long long v = _strtoui64(s.c_str(), &end, 0);
            if (end == s.c_str() || *end != '\0' || errno != 0)
                return false;
            *out = static_cast<ULONG_PTR>(v);
            return true;
        }

        /** RVA 序列化为 YAML 标量（十六进制）。 */
        std::string Hex(ULONG_PTR v) {
            char buf[32];
            _snprintf_s(
                buf,
                sizeof(buf),
                _TRUNCATE,
                "0x%llX",
                static_cast<unsigned long long>(v)
            );
            return buf;
        }

    } // namespace

    bool AppConfig::Parse(
        const std::string & text,
        AppConfig & out,
        std::string & err
    ) {
        yamlmini::Node root;
        if (!yamlmini::Parse(text, root, err))
            return false;
        if (root.kind != yamlmini::Node::Map) {
            err = "root: 期望键值映射";
            return false;
        }

        out = AppConfig{};
        out.enabled = BoolOf(root.find("enabled"), true);

        const yamlmini::Node * targets = root.find("targets");
        if (targets) {
            if (targets->kind != yamlmini::Node::Seq) {
                err = "targets: 期望序列";
                return false;
            }
            for (const auto & t : targets->seq) {
                TargetRule r;
                r.executable = StrOf(t.find("executable"), "");
                r.enabled = BoolOf(t.find("enabled"), true);
                r.strategy = StrOf(t.find("strategy"), "sdl2-gdi");
                r.geometry = StrOf(t.find("geometry"), "work_area");
                r.injectDelayMs = IntOf(t.find("inject_delay_ms"), 400);
                if (r.injectDelayMs < 0)
                    r.injectDelayMs = 0;
                r.refreshDisplays = StrOf(t.find("refresh_displays"), "none");

                const yamlmini::Node * safety = t.find("safety");
                r.verifyCallsite = BoolOf(
                    safety ? safety->find("verify_callsite") : nullptr,
                    true
                );

                // hooks 子树交给组件解析：这里只拆出
                // enabled/scope，其余原样保留。
                const yamlmini::Node * hooks = t.find("hooks");
                if (hooks && hooks->kind == yamlmini::Node::Map) {
                    for (const auto & kv : hooks->map) {
                        HookSetting h;
                        h.name = kv.first;
                        h.enabled = BoolOf(kv.second.find("enabled"), true);
                        h.scope = StrOf(kv.second.find("scope"), "callsite");
                        h.params.kind = yamlmini::Node::Map;
                        for (const auto & p : kv.second.map)
                            if (p.first != "enabled" && p.first != "scope")
                                h.params.map.push_back(p);
                        r.hooks.push_back(std::move(h));
                    }
                }
                out.targets.push_back(std::move(r));
            }
        }

        // 顶层 strategies: 策略名 -> 组件 -> 调用指令 RVA 序列。目标规则按
        // strategy 名引用；从配置装载而非编译进代码，新增版本无需重新编译。
        const yamlmini::Node * strategies = root.find("strategies");
        if (strategies) {
            if (strategies->kind != yamlmini::Node::Map) {
                err = "strategies: 期望键值映射";
                return false;
            }
            for (const auto & st : strategies->map) {
                if (st.second.kind != yamlmini::Node::Map) {
                    err = "strategies." + st.first + ": 期望键值映射";
                    return false;
                }
                StrategyCallsites callsites;
                for (const auto & comp : st.second.map) {
                    if (comp.second.kind != yamlmini::Node::Seq) {
                        err = "strategies." + st.first + "." + comp.first +
                              ": 期望序列";
                        return false;
                    }
                    std::vector<ULONG_PTR> rvas;
                    for (const auto & item : comp.second.seq) {
                        ULONG_PTR rva = 0;
                        if (!RvaOf(item, &rva)) {
                            err = "strategies." + st.first + "." + comp.first +
                                  ": 非法 RVA '" + item.str() + "'";
                            return false;
                        }
                        rvas.push_back(rva);
                    }
                    callsites[comp.first] = std::move(rvas);
                }
                out.strategies[st.first] = std::move(callsites);
            }
        }
        return true;
    }

    std::string AppConfig::Dump(const std::string & header) const {
        yamlmini::Node root;
        root.kind = yamlmini::Node::Map;
        root.map.emplace_back("enabled", Scalar(B(enabled)));

        yamlmini::Node targets;
        targets.kind = yamlmini::Node::Seq;
        for (const auto & t : this->targets) {
            yamlmini::Node tn;
            tn.kind = yamlmini::Node::Map;
            tn.map.emplace_back("executable", Scalar(t.executable));
            tn.map.emplace_back("enabled", Scalar(B(t.enabled)));
            tn.map.emplace_back("strategy", Scalar(t.strategy));
            tn.map.emplace_back("geometry", Scalar(t.geometry));
            tn.map.emplace_back(
                "inject_delay_ms",
                Scalar(std::to_string(t.injectDelayMs))
            );
            tn.map.emplace_back("refresh_displays", Scalar(t.refreshDisplays));

            yamlmini::Node hooks;
            hooks.kind = yamlmini::Node::Map;
            for (const auto & h : t.hooks) {
                yamlmini::Node hn;
                hn.kind = yamlmini::Node::Map;
                hn.map.emplace_back("enabled", Scalar(B(h.enabled)));
                for (const auto & p : h.params.map)
                    hn.map.push_back(p);
                hn.map.emplace_back("scope", Scalar(h.scope));
                hooks.map.emplace_back(h.name, std::move(hn));
            }
            tn.map.emplace_back("hooks", std::move(hooks));

            yamlmini::Node safety;
            safety.kind = yamlmini::Node::Map;
            safety.map.emplace_back(
                "verify_callsite",
                Scalar(B(t.verifyCallsite))
            );
            tn.map.emplace_back("safety", std::move(safety));

            targets.seq.push_back(std::move(tn));
        }
        root.map.emplace_back("targets", std::move(targets));

        if (!this->strategies.empty()) {
            yamlmini::Node strategies;
            strategies.kind = yamlmini::Node::Map;
            for (const auto & st : this->strategies) {
                yamlmini::Node components;
                components.kind = yamlmini::Node::Map;
                for (const auto & comp : st.second) {
                    yamlmini::Node list;
                    list.kind = yamlmini::Node::Seq;
                    for (ULONG_PTR rva : comp.second)
                        list.seq.push_back(Scalar(Hex(rva)));
                    components.map.emplace_back(comp.first, std::move(list));
                }
                strategies.map.emplace_back(st.first, std::move(components));
            }
            root.map.emplace_back("strategies", std::move(strategies));
        }

        return header + yamlmini::Dump(root);
    }

    const TargetRule * AppConfig::FindRule(const std::string & exeName) const {
        for (const auto & t : targets)
            if (t.enabled && !t.executable.empty() &&
                _stricmp(t.executable.c_str(), exeName.c_str()) == 0)
                return &t;
        return nullptr;
    }

    bool StrategyHasCallsites(
        const StrategyTable & table,
        const std::string & strategy,
        const std::string & component
    ) {
        const auto st = table.find(strategy);
        if (st == table.end())
            return false;
        const auto comp = st->second.find(component);
        return comp != st->second.end() && !comp->second.empty();
    }

    bool EnforceCallsiteScopes(AppConfig & cfg) {
        bool changed = false;
        for (auto & t : cfg.targets)
            for (auto & h : t.hooks)
                if (h.scope != "global" &&
                    !StrategyHasCallsites(cfg.strategies, t.strategy, h.name)) {
                    h.scope = "global";
                    changed = true;
                }
        return changed;
    }

    TargetRule NewDefaultRule(const std::string & executable) {
        TargetRule r;
        r.executable = executable;

        // 位置来源：SDL 用 rcMonitor 居中；改为 rcWork
        // 后居中结果落在工作区左上角。
        HookSetting gmi;
        gmi.name = "GetMonitorInfoW";
        gmi.params.kind = yamlmini::Node::Map;
        {
            yamlmini::Node fields;
            fields.kind = yamlmini::Node::Map;
            fields.map.emplace_back("rcMonitor", Scalar("rcWork"));
            gmi.params.map.emplace_back("fields", std::move(fields));
        }
        r.hooks.push_back(std::move(gmi));

        // 尺寸来源：SDL 取 desktop_mode；改为工作区宽高后窗口正好铺满工作区。
        HookSetting gdc;
        gdc.name = "GetDeviceCaps";
        gdc.params.kind = yamlmini::Node::Map;
        {
            yamlmini::Node indices;
            indices.kind = yamlmini::Node::Seq;
            indices.seq.push_back(Scalar("HORZRES"));
            indices.seq.push_back(Scalar("VERTRES"));
            gdc.params.map.emplace_back("indices", std::move(indices));
        }
        r.hooks.push_back(std::move(gdc));

        // 尺寸的另一条来源：SDL 的显示模式列表由 EnumDisplaySettingsW
        // 逐项填充，不经 GetDeviceCaps。
        HookSetting eds;
        eds.name = "EnumDisplaySettingsW";
        eds.scope = "global"; // 通用模板不绑定策略表；群星默认见 DefaultConfig
        eds.params.kind = yamlmini::Node::Map;
        r.hooks.push_back(std::move(eds));

        return r;
    }

    AppConfig DefaultConfig() {
        AppConfig c;

        // 默认策略表：调用点 RVA 绑定 stellaris.exe + 静态链接的 SDL 2.26.5
        // 特定版本；定位过程与约束见 docs/stellaris.md。verify_callsite 会用
        // 指令字节与 IAT 槽复核，不一致即回退，版本变化不会写坏进程。
        // 这份表只在首次生成配置文件时写入；已有配置以文件内容为准。
        StrategyCallsites & sdl2gdi = c.strategies["sdl2-gdi"];
        sdl2gdi["GetMonitorInfoW"] =
            {0x1DFAAF5, 0x1DFABA1, 0x1DFB491, 0x1DFE184};
        sdl2gdi["GetDeviceCaps"] = {0x1DFBB35, 0x1DFBB45};
        sdl2gdi["EnumDisplaySettingsW"] = {0x1DFAE30, 0x1DFB022, 0x1DFBA98};

        c.targets.push_back(NewDefaultRule("stellaris.exe"));
        // 群星的 desktop_mode 只在视频初始化或收到 WM_DISPLAYCHANGE 时重建，
        // 需要 SDL 刷新方式；通用模板 NewDefaultRule 默认 "none"。
        c.targets[0].refreshDisplays = "sdl";
        // 群星的 EnumDisplaySettingsW 调用点已定位（SDL Win32 后端 3 处），
        // 上面已给出表项，这里收窄为 callsite；通用模板默认 global。
        for (auto & h : c.targets[0].hooks)
            if (h.name == "EnumDisplaySettingsW")
                h.scope = "callsite";
        return c;
    }

    bool RenameStrategy(
        AppConfig & cfg,
        const std::string & from,
        const std::string & to
    ) {
        if (from.empty() || to.empty() || from == to)
            return false;
        if (cfg.strategies.find(from) == cfg.strategies.end())
            return false;
        if (cfg.strategies.find(to) != cfg.strategies.end())
            return false;

        auto node = cfg.strategies.extract(from);
        node.key() = to;
        cfg.strategies.insert(std::move(node));

        // 引用该策略的规则同步改名；否则改名后规则会指向一张不存在的表。
        for (auto & t : cfg.targets)
            if (t.strategy == from)
                t.strategy = to;
        return true;
    }

} // namespace waf
