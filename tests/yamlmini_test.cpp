// yamlmini 自检：覆盖本仓库配置出现的全部语法形态，断言解析结果。
// 只做 assert，不引入测试框架。
#include <cassert>
#include <cstdio>
#include <string>

#include "hookkit/yamlmini.h"

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

static void TestFull()
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
}

static void TestErrors()
{
    Node        n;
    std::string err;
    assert(!yamlmini::Parse("targets:\n  - executable stellaris.exe\n", n, err));
    assert(!err.empty());
}

int main()
{
    TestFull();
    TestScalarsAndQuotes();
    TestErrors();
    printf("yamlmini self-check: OK\n");
    return 0;
}
