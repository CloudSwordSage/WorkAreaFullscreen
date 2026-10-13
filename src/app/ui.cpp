// ui.cpp - ImGui 界面：规则编辑 + RVA 策略表 + 启动代理（IFEO）管理。
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
        int StrResizeCallback(ImGuiInputTextCallbackData * d) {
            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                std::string * s = static_cast<std::string *>(d->UserData);
                s->resize(static_cast<size_t>(d->BufTextLen));
                d->Buf = const_cast<char *>(s->c_str());
            }
            return 0;
        }

        bool InputString(const char * label, std::string & s) {
            if (s.capacity() < 32)
                s.reserve(32);
            return ImGui::InputText(
                label,
                const_cast<char *>(s.c_str()),
                s.capacity() + 1,
                ImGuiInputTextFlags_CallbackResize,
                StrResizeCallback,
                &s
            );
        }

        /** 去掉首尾空白；全空白时返回空串。 */
        std::string Trim(const std::string & s) {
            const size_t begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return std::string();
            const size_t end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        // 「RVA 配置」页的输入缓冲（只在主线程访问）：新增策略/组件，以及每个
        // 策略名的编辑缓冲（std::map 的键不可原地改，编辑后统一走
        // RenameStrategy）。
        std::string g_newStrategy;
        std::string g_newComponent;
        std::map<std::string, std::string> g_nameEdit;

        // 用组件目录里的默认配置构造一条 HookSetting：YAML 里的 scope
        // 提到结构字段，其余进 params。
        HookSetting MakeHookFromCatalog(const ComponentInfo & c) {
            HookSetting hs;
            hs.name = c.name;

            yamlmini::Node root;
            std::string err;
            if (!c.defaultConfigYaml.empty() &&
                yamlmini::Parse(c.defaultConfigYaml, root, err) &&
                root.kind == yamlmini::Node::Map) {
                for (const auto & kv : root.map) {
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

        void DrawTargets(State & st) {
            AppConfig & cfg = st.config;

            // callsite 需要策略表里有该组件 RVA；界面里删 RVA 或改策略后
            // 可能不成立，这里统一收窄为 global。
            if (EnforceCallsiteScopes(cfg))
                st.configDirty = true;

            if (ImGui::Checkbox("启用（HookLauncher 读取）", &cfg.enabled))
                st.configDirty = true;
            ImGui::Spacing();

            if (ImGui::Button("添加规则")) {
                cfg.targets.push_back(NewDefaultRule("game.exe"));
                st.configDirty = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("共 %zu 条", cfg.targets.size());

            int removeAt = -1;
            for (size_t i = 0; i < cfg.targets.size(); ++i) {
                TargetRule & t = cfg.targets[i];
                ImGui::PushID(static_cast<int>(i));

                char title[300];
                _snprintf_s(
                    title,
                    sizeof(title),
                    _TRUNCATE,
                    "%s%s###rule",
                    t.enabled ? "" : "[停用] ",
                    t.executable.c_str()
                );
                if (!ImGui::CollapsingHeader(
                        title,
                        ImGuiTreeNodeFlags_DefaultOpen
                    )) {
                    ImGui::PopID();
                    continue;
                }

                bool dirty = false;
                dirty |= ImGui::Checkbox("启用", &t.enabled);

                ImGui::SetNextItemWidth(240);
                dirty |= InputString("进程名", t.executable);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "进程映像名，如 stellaris.exe（不区分大小写）。"
                    );

                ImGui::SetNextItemWidth(150);
                int geo = (t.geometry == "monitor") ? 1 : 0;
                if (ImGui::Combo("几何", &geo, "work_area\0monitor\0")) {
                    t.geometry = geo ? "monitor" : "work_area";
                    dirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "work_area：窗口铺满工作区，保留任务栏等空间\n"
                        "monitor：显示器整块区域（等价不改写）"
                    );

                ImGui::SetNextItemWidth(150);
                InputString("策略", t.strategy);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "引用配置里 strategies: 下的调用点 RVA 表。"
                    );

                ImGui::SetNextItemWidth(150);
                if (ImGui::InputInt("注入延迟 (ms)", &t.injectDelayMs))
                    dirty = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "目标进程启动后多少毫秒再注入。默认 400。"
                    );
                if (t.injectDelayMs < 0) {
                    t.injectDelayMs = 0;
                    dirty = true;
                }

                ImGui::SetNextItemWidth(150);
                int refresh = (t.refreshDisplays == "sdl") ? 1 : 0;
                if (ImGui::Combo(
                        "刷新方式",
                        &refresh,
                        "不刷新\0SDL 引擎刷新\0"
                    )) {
                    t.refreshDisplays = refresh ? "sdl" : "none";
                    dirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "SDL 引擎刷新：注入后向目标的 SDL 窗口发一次\n"
                        "WM_DISPLAYCHANGE，让 SDL 按改写后的尺寸重建"
                        "显示器缓存。"
                    );

                dirty |= ImGui::Checkbox("校验调用点", &t.verifyCallsite);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "注入前按 RVA 校验指令形态；版本不匹配时回退为不改写。"
                    );

                ImGui::SeparatorText("hook 组件");

                int removeHook = -1;
                for (size_t h = 0; h < t.hooks.size(); ++h) {
                    HookSetting & hs = t.hooks[h];
                    ImGui::PushID(static_cast<int>(h));
                    dirty |= ImGui::Checkbox(hs.name.c_str(), &hs.enabled);
                    ImGui::SameLine(220);
                    ImGui::SetNextItemWidth(120);

                    // 策略表没有该组件的调用点 RVA 时不允许选 callsite：上面的
                    // 收窄已把这类取值改成 global，这里只禁用该项并说明原因。
                    const bool canCallsite = StrategyHasCallsites(
                        cfg.strategies,
                        t.strategy,
                        hs.name
                    );
                    if (ImGui::BeginCombo(
                            "##scope",
                            (hs.scope == "global") ? "global" : "callsite"
                        )) {
                        if (ImGui::Selectable(
                                "callsite",
                                hs.scope != "global",
                                canCallsite ? ImGuiSelectableFlags_None
                                            : ImGuiSelectableFlags_Disabled
                            )) {
                            hs.scope = "callsite";
                            dirty = true;
                        }
                        if (ImGui::Selectable("global", hs.scope == "global")) {
                            hs.scope = "global";
                            dirty = true;
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            canCallsite
                                ? "callsite：只对策略表记录的调用点生效\n"
                                  "global：对该 API 的所有调用生效"
                                : "策略表没有该组件的调用点 RVA，无法使用 "
                                  "callsite"
                        );
                    ImGui::SameLine();
                    if (ImGui::SmallButton("移除"))
                        removeHook = static_cast<int>(h);
                    if (!canCallsite) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("策略无该组件 RVA");
                    }
                    ImGui::PopID();
                }
                if (removeHook >= 0) {
                    t.hooks.erase(t.hooks.begin() + removeHook);
                    dirty = true;
                }

                if (t.hooks.empty())
                    ImGui::TextDisabled(
                        "该规则没有 hook 组件，不会改写任何东西"
                    );

                // 可添加的组件来自 injector.dll
                // 的导出目录，界面不硬编码组件知识
                if (ImGui::Button("新增组件"))
                    ImGui::OpenPopup("##addhook");
                if (ImGui::BeginPopup("##addhook")) {
                    bool anyAvailable = false;
                    for (const auto & c : AvailableComponents()) {
                        bool already = false;
                        for (const auto & h : t.hooks)
                            if (h.name == c.name) {
                                already = true;
                                break;
                            }
                        if (already)
                            continue;

                        anyAvailable = true;
                        if (ImGui::Selectable(c.name.c_str())) {
                            t.hooks.push_back(MakeHookFromCatalog(c));
                            dirty = true;
                        }
                    }
                    if (!anyAvailable)
                        ImGui::TextDisabled("没有可添加的组件");
                    ImGui::EndPopup();
                }

                if (ImGui::SmallButton("删除此规则"))
                    removeAt = static_cast<int>(i);
                if (dirty)
                    st.configDirty = true;

                ImGui::PopID();
                ImGui::Spacing();
            }

            if (removeAt >= 0) {
                cfg.targets.erase(cfg.targets.begin() + removeAt);
                st.configDirty = true;
            }
        }

        // ---- 页：RVA 配置 ----

        /**
         * 策略调用点表编辑：策略名 -> 组件 -> RVA 列表。
         *
         * 每个策略可折叠；策略名是就地编辑的输入框，失焦或回车后走
         * RenameStrategy（同步更新引用它的规则）。RVA
         * 列表为空表示该组件没有已知 调用点，callsite 组件会整条回退。
         */
        void DrawStrategies(State & st) {
            StrategyTable & table = st.config.strategies;

            ImGui::TextDisabled(
                "调用点 RVA 表：策略名 -> 组件 -> 调用指令 "
                "RVA（相对目标映像基址的\n"
                "十六进制地址）。规则的「策略」按名字引用一张表；scope="
                "callsite 的\n"
                "组件按表判定调用点，verify_callsite 按表校验指令形态。"
            );
            ImGui::Spacing();

            ImGui::SetNextItemWidth(200);
            InputString("##newstrategy", g_newStrategy);
            ImGui::SameLine();
            if (ImGui::Button("新增策略")) {
                const std::string name = Trim(g_newStrategy);
                if (!name.empty() && table.find(name) == table.end()) {
                    table[name];
                    st.configDirty = true;
                }
                g_newStrategy.clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("共 %zu 个策略", table.size());
            ImGui::Spacing();

            std::string removeStrategy;
            std::string renameOldKey;
            std::string renameNewName;
            for (auto & entry : table) {
                ImGui::PushID(entry.first.c_str());

                char title[320];
                _snprintf_s(
                    title,
                    sizeof(title),
                    _TRUNCATE,
                    "%s (%zu 组件)###strategy",
                    entry.first.c_str(),
                    entry.second.size()
                );
                if (!ImGui::CollapsingHeader(
                        title,
                        ImGuiTreeNodeFlags_DefaultOpen
                    )) {
                    ImGui::PopID();
                    continue;
                }

                bool dirty = false;

                // 策略名（map 的键）就地编辑，标签为「策略名」。
                // 失焦或回车时提交，改表推迟到循环外，避免遍历中修改容器。
                std::string & nameBuf = g_nameEdit[entry.first];
                if (nameBuf.empty())
                    nameBuf = entry.first;
                ImGui::SetNextItemWidth(200);
                InputString("策略名", nameBuf);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "失焦或回车后提交；改名会同步更新引用该策略的规则的"
                        "strategy。"
                    );
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    renameOldKey = entry.first;
                    renameNewName = Trim(nameBuf);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("删除策略"))
                    removeStrategy = entry.first;

                StrategyCallsites & components = entry.second;

                std::string removeComponent;
                for (auto & comp : components) {
                    ImGui::PushID(comp.first.c_str());

                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(comp.first.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("移除组件"))
                        removeComponent = comp.first;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "组件名须与该 hook 组件的 name 一致。"
                        );

                    std::vector<ULONG_PTR> & rvas = comp.second;
                    ImGui::Indent();
                    int removeRva = -1;
                    for (size_t i = 0; i < rvas.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::SetNextItemWidth(120);
                        ImU64 v = static_cast<ImU64>(rvas[i]);
                        if (ImGui::InputScalar(
                                "##rva",
                                ImGuiDataType_U64,
                                &v,
                                nullptr,
                                nullptr,
                                "%llX",
                                ImGuiInputTextFlags_CharsHexadecimal
                            )) {
                            rvas[i] = static_cast<ULONG_PTR>(v);
                            dirty = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("移除"))
                            removeRva = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    if (removeRva >= 0) {
                        rvas.erase(rvas.begin() + removeRva);
                        dirty = true;
                    }
                    if (ImGui::SmallButton("新增 RVA")) {
                        rvas.push_back(0);
                        dirty = true;
                    }
                    if (rvas.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("（空表：该组件会回退为不改写）");
                    }
                    ImGui::Unindent();
                    ImGui::Spacing();

                    ImGui::PopID();
                }

                if (!removeComponent.empty()) {
                    components.erase(removeComponent);
                    dirty = true;
                }

                if (ImGui::Button("新增组件"))
                    ImGui::OpenPopup("##addcomp");
                if (ImGui::BeginPopup("##addcomp")) {
                    ImGui::TextDisabled(
                        "组件名须与 hook 组件名一致，如 GetDeviceCaps"
                    );
                    ImGui::SetNextItemWidth(220);
                    InputString("##newcomponent", g_newComponent);
                    if (ImGui::Button("添加")) {
                        const std::string name = Trim(g_newComponent);
                        if (!name.empty() &&
                            components.find(name) == components.end()) {
                            components[name];
                            dirty = true;
                        }
                        g_newComponent.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                if (dirty)
                    st.configDirty = true;

                ImGui::Spacing();
                ImGui::PopID();
            }

            if (!removeStrategy.empty()) {
                table.erase(removeStrategy);
                g_nameEdit.erase(removeStrategy);
                st.configDirty = true;
            }

            // 改名：成功后把编辑缓冲迁到新键；失败（空名/重名/未变）则回显真实名。
            if (!renameOldKey.empty()) {
                if (RenameStrategy(st.config, renameOldKey, renameNewName)) {
                    st.configDirty = true;
                    g_nameEdit[renameNewName] =
                        std::move(g_nameEdit[renameOldKey]);
                    g_nameEdit.erase(renameOldKey);
                } else {
                    g_nameEdit[renameOldKey] = renameOldKey;
                }
            }

            // 丢掉已不存在的策略遗留的编辑缓冲（外部热重载或删除之后）。
            for (auto it = g_nameEdit.begin(); it != g_nameEdit.end();)
                if (table.find(it->first) == table.end())
                    it = g_nameEdit.erase(it);
                else
                    ++it;

            if (table.empty())
                ImGui::TextDisabled(
                    "还没有策略表。scope=callsite 的组件需要在这里定义调用点。"
                );
        }

        // ---- 页：IFEO 管理 ----

        void DrawIfeoManager(State & st) {
            ImGui::TextDisabled(
                "挂载后，任何程序启动该可执行文件都会先经过 HookLauncher，\n"
                "在目标执行第一条指令之前完成注入。需要管理员权限（点按钮时会请"
                "求）。"
            );
            ImGui::Spacing();

            if (st.config.targets.empty()) {
                ImGui::TextDisabled(
                    "配置里还没有目标应用，请先在「规则」页添加。"
                );
                return;
            }

            const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##ifeo", 2, flags)) {
                ImGui::TableSetupColumn(
                    "可执行文件",
                    ImGuiTableColumnFlags_WidthStretch,
                    3.0f
                );
                ImGui::TableSetupColumn(
                    "操作",
                    ImGuiTableColumnFlags_WidthStretch,
                    1.0f
                );
                ImGui::TableHeadersRow();

                for (const auto & t : st.config.targets) {
                    const std::string exeName = waf::ExeNameOf(t.executable);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (exeName.empty())
                        ImGui::TextDisabled("（未填写进程名）");
                    else
                        ImGui::TextUnformatted(exeName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(exeName.c_str());
                    if (exeName.empty()) {
                        ImGui::TextDisabled("—");
                    } else if (IsLaunchProxyOn(exeName)) {
                        if (ImGui::Button("卸载"))
                            ApplyLaunchProxy(exeName, false);
                    } else {
                        if (ImGui::Button("挂载"))
                            ApplyLaunchProxy(exeName, true);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

    } // namespace

    void DrawUi(State & st) {
        ImGuiIO & io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("WorkAreaFullscreen##root", nullptr, flags);

        if (ImGui::Button("保存"))
            SaveConfig(st);
        ImGui::SameLine();
        if (ImGui::Button("重新加载"))
            ReloadConfig(st);
        ImGui::SameLine();
        if (st.configDirty)
            ImGui::TextColored(kAmber, "● 有未保存修改");
        else
            ImGui::TextDisabled("已保存");
        ImGui::SameLine();
        ImGui::TextDisabled("配置：%s", Narrow(st.configPath).c_str());

        ImGui::Separator();

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("规则")) {
                DrawTargets(st);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("RVA 配置")) {
                DrawStrategies(st);
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
