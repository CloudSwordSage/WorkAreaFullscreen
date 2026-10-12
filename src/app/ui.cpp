// ui.cpp - ImGui 界面：规则编辑 + 启动代理（IFEO）管理。
//
// 本程序不监听进程、不注入；「IFEO 管理」页负责把启动代理挂到目标上。
#include "app/app.h"

#include <imgui.h>

#include <string>

#include "waf/ifeo.h"

namespace waf::app {
namespace {

const ImVec4 kGreen = ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
const ImVec4 kAmber = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);

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

// ---- 页：规则 ----

void DrawTargets(State& st)
{
    AppConfig& cfg = st.config;

    if (ImGui::Checkbox("启用（HookLauncher 读取）", &cfg.enabled)) st.configDirty = true;
    ImGui::Spacing();

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
            t.geometry = geo ? "monitor" : "work_area";
            dirty      = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("work_area：窗口铺满工作区，保留任务栏等空间\n"
                              "monitor：显示器整块区域（等价不改写）");

        ImGui::SetNextItemWidth(150);
        InputString("策略", t.strategy);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("提供与目标版本绑定的调用点 RVA 表。");

        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("注入延迟 (ms)", &t.injectDelayMs)) dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("目标进程启动后多少毫秒再注入。默认 400。");
        if (t.injectDelayMs < 0) { t.injectDelayMs = 0; dirty = true; }

        dirty |= ImGui::Checkbox("校验调用点", &t.verifyCallsite);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("注入前按 RVA 校验指令形态；版本不匹配时回退为不改写。");

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

// ---- 页：IFEO 管理 ----

void DrawIfeoManager(State& st)
{
    ImGui::TextDisabled("挂载后，任何程序启动该可执行文件都会先经过 HookLauncher，\n"
                        "在目标执行第一条指令之前完成注入。需要管理员权限（点按钮时会请求）。");
    ImGui::Spacing();

    if (st.config.targets.empty()) {
        ImGui::TextDisabled("配置里还没有目标应用，请先在「规则」页添加。");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##ifeo", 2, flags)) {
        ImGui::TableSetupColumn("可执行文件", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (const auto& t : st.config.targets) {
            const std::string exeName = waf::ExeNameOf(t.executable);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (exeName.empty()) ImGui::TextDisabled("（未填写进程名）");
            else                 ImGui::TextUnformatted(exeName.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(exeName.c_str());
            if (exeName.empty()) {
                ImGui::TextDisabled("—");
            } else if (IsLaunchProxyOn(exeName)) {
                if (ImGui::Button("卸载")) ApplyLaunchProxy(exeName, false);
            } else {
                if (ImGui::Button("挂载")) ApplyLaunchProxy(exeName, true);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
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
    ImGui::TextDisabled("配置：%s", Narrow(st.configPath).c_str());

    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("规则")) {
            DrawTargets(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("IFEO 管理")) {
            DrawIfeoManager(st);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace waf::app
