// ui.cpp - ImGui 界面：配置编辑与启动代理管理。
//
// 本程序不监听进程、不注入；每一条规则可以挂上/摘掉 IFEO 启动代理，
// 实际注入由 HookLauncher.exe 在目标执行第一条指令前完成。
#include "app.h"

#include <imgui.h>

#include <string>

#include "waf/ifeo.h"

namespace waf::app {
namespace {

const ImVec4 kGreen = ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
const ImVec4 kAmber = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);
const ImVec4 kRed   = ImVec4(0.95f, 0.45f, 0.4f, 1.0f);

// 让 InputText 直接编辑 std::string：内容变长时按需重分配缓冲区。
int StrResizeCallback(ImGuiInputTextCallbackData* d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = const_cast<char*>(s->c_str());
    }
    return 0;
}

bool InputString(const char* label, std::string& s)
{
    if (s.capacity() < 32) s.reserve(32);
    return ImGui::InputText(label, const_cast<char*>(s.c_str()), s.capacity() + 1,
                            ImGuiInputTextFlags_CallbackResize, StrResizeCallback, &s);
}

// 用组件目录里的默认配置构造一条 HookSetting：YAML 里的 scope 提到结构字段，其余进 params。
HookSetting MakeHookFromCatalog(const ComponentInfo& c)
{
    HookSetting hs;
    hs.name = c.name;

    yamlmini::Node root;
    std::string    err;
    if (!c.defaultConfigYaml.empty() &&
        yamlmini::Parse(c.defaultConfigYaml, root, err) &&
        root.kind == yamlmini::Node::Map) {
        for (const auto& kv : root.map) {
            if (kv.first == "scope") {
                hs.scope = kv.second.str("callsite");
                continue;
            }
            hs.params.map.push_back(kv);
        }
    }
    hs.params.kind = yamlmini::Node::Map;
    return hs;
}

void DrawGlobal(State& st)
{
    AppConfig& cfg   = st.config;
    bool       dirty = false;

    ImGui::SeparatorText("全局");
    dirty |= ImGui::Checkbox("关闭窗口时收进托盘", &cfg.minimizeToTray);

    if (!waf::IsProcessElevated())
        ImGui::TextColored(kAmber, "未以管理员身份运行：可以编辑配置，但挂/摘启动代理会失败");

    if (dirty) st.configDirty = true;
}

// 一条规则的启动代理开关：状态直接来自注册表，不存进配置。
void DrawLaunchProxy(State& st, const TargetRule& t)
{
    ImGui::SeparatorText("启动代理");

    const std::string exeName = waf::ExeNameOf(t.executable);
    if (exeName.empty()) {
        ImGui::TextDisabled("先填写进程名");
        return;
    }

    std::wstring installed;
    const bool   on = waf::IfeoIsInstalled(exeName, &installed);
    if (on)
        ImGui::TextColored(kGreen, "已挂载 -> %s", Narrow(installed).c_str());
    else
        ImGui::TextDisabled("未挂载");

    const bool elevated = waf::IsProcessElevated();
    if (!elevated) ImGui::BeginDisabled();
    if (!on) {
        if (ImGui::Button("挂载启动代理")) SetLaunchProxy(st, exeName, true);
    } else {
        if (ImGui::Button("摘除启动代理")) SetLaunchProxy(st, exeName, false);
    }
    if (!elevated) ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("挂载后，任何程序启动 %s 都会先经过 HookLauncher，\n"
                          "在目标执行第一条指令之前完成注入（需要管理员权限）。\n"
                          "摘除后即恢复原始启动方式。",
                          exeName.c_str());
}

void DrawTargets(State& st)
{
    AppConfig& cfg = st.config;

    ImGui::SeparatorText("目标应用规则");
    if (ImGui::Button("添加规则")) {
        cfg.targets.push_back(NewDefaultRule("game.exe"));
        st.configDirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("共 %zu 条", cfg.targets.size());

    int removeAt = -1;
    for (size_t i = 0; i < cfg.targets.size(); ++i) {
        TargetRule& t = cfg.targets[i];
        ImGui::PushID(static_cast<int>(i));

        char title[300];
        _snprintf_s(title, sizeof(title), _TRUNCATE, "%s%s###rule",
                    t.enabled ? "" : "[停用] ", t.executable.c_str());
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PopID();
            continue;
        }

        bool dirty = false;
        dirty |= ImGui::Checkbox("启用", &t.enabled);

        ImGui::SetNextItemWidth(240);
        dirty |= InputString("进程名", t.executable);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("进程映像名，如 stellaris.exe（不区分大小写）。");

        ImGui::SetNextItemWidth(150);
        int geo = (t.geometry == "monitor") ? 1 : 0;
        if (ImGui::Combo("几何", &geo, "work_area\0monitor\0")) {
            t.geometry  = geo ? "monitor" : "work_area";
            dirty       = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("work_area：窗口铺满工作区，保留任务栏等空间\n"
                              "monitor：显示器整块区域（等价不改写）");

        ImGui::SetNextItemWidth(150);
        InputString("策略", t.strategy);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("提供与目标版本绑定的调用点 RVA 表。");

        dirty |= ImGui::Checkbox("校验调用点", &t.verifyCallsite);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("注入前按 RVA 校验指令字节；版本不匹配时回退为不改写。");

        ImGui::SeparatorText("hook 组件");

        int removeHook = -1;
        for (size_t h = 0; h < t.hooks.size(); ++h) {
            HookSetting& hs = t.hooks[h];
            ImGui::PushID(static_cast<int>(h));
            dirty |= ImGui::Checkbox(hs.name.c_str(), &hs.enabled);
            ImGui::SameLine(220);
            ImGui::SetNextItemWidth(120);
            int scope = (hs.scope == "global") ? 1 : 0;
            if (ImGui::Combo("##scope", &scope, "callsite\0global\0")) {
                hs.scope = scope ? "global" : "callsite";
                dirty    = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("callsite：只对策略表记录的调用点生效\n"
                                  "global：对该 API 的所有调用生效");
            ImGui::SameLine();
            if (ImGui::SmallButton("移除")) removeHook = static_cast<int>(h);
            ImGui::PopID();
        }
        if (removeHook >= 0) {
            t.hooks.erase(t.hooks.begin() + removeHook);
            dirty = true;
        }

        if (t.hooks.empty())
            ImGui::TextDisabled("该规则没有 hook 组件，不会改写任何东西");

        // 可添加的组件来自 injector.dll 的导出目录，界面不硬编码组件知识
        if (ImGui::Button("新增组件")) ImGui::OpenPopup("##addhook");
        if (ImGui::BeginPopup("##addhook")) {
            bool anyAvailable = false;
            for (const auto& c : AvailableComponents()) {
                bool already = false;
                for (const auto& h : t.hooks)
                    if (h.name == c.name) { already = true; break; }
                if (already) continue;

                anyAvailable = true;
                if (ImGui::Selectable(c.name.c_str())) {
                    t.hooks.push_back(MakeHookFromCatalog(c));
                    dirty = true;
                }
            }
            if (!anyAvailable) ImGui::TextDisabled("没有可添加的组件");
            ImGui::EndPopup();
        }

        DrawLaunchProxy(st, t);

        if (ImGui::SmallButton("删除此规则")) removeAt = static_cast<int>(i);
        if (dirty) st.configDirty = true;

        ImGui::PopID();
        ImGui::Spacing();
    }

    if (removeAt >= 0) {
        cfg.targets.erase(cfg.targets.begin() + removeAt);
        st.configDirty = true;
    }
}

void DrawStatus(State& st)
{
    ImGui::SeparatorText("部署状态");
    ImGui::TextWrapped("配置文件：%s", Narrow(st.configPath).c_str());
    ImGui::Text("管理员权限：%s", waf::IsProcessElevated() ? "是" : "否（无法修改启动代理）");

    const struct {
        const wchar_t* label;
        std::wstring   path;
    } files[] = {
        { L"HookLauncher.exe", LauncherPath() },
        { L"injector.dll",     InjectorPath() },
    };
    for (const auto& f : files) {
        const bool exists = GetFileAttributesW(f.path.c_str()) != INVALID_FILE_ATTRIBUTES;
        ImGui::TextColored(exists ? kGreen : kRed, "%ls：%s", f.label,
                           exists ? "已就位" : "缺失（请与本程序放在同一目录）");
    }

    ImGui::SeparatorText("日志");
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& e : st.log)
        ImGui::TextUnformatted((e.time + "  " + e.text).c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace

void DrawUi(State& st)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("WorkAreaFullscreen##root", nullptr, flags);

    if (ImGui::Button("保存")) SaveConfig(st);
    ImGui::SameLine();
    if (ImGui::Button("重新加载")) ReloadConfig(st);
    ImGui::SameLine();
    if (st.configDirty) ImGui::TextColored(kAmber, "● 有未保存修改");
    else                ImGui::TextDisabled("已保存");
    ImGui::SameLine();
    if (ImGui::Button("收进托盘")) HideMainWindow(st);

    ImGui::Separator();

    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##left", ImVec2(availW * 0.56f, 0), ImGuiChildFlags_Border);
    DrawGlobal(st);
    DrawTargets(st);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawStatus(st);
    ImGui::EndChild();

    ImGui::End();
}

} // namespace waf::app
