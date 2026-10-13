// yamlmini.h - 最小 YAML 子集解析器
//
// 只覆盖本项目配置所需的语法：
//   嵌套映射（`key: value` / `key:` + 缩进子块）、
//   序列（`- item` / `- key: value` + 同列后续键）、
//   空容器（`[]` / `{}`，与 Dump 的输出对应）、
//   行首或空白之后的 `#` 注释、单/双引号标量。
// 不支持：锚点、别名、多行标量、非空的流式（`[a, b]` / `{a: b}`）风格、复杂键。
//
// ponytail: 缩进宽度按「前导空白字符个数」计算，tab 记为 1 列；混合 tab/空格的
// 配置可能解析失败。当前配置全用空格，若将来需要 tab 支持，在 Parse()
// 里统一展开即可。
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace yamlmini {

    /** 解析后的节点。同一节点按 kind 只使用对应的一个成员。 */
    struct Node {
            enum Kind { Null, Scalar, Map, Seq };

            Kind kind = Null;
            std::string scalar;                            ///< kind == Scalar
            std::vector<std::pair<std::string, Node>> map; ///< kind == Map
            std::vector<Node> seq;                         ///< kind == Seq

            /** 在 Map 中按键查找子节点。 @return 命中返回节点，否则 nullptr。
             */
            const Node * find(const std::string & key) const;

            /** @return Scalar 文本；非 Scalar 时返回 def。 */
            std::string str(const std::string & def = std::string()) const;

            /** @return 十进制整数；非数字时返回 def。 */
            int toInt(int def = 0) const;

            /** @return true/yes/on/1 为 true，false/no/off/0 为 false，其他为
             * def。 */
            bool toBool(bool def = false) const;
    };

    /**
     * 解析 YAML 子集文本。
     *
     * @param text UTF-8 文本。
     * @param out  根节点（成功时填充）。
     * @param err  失败原因，含 1-based 行号。
     * @return     成功为 true。
     */
    bool Parse(const std::string & text, Node & out, std::string & err);

    /**
     * 序列化节点为 YAML 文本，每层缩进 2 空格。
     *
     * 与 Parse() 构成往返：Dump(Parse(x)) 可再次被 Parse() 读回。
     * 需要引号保护的标量（空串、含特殊字符、与布尔/数字字面量同形）会用双引号包裹。
     *
     * @param node   待序列化节点；Scalar 作为根时输出裸标量行。
     * @param indent 根节点的起始缩进宽度。
     * @return YAML 文本，含结尾换行。
     */
    std::string Dump(const Node & node, int indent = 0);

} // namespace yamlmini
