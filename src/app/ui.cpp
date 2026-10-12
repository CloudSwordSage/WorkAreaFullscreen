// ui.cpp - ImGui 界面：全局设置、目标规则编辑、运行状态与日志。
#include "app.h"

#include <imgui.h>

#include <string>

namespace waf::app {

namespace {

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
    AppConfig& cfg = st.config;
    bool       dirty = false;

    ImGui::SeparatorText("全局");
    dirty |= ImGui::Checkbox("启用监听与注入", &cfg.enabled);
    dirty |= ImGui::Checkbox("关闭窗口时收进托盘", &cfg.minimizeToTray);

    ImGui::SetNextItemWidth(150);
    dirty |= ImGui::InputInt("注入延迟 (ms)", &cfg.injectDelayMs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("目标进程“启动后”多少毫秒注入。\n必须在目标完成显示初始化之前，否则改不到窗口尺寸。\n立即注入会让部分引擎崩溃，故不要设得过小。");

    ImGui::SetNextItemWidth(150);
    dirty |= ImGui::InputInt("扫描间隔 (ms)", &cfg.scanIntervalMs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("轮询进程列表的间隔。\n越小越能赶在目标初始化完成前发现它；\n设得太大会错过注入时机。");

    if (cfg.injectDelayMs < 0)    { cfg.injectDelayMs = 0;   dirty = true; }
    if (cfg.scanIntervalMs < 100) { cfg.scanIntervalMs = 100; dirty = true; }

    if (dirty) st.configDirty = true;
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
            ImGui::SetTooltip("注入前按 RVA 校验指令字节与 IAT 槽；\n版本不匹配时回退为不改写。");

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
    ImGui::SeparatorText("运行状态");
    ImGui::TextWrapped("配置文件：%s", Narrow(st.configPath).c_str());
    ImGui::Text("进程监听：轮询（间隔 %d ms）%s", st.config.scanIntervalMs,
                ProcessWatcherActive() ? " + WMI 事件" : "");
    if (ProcessWatcherActive())
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "WMI 进程事件订阅已生效");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "提示：以管理员身份运行可额外启用 WMI 进程事件订阅");
    ImGui::Text("已注入进程：%zu", st.injectedPids.size());
    for (DWORD pid : st.injectedPids) ImGui::BulletText("PID %lu", pid);
    if (!st.pending.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "等待注入：%zu", st.pending.size());
    if (!st.injecting.empty())
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "正在注入：%zu", st.injecting.size());

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
    if (st.configDirty) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "● 有未保存修改");
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
