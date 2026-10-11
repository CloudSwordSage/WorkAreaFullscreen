// yamlmini 解析器与配置模型的往返自检。只做 assert，不引入测试框架。
#include <cassert>
#include <cstdio>
#include <string>

#include "waf/config.h"
#include "waf/yamlmini.h"

using yamlmini::Node;

static const char* kFull = R"(# 顶层开关
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

static void TestParse()
{
    Node        root;
    std::string err;
    const bool  ok = yamlmini::Parse(kFull, root, err);
    if (!ok) printf("parse failed: %s\n", err.c_str());
    assert(ok);

    assert(root.kind == Node::Map);
    assert(root.find("enabled")->toBool(false) == true);

    const Node* targets = root.find("targets");
    assert(targets && targets->kind == Node::Seq && targets->seq.size() == 1);

    const Node& t = targets->seq[0];
    assert(t.find("executable")->str() == "stellaris.exe");
    assert(t.find("enabled")->toBool(false) == true);
    assert(t.find("strategy")->str() == "sdl2-gdi");
    assert(t.find("geometry")->str() == "work_area");   // 行尾注释已剥离

    const Node* hooks = t.find("hooks");
    assert(hooks && hooks->kind == Node::Map && hooks->map.size() == 2);

    const Node* gdc = hooks->find("GetDeviceCaps");
    assert(gdc && gdc->find("enabled")->toBool(false) == true);
    const Node* idx = gdc->find("indices");
    assert(idx && idx->kind == Node::Seq && idx->seq.size() == 2);
    assert(idx->seq[0].str() == "HORZRES" && idx->seq[1].str() == "VERTRES");
    assert(gdc->find("scope")->str() == "callsite");

    const Node* gmi = hooks->find("GetMonitorInfoW");
    const Node* fld = gmi->find("fields");
    assert(fld && fld->kind == Node::Map && fld->map.size() == 1);
    assert(fld->map[0].first == "rcMonitor" && fld->map[0].second.str() == "rcWork");

    const Node* safety = t.find("safety");
    assert(safety->find("fallback")->str() == "passthrough");
    assert(safety->find("verify_callsite")->toBool(false) == true);
}

// Dump 的输出必须能被 Parse 读回，且关键字段不变。
static void TestDumpRoundTrip()
{
    Node        root;
    std::string err;
    assert(yamlmini::Parse(kFull, root, err));

    const std::string dumped = yamlmini::Dump(root);
    Node              again;
    std::string       err2;
    if (!yamlmini::Parse(dumped, again, err2))
        printf("dump reparse failed: %s\n---\n%s\n", err2.c_str(), dumped.c_str());
    assert(yamlmini::Parse(dumped, again, err2));

    assert(again.find("enabled")->toBool(false) == true);
    const Node* t = &again.find("targets")->seq[0];
    assert(t->find("executable")->str() == "stellaris.exe");
    assert(t->find("geometry")->str() == "work_area");
    assert(t->find("hooks")->find("GetDeviceCaps")->find("scope")->str() == "callsite");
    assert(t->find("hooks")->find("GetMonitorInfoW")->find("fields")->map[0].second.str() == "rcWork");
    assert(t->find("safety")->find("verify_callsite")->toBool(false) == true);
}

static void TestScalarsAndQuotes()
{
    Node        n;
    std::string err;
    const bool  ok = yamlmini::Parse(
        "a: 42\nb: \"x:y\"\nc: 'it''s'\nd: OFF\ne: off-chance\nf:\n", n, err);
    if (!ok) printf("parse failed: %s\n", err.c_str());
    assert(ok);
    assert(n.find("a")->toInt(-1) == 42);
    assert(n.find("b")->str() == "x:y");       // 引号内的 ':' 不是键分隔符
    assert(n.find("c")->str() == "it''s");     // 仅剥离外围引号
    assert(n.find("d")->toBool(true) == false);
    assert(n.find("e")->toBool(true) == true); // 非布尔词回退默认值
    assert(n.find("f")->kind == Node::Null);
    assert(n.find("missing") == nullptr);

    // 需要引号保护的值往返后仍是标量而不是布尔
    Node        q;
    assert(yamlmini::Parse("v: \"true\"\n", q, err));
    assert(yamlmini::Dump(q).find("\"true\"") != std::string::npos);
}

static void TestAppConfig()
{
    waf::AppConfig cfg = waf::DefaultConfig();
    cfg.injectDelayMs  = 250;
    cfg.minimizeToTray = false;

    const std::string text = cfg.Dump("# header\n");
    assert(text.compare(0, 9, "# header\n") == 0);

    waf::AppConfig back;
    std::string    err;
    if (!waf::AppConfig::Parse(text, back, err)) printf("config parse: %s\n", err.c_str());
    assert(waf::AppConfig::Parse(text, back, err));

    assert(back.injectDelayMs == 250);
    assert(back.minimizeToTray == false);
    assert(back.targets.size() == 1);
    assert(back.targets[0].executable == "stellaris.exe");
    assert(back.targets[0].geometry == "work_area");
    assert(back.targets[0].hooks.size() == 2);
    assert(back.targets[0].hooks[0].name == "GetMonitorInfoW");
    assert(back.targets[0].hooks[1].name == "GetDeviceCaps");
    assert(back.targets[0].hooks[0].params.find("fields") != nullptr);
    assert(back.targets[0].hooks[1].params.find("indices") != nullptr);

    // 规则匹配：不区分大小写，且只看启用中的规则
    assert(back.FindRule("STELLARIS.EXE") != nullptr);
    assert(back.FindRule("other.exe") == nullptr);
    back.targets[0].enabled = false;
    assert(back.FindRule("stellaris.exe") == nullptr);
}

static void TestErrors()
{
    Node        n;
    std::string err;
    assert(!yamlmini::Parse("targets:\n  - executable stellaris.exe\n", n, err));
    assert(!err.empty());

    waf::AppConfig cfg;
    std::string    cerr;
    assert(!waf::AppConfig::Parse("targets: 3\n", cfg, cerr));
}

int main()
{
    TestParse();
    TestDumpRoundTrip();
    TestScalarsAndQuotes();
    TestAppConfig();
    TestErrors();
    printf("yamlmini + config self-check: OK\n");
    return 0;
}
