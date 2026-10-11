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
        ImGui::SetTooltip("发现目标进程后等待多久再注入。\n立即注入会让部分引擎在初始化期崩溃，故默认 400 ms。");

    ImGui::SetNextItemWidth(150);
    dirty |= ImGui::InputInt("扫描间隔 (ms)", &cfg.scanIntervalMs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("轮询进程列表的间隔。");

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
        for (size_t h = 0; h < t.hooks.size(); ++h) {
            HookSetting& hs = t.hooks[h];
            ImGui::PushID(static_cast<int>(h));
            dirty |= ImGui::Checkbox(hs.name.c_str(), &hs.enabled);
            ImGui::SameLine(220);
            ImGui::SetNextItemWidth(140);
            int scope = (hs.scope == "global") ? 1 : 0;
            if (ImGui::Combo("##scope", &scope, "callsite\0global\0")) {
                hs.scope = scope ? "global" : "callsite";
                dirty    = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("callsite：只对策略表记录的调用点生效\n"
                                  "global：对该 API 的所有调用生效");
            ImGui::PopID();
        }
        if (t.hooks.empty())
            ImGui::TextDisabled("该规则没有 hook 组件，不会改写任何东西");

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
