// yamlmini.cpp - 最小 YAML 子集解析器实现，见 yamlmini.h。
#include "waf/yamlmini.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace yamlmini {
namespace {

struct Line {
    int         indent = 0;
    int         no     = 0;   // 1-based 行号，用于报错
    std::string text;         // 已去注释并 trim
};

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

bool StartsWith(const std::string& s, const char* p)
{
    return s.compare(0, strlen(p), p) == 0;
}

// 去掉行内注释：仅当 '#' 位于行首或紧跟空白时才是注释起点（引号内的 '#' 不算）。
std::string StripComment(const std::string& s)
{
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    }
    return s;
}

std::string Unquote(const std::string& s)
{
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

bool IsSeqItem(const std::string& t)
{
    return t == "-" || StartsWith(t, "- ");
}

// 拆 "key: value"：key 非空，且 ':' 之后是空白或行尾。
bool SplitKey(const std::string& t, std::string& key, std::string& rest)
{
    for (size_t c = t.find(':'); c != std::string::npos; c = t.find(':', c + 1)) {
        if (c > 0 && (c + 1 == t.size() || t[c + 1] == ' ' || t[c + 1] == '\t')) {
            key  = Trim(t.substr(0, c));
            rest = Trim(t.substr(c + 1));
            return !key.empty();
        }
    }
    return false;
}

class Parser {
public:
    explicit Parser(const std::vector<Line>& lines) : L_(lines) {}

    bool Run(Node& out, std::string& err)
    {
        if (L_.empty()) { out.kind = Node::Null; return true; }
        return Block(L_[0].indent, out, err);
    }

private:
    const std::vector<Line>& L_;
    size_t                   i_ = 0;

    bool Fail(std::string& err, int line, const char* what)
    {
        err = "line " + std::to_string(line) + ": " + what;
        return false;
    }

    bool Block(int indent, Node& out, std::string& err)
    {
        if (i_ >= L_.size()) return true;
        if (IsSeqItem(L_[i_].text)) return Seq(indent, out, err);

        out.kind = Node::Map;
        while (i_ < L_.size() && L_[i_].indent == indent && !IsSeqItem(L_[i_].text))
            if (!MapEntry(indent, out, err)) return false;
        return true;
    }

    // 解析一行 "key: value"，i_ 指向该行；子块或标量写入 out.map。
    bool MapEntry(int indent, Node& out, std::string& err)
    {
        std::string key, rest;
        if (!SplitKey(L_[i_].text, key, rest))
            return Fail(err, L_[i_].no, "expected 'key: value'");
        ++i_;

        Node child;
        if (rest.empty()) {
            if (i_ < L_.size() && L_[i_].indent > indent)
                if (!Block(L_[i_].indent, child, err)) return false;
        } else {
            child.kind   = Node::Scalar;
            child.scalar = Unquote(rest);
        }
        out.kind = Node::Map;
        out.map.emplace_back(key, std::move(child));
        return true;
    }

    bool Seq(int indent, Node& out, std::string& err)
    {
        out.kind = Node::Seq;
        while (i_ < L_.size() && L_[i_].indent == indent && IsSeqItem(L_[i_].text)) {
            std::string rest = Trim(L_[i_].text.substr(1));
            ++i_;

            Node item;
            if (rest.empty()) {
                if (i_ < L_.size() && L_[i_].indent > indent)
                    if (!Block(L_[i_].indent, item, err)) return false;
            } else {
                std::string key, val;
                if (SplitKey(rest, key, val)) {
                    // 紧凑映射项 "- key: value"；后续键与项内容对齐（"- " 占 2 列）。
                    item.kind = Node::Map;
                    Node first;
                    if (val.empty()) {
                        if (i_ < L_.size() && L_[i_].indent > indent)
                            if (!Block(L_[i_].indent, first, err)) return false;
                    } else {
                        first.kind   = Node::Scalar;
                        first.scalar = Unquote(val);
                    }
                    item.map.emplace_back(key, std::move(first));

                    const int child = indent + 2;
                    while (i_ < L_.size() && L_[i_].indent == child && !IsSeqItem(L_[i_].text))
                        if (!MapEntry(child, item, err)) return false;
                } else {
                    item.kind   = Node::Scalar;
                    item.scalar = Unquote(rest);
                }
            }
            out.seq.push_back(std::move(item));
        }
        return true;
    }
};

// ---- 序列化 ----

bool NeedsQuote(const std::string& s)
{
    if (s.empty()) return true;
    if (s.front() == ' ' || s.back() == ' ') return true;
    if (s.front() == '-' || s.front() == '#' || s.front() == ':' || s.front() == '[' || s.front() == '{')
        return true;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (isalnum(u)) continue;
        if (c == '.' || c == '_' || c == '-' || c == '/' || c == '\\' || c == ' ') continue;
        return true;   // ':' 会歧义成键分隔符，'#' 会歧义成注释
    }
    // 与布尔/空字面量同形时也加引号，避免读回时被 toBool/toInt 当成别的类型。
    return s == "true" || s == "false" || s == "yes" || s == "no" ||
           s == "on" || s == "off" || s == "null" || s == "~";
}

std::string ScalarText(const std::string& s)
{
    return NeedsQuote(s) ? ("\"" + s + "\"") : s;
}

bool EmptyNode(const Node& n)
{
    if (n.kind == Node::Map) return n.map.empty();
    if (n.kind == Node::Seq) return n.seq.empty();
    return true;
}

void DumpNode(const Node& n, int indent, std::string& out);

void DumpEntry(const std::string& key, const Node& v, int indent, const char* prefix, std::string& out)
{
    out += std::string(static_cast<size_t>(indent), ' ') + prefix + key + ":";
    if (v.kind == Node::Scalar) {
        out += " " + ScalarText(v.scalar) + "\n";
    } else if (!EmptyNode(v)) {
        out += "\n";
        DumpNode(v, indent + 2, out);
    } else {
        // 空容器显式写出，避免读回时变成 Null。
        out += (v.kind == Node::Seq) ? " []\n" : " {}\n";
    }
}

void DumpNode(const Node& n, int indent, std::string& out)
{
    if (n.kind == Node::Map) {
        for (const auto& kv : n.map) DumpEntry(kv.first, kv.second, indent, "", out);
        return;
    }

    if (n.kind == Node::Seq) {
        const std::string pad(static_cast<size_t>(indent), ' ');
        for (const auto& item : n.seq) {
            if (item.kind == Node::Map && !item.map.empty()) {
                // 首项并入 "- "，后续键与项内容对齐（"- " 占 2 列）。
                DumpEntry(item.map[0].first, item.map[0].second, indent, "- ", out);
                for (size_t i = 1; i < item.map.size(); ++i)
                    DumpEntry(item.map[i].first, item.map[i].second, indent + 2, "", out);
            } else if (item.kind == Node::Seq && !item.seq.empty()) {
                out += pad + "-\n";
                DumpNode(item, indent + 2, out);
            } else if (item.kind == Node::Map) {
                out += pad + "- {}\n";
            } else {
                out += pad + "- " + ScalarText(item.scalar) + "\n";
            }
        }
        return;
    }

    out += std::string(static_cast<size_t>(indent), ' ') + ScalarText(n.scalar) + "\n";
}

} // namespace

std::string Dump(const Node& node, int indent)
{
    std::string out;
    DumpNode(node, indent, out);
    return out;
}

const Node* Node::find(const std::string& key) const
{
    if (kind != Map) return nullptr;
    for (const auto& kv : map)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string Node::str(const std::string& def) const
{
    return (kind == Scalar) ? scalar : def;
}

int Node::toInt(int def) const
{
    if (kind != Scalar || scalar.empty()) return def;
    char* end = nullptr;
    long  v   = strtol(scalar.c_str(), &end, 10);
    if (!end || *end != '\0') return def;
    return static_cast<int>(v);
}

bool Node::toBool(bool def) const
{
    if (kind != Scalar) return def;
    std::string s;
    s.reserve(scalar.size());
    for (char c : scalar) s.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
    if (s == "false" || s == "no" || s == "off" || s == "0") return false;
    return def;
}

bool Parse(const std::string& text, Node& out, std::string& err)
{
    std::vector<Line> lines;
    int               no = 0;
    size_t            p  = 0;

    for (;;) {
        const size_t e   = text.find('\n', p);
        std::string  raw = (e == std::string::npos) ? text.substr(p) : text.substr(p, e - p);
        const bool   end = (e == std::string::npos);
        p = end ? text.size() + 1 : e + 1;
        ++no;

        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string s = StripComment(raw);

        int    indent = 0;
        size_t k      = 0;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) { ++indent; ++k; }
        s = Trim(s.substr(k));
        if (!s.empty()) {
            Line ln;
            ln.indent = indent;
            ln.no     = no;
            ln.text   = s;
            lines.push_back(std::move(ln));
        }
        if (end) break;
    }

    err.clear();
    Parser ps(lines);
    out = Node{};
    return ps.Run(out, err);
}

} // namespace yamlmini
