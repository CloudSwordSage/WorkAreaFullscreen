// config.cpp - 配置模型与 YAML 的互转。
#include "waf/config.h"

#include <string.h>

namespace waf {
namespace {

yamlmini::Node Scalar(const std::string& v)
{
    yamlmini::Node n;
    n.kind   = yamlmini::Node::Scalar;
    n.scalar = v;
    return n;
}

const char* B(bool v) { return v ? "true" : "false"; }

bool   BoolOf(const yamlmini::Node* n, bool def)          { return n ? n->toBool(def) : def; }
int    IntOf(const yamlmini::Node* n, int def)            { return n ? n->toInt(def) : def; }
std::string StrOf(const yamlmini::Node* n, const char* def) { return n ? n->str(def) : std::string(def); }

} // namespace

bool AppConfig::Parse(const std::string& text, AppConfig& out, std::string& err)
{
    yamlmini::Node root;
    if (!yamlmini::Parse(text, root, err)) return false;
    if (root.kind != yamlmini::Node::Map) {
        err = "root: 期望键值映射";
        return false;
    }

    out                = AppConfig{};
    out.enabled        = BoolOf(root.find("enabled"), true);
    out.injectDelayMs  = IntOf(root.find("inject_delay_ms"), 400);
    out.scanIntervalMs = IntOf(root.find("scan_interval_ms"), 1000);
    if (out.injectDelayMs < 0)    out.injectDelayMs = 0;
    if (out.scanIntervalMs < 100) out.scanIntervalMs = 100;   // 低于此值只是空转

    const yamlmini::Node* ui = root.find("ui");
    out.minimizeToTray = BoolOf(ui ? ui->find("minimize_to_tray") : nullptr, true);

    const yamlmini::Node* targets = root.find("targets");
    if (targets) {
        if (targets->kind != yamlmini::Node::Seq) {
            err = "targets: 期望序列";
            return false;
        }
        for (const auto& t : targets->seq) {
            TargetRule r;
            r.executable = StrOf(t.find("executable"), "");
            r.enabled    = BoolOf(t.find("enabled"), true);
            r.strategy   = StrOf(t.find("strategy"), "sdl2-gdi");
            r.geometry   = StrOf(t.find("geometry"), "work_area");

            const yamlmini::Node* safety = t.find("safety");
            r.verifyCallsite = BoolOf(safety ? safety->find("verify_callsite") : nullptr, true);

            // hooks 子树交给组件解析：这里只拆出 enabled/scope，其余原样保留。
            const yamlmini::Node* hooks = t.find("hooks");
            if (hooks && hooks->kind == yamlmini::Node::Map) {
                for (const auto& kv : hooks->map) {
                    HookSetting h;
                    h.name    = kv.first;
                    h.enabled = BoolOf(kv.second.find("enabled"), true);
                    h.scope   = StrOf(kv.second.find("scope"), "callsite");
                    h.params.kind = yamlmini::Node::Map;
                    for (const auto& p : kv.second.map)
                        if (p.first != "enabled" && p.first != "scope")
                            h.params.map.push_back(p);
                    r.hooks.push_back(std::move(h));
                }
            }
            out.targets.push_back(std::move(r));
        }
    }
    return true;
}

std::string AppConfig::Dump(const std::string& header) const
{
    yamlmini::Node root;
    root.kind = yamlmini::Node::Map;
    root.map.emplace_back("enabled", Scalar(B(enabled)));
    root.map.emplace_back("inject_delay_ms", Scalar(std::to_string(injectDelayMs)));
    root.map.emplace_back("scan_interval_ms", Scalar(std::to_string(scanIntervalMs)));

    yamlmini::Node ui;
    ui.kind = yamlmini::Node::Map;
    ui.map.emplace_back("minimize_to_tray", Scalar(B(minimizeToTray)));
    root.map.emplace_back("ui", std::move(ui));

    yamlmini::Node targets;
    targets.kind = yamlmini::Node::Seq;
    for (const auto& t : this->targets) {
        yamlmini::Node tn;
        tn.kind = yamlmini::Node::Map;
        tn.map.emplace_back("executable", Scalar(t.executable));
        tn.map.emplace_back("enabled", Scalar(B(t.enabled)));
        tn.map.emplace_back("strategy", Scalar(t.strategy));
        tn.map.emplace_back("geometry", Scalar(t.geometry));

        yamlmini::Node hooks;
        hooks.kind = yamlmini::Node::Map;
        for (const auto& h : t.hooks) {
            yamlmini::Node hn;
            hn.kind = yamlmini::Node::Map;
            hn.map.emplace_back("enabled", Scalar(B(h.enabled)));
            for (const auto& p : h.params.map) hn.map.push_back(p);
            hn.map.emplace_back("scope", Scalar(h.scope));
            hooks.map.emplace_back(h.name, std::move(hn));
        }
        tn.map.emplace_back("hooks", std::move(hooks));

        yamlmini::Node safety;
        safety.kind = yamlmini::Node::Map;
        safety.map.emplace_back("verify_callsite", Scalar(B(t.verifyCallsite)));
        tn.map.emplace_back("safety", std::move(safety));

        targets.seq.push_back(std::move(tn));
    }
    root.map.emplace_back("targets", std::move(targets));

    return header + yamlmini::Dump(root);
}

const TargetRule* AppConfig::FindRule(const std::string& exeName) const
{
    for (const auto& t : targets)
        if (t.enabled && !t.executable.empty() && _stricmp(t.executable.c_str(), exeName.c_str()) == 0)
            return &t;
    return nullptr;
}

TargetRule NewDefaultRule(const std::string& executable)
{
    TargetRule r;
    r.executable = executable;

    // 位置来源：SDL 用 rcMonitor 居中；改为 rcWork 后居中结果落在工作区左上角。
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

    // 尺寸的另一条来源：SDL 的显示模式列表由 EnumDisplaySettingsW 逐项填充，不经 GetDeviceCaps。
    HookSetting eds;
    eds.name = "EnumDisplaySettingsW";
    eds.scope = "global";   // 策略表里没有该组件的调用点，只能按全局处理
    eds.params.kind = yamlmini::Node::Map;
    r.hooks.push_back(std::move(eds));

    return r;
}

AppConfig DefaultConfig()
{
    AppConfig c;
    c.targets.push_back(NewDefaultRule("stellaris.exe"));
    return c;
}

} // namespace waf
