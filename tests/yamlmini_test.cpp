// yamlmini 解析器与配置模型的往返自检。只做 assert，不引入测试框架。
#include <cassert>
#include <cstdio>
#include <string>

#include "waf/config.h"
#include "waf/runtime.h"
#include "waf/yamlmini.h"

using yamlmini::Node;

static const char * kFull = R"(# 顶层开关
enabled: true

targets:
  - executable: stellaris.exe
    enabled: true

    strategy: sdl2-gdi
    geometry: work_area          # 行尾注释

    hooks:
      GetDeviceCaps:
        enabled: true
        indices:
          - HORZRES
          - VERTRES
        scope: callsite

      GetMonitorInfoW:
        enabled: true
        fields:
          rcMonitor: rcWork
        scope: callsite

    safety:
      fallback: passthrough
      verify_callsite: true
)";

static void TestParse() {
    Node root;
    std::string err;
    const bool ok = yamlmini::Parse(kFull, root, err);
    if (!ok)
        printf("parse failed: %s\n", err.c_str());
    assert(ok);

    assert(root.kind == Node::Map);
    assert(root.find("enabled")->toBool(false) == true);

    const Node * targets = root.find("targets");
    assert(targets && targets->kind == Node::Seq && targets->seq.size() == 1);

    const Node & t = targets->seq[0];
    assert(t.find("executable")->str() == "stellaris.exe");
    assert(t.find("enabled")->toBool(false) == true);
    assert(t.find("strategy")->str() == "sdl2-gdi");
    assert(t.find("geometry")->str() == "work_area"); // 行尾注释已剥离

    const Node * hooks = t.find("hooks");
    assert(hooks && hooks->kind == Node::Map && hooks->map.size() == 2);

    const Node * gdc = hooks->find("GetDeviceCaps");
    assert(gdc && gdc->find("enabled")->toBool(false) == true);
    const Node * idx = gdc->find("indices");
    assert(idx && idx->kind == Node::Seq && idx->seq.size() == 2);
    assert(idx->seq[0].str() == "HORZRES" && idx->seq[1].str() == "VERTRES");
    assert(gdc->find("scope")->str() == "callsite");

    const Node * gmi = hooks->find("GetMonitorInfoW");
    const Node * fld = gmi->find("fields");
    assert(fld && fld->kind == Node::Map && fld->map.size() == 1);
    assert(
        fld->map[0].first == "rcMonitor" && fld->map[0].second.str() == "rcWork"
    );

    const Node * safety = t.find("safety");
    assert(safety->find("fallback")->str() == "passthrough");
    assert(safety->find("verify_callsite")->toBool(false) == true);
}

// Dump 的输出必须能被 Parse 读回，且关键字段不变。
static void TestDumpRoundTrip() {
    Node root;
    std::string err;
    assert(yamlmini::Parse(kFull, root, err));

    const std::string dumped = yamlmini::Dump(root);
    Node again;
    std::string err2;
    if (!yamlmini::Parse(dumped, again, err2))
        printf(
            "dump reparse failed: %s\n---\n%s\n",
            err2.c_str(),
            dumped.c_str()
        );
    assert(yamlmini::Parse(dumped, again, err2));

    assert(again.find("enabled")->toBool(false) == true);
    const Node * t = &again.find("targets")->seq[0];
    assert(t->find("executable")->str() == "stellaris.exe");
    assert(t->find("geometry")->str() == "work_area");
    assert(
        t->find("hooks")->find("GetDeviceCaps")->find("scope")->str() ==
        "callsite"
    );
    assert(
        t->find("hooks")
            ->find("GetMonitorInfoW")
            ->find("fields")
            ->map[0]
            .second.str() == "rcWork"
    );
    assert(t->find("safety")->find("verify_callsite")->toBool(false) == true);
}

static void TestScalarsAndQuotes() {
    Node n;
    std::string err;
    const bool ok = yamlmini::Parse(
        "a: 42\nb: \"x:y\"\nc: 'it''s'\nd: OFF\ne: off-chance\nf:\n",
        n,
        err
    );
    if (!ok)
        printf("parse failed: %s\n", err.c_str());
    assert(ok);
    assert(n.find("a")->toInt(-1) == 42);
    assert(n.find("b")->str() == "x:y");   // 引号内的 ':' 不是键分隔符
    assert(n.find("c")->str() == "it''s"); // 仅剥离外围引号
    assert(n.find("d")->toBool(true) == false);
    assert(n.find("e")->toBool(true) == true); // 非布尔词回退默认值
    assert(n.find("f")->kind == Node::Null);
    assert(n.find("missing") == nullptr);

    // 需要引号保护的值往返后仍是标量而不是布尔
    Node q;
    assert(yamlmini::Parse("v: \"true\"\n", q, err));
    assert(yamlmini::Dump(q).find("\"true\"") != std::string::npos);
}

static void TestAppConfig() {
    waf::AppConfig cfg = waf::DefaultConfig();

    const std::string text = cfg.Dump("# header\n");
    assert(text.compare(0, 9, "# header\n") == 0);

    waf::AppConfig back;
    std::string err;
    if (!waf::AppConfig::Parse(text, back, err))
        printf("config parse: %s\n", err.c_str());
    assert(waf::AppConfig::Parse(text, back, err));

    assert(back.enabled == true);
    assert(back.targets.size() == 1);
    assert(back.targets[0].executable == "stellaris.exe");
    assert(back.targets[0].geometry == "work_area");
    assert(back.targets[0].hooks.size() == 3);
    assert(back.targets[0].injectDelayMs == 400);     // 默认注入延迟
    assert(back.targets[0].refreshDisplays == "sdl"); // 默认规则的刷新方式
    assert(back.targets[0].hooks[0].name == "GetMonitorInfoW");
    assert(back.targets[0].hooks[1].name == "GetDeviceCaps");
    assert(back.targets[0].hooks[2].name == "EnumDisplaySettingsW");
    assert(back.targets[0].hooks[0].params.find("fields") != nullptr);
    assert(back.targets[0].hooks[1].params.find("indices") != nullptr);

    // 策略调用点表来自配置，往返后 RVA 保持十六进制数值
    assert(back.strategies.count("sdl2-gdi") == 1);
    const auto & gmi = back.strategies.at("sdl2-gdi").at("GetMonitorInfoW");
    assert(gmi.size() == 4);
    assert(gmi[0] == 0x1DFAAF5 && gmi[3] == 0x1DFE184);
    const auto & gdc = back.strategies.at("sdl2-gdi").at("GetDeviceCaps");
    assert(gdc.size() == 2);
    assert(gdc[0] == 0x1DFBB35 && gdc[1] == 0x1DFBB45);

    // 规则匹配：不区分大小写，且只看启用中的规则
    assert(back.FindRule("STELLARIS.EXE") != nullptr);
    assert(back.FindRule("other.exe") == nullptr);
    back.targets[0].enabled = false;
    assert(back.FindRule("stellaris.exe") == nullptr);
}

static void TestErrors() {
    Node n;
    std::string err;
    assert(
        !yamlmini::Parse("targets:\n  - executable stellaris.exe\n", n, err)
    );
    assert(!err.empty());

    waf::AppConfig cfg;
    std::string cerr;
    assert(!waf::AppConfig::Parse("targets: 3\n", cfg, cerr));

    // strategies 段里的 RVA 必须是十六进制/十进制数值
    assert(!waf::AppConfig::Parse(
        "strategies:\n  sdl2-gdi:\n    GetDeviceCaps:\n      - nope\n",
        cfg,
        cerr
    ));
    assert(!waf::AppConfig::Parse("strategies: 3\n", cfg, cerr));
}

// 策略调用点表：从配置装载 -> 按策略/组件查询 -> 调用点命中判定。
static void TestStrategyTable() {
    waf::AppConfig cfg = waf::DefaultConfig();
    waf::HkLoadStrategies(cfg.strategies);

    const std::vector<ULONG_PTR> & gmi =
        waf::HkStrategyCallsites("sdl2-gdi", "GetMonitorInfoW");
    assert(gmi.size() == 4);
    assert(gmi[0] == 0x1DFAAF5 && gmi[3] == 0x1DFE184);
    assert(waf::HkStrategyCallsites("sdl2-gdi", "GetDeviceCaps").size() == 2);
    // 未知策略/组件返回空表
    assert(waf::HkStrategyCallsites("nope", "GetMonitorInfoW").empty());
    assert(waf::HkStrategyCallsites("sdl2-gdi", "nope").empty());

    waf::HkEnv env;
    env.targetMod = reinterpret_cast<HMODULE>(0x10000000);
    env.strategy = "sdl2-gdi";
    // 命中：返回地址 = 模块基址 + 调用指令 RVA + 6（call qword ptr
    // [rip+disp32]）
    void * hit = reinterpret_cast<void *>(
        static_cast<ULONG_PTR>(0x10000000) + 0x1DFAAF5 + 6
    );
    assert(waf::HkFromKnownCallsite(env, "GetMonitorInfoW", hit));
    void * miss = reinterpret_cast<void *>(
        static_cast<ULONG_PTR>(0x10000000) + 0x1DFAAF5
    );
    assert(!waf::HkFromKnownCallsite(env, "GetMonitorInfoW", miss));

    // 重新装载后旧表失效（换版本时以配置为准）
    waf::AppConfig other;
    other.strategies["sdl2-gdi"]["GetDeviceCaps"] = {0x1000};
    waf::HkLoadStrategies(other.strategies);
    assert(waf::HkStrategyCallsites("sdl2-gdi", "GetMonitorInfoW").empty());
    assert(waf::HkStrategyCallsites("sdl2-gdi", "GetDeviceCaps").size() == 1);
}

// 空容器（Dump 会写成 [] / {}）必须能被读回：界面允许把规则/策略/RVA 删空。
static void TestEmptyContainers() {
    Node n;
    std::string err;
    assert(yamlmini::Parse("a: []\nb: {}\nseq:\n  - {}\n  - []\n", n, err));
    assert(n.find("a")->kind == Node::Seq && n.find("a")->seq.empty());
    assert(n.find("b")->kind == Node::Map && n.find("b")->map.empty());
    assert(n.find("seq")->kind == Node::Seq && n.find("seq")->seq.size() == 2);
    assert(n.find("seq")->seq[0].kind == Node::Map);
    assert(n.find("seq")->seq[1].kind == Node::Seq);
    assert(yamlmini::Dump(n).find("a: []") != std::string::npos);

    // 配置模型层：删空规则、空策略、空 RVA 列表都要能往返
    waf::AppConfig cfg;
    cfg.strategies["newstrat"] = waf::StrategyCallsites{};
    cfg.strategies["sdl2-gdi"]["GetDeviceCaps"] = {};
    waf::AppConfig back;
    assert(waf::AppConfig::Parse(cfg.Dump(), back, err));
    assert(back.targets.empty());
    assert(back.strategies.size() == 2);
    assert(back.strategies.at("newstrat").empty());
    assert(back.strategies.at("sdl2-gdi").at("GetDeviceCaps").empty());
}

// 策略改名：键迁移、表内容保留、引用同步，非法改名被拒绝。
static void TestRenameStrategy() {
    waf::AppConfig cfg = waf::DefaultConfig();
    assert(cfg.strategies.count("sdl2-gdi") == 1);
    assert(cfg.targets[0].strategy == "sdl2-gdi");

    assert(!waf::RenameStrategy(cfg, "sdl2-gdi", "sdl2-gdi")); // 同名
    assert(!waf::RenameStrategy(cfg, "sdl2-gdi", ""));         // 空名
    assert(!waf::RenameStrategy(cfg, "missing", "x"));         // 源不存在

    cfg.strategies["other"] = {};
    assert(!waf::RenameStrategy(cfg, "sdl2-gdi", "other")); // 目标已存在
    assert(cfg.strategies.count("sdl2-gdi") == 1);

    assert(waf::RenameStrategy(cfg, "sdl2-gdi", "sdl2-gdi-v2"));
    assert(cfg.strategies.count("sdl2-gdi") == 0);
    assert(cfg.strategies.count("sdl2-gdi-v2") == 1);
    assert(cfg.strategies.at("sdl2-gdi-v2").at("GetMonitorInfoW").size() == 4);
    assert(cfg.strategies.at("sdl2-gdi-v2").at("GetDeviceCaps").size() == 2);
    assert(cfg.targets[0].strategy == "sdl2-gdi-v2"); // 引用已同步
}

int main() {
    TestParse();
    TestDumpRoundTrip();
    TestScalarsAndQuotes();
    TestAppConfig();
    TestStrategyTable();
    TestEmptyContainers();
    TestRenameStrategy();
    TestErrors();
    printf("yamlmini + config self-check: OK\n");
    return 0;
}
