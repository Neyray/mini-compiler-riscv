# 前端 (Frontend) —— 负责人 B

词法分析 + 语法分析（AST 构建）。语言规格（ToyC 文法/终结符/语义约束）见
[`docs/toyc-spec.md`](../../docs/toyc-spec.md)；AST → 后端的接口契约见
[`docs/backend-design.md`](../../docs/backend-design.md) §1。

- **词法分析**：将源字符流切分为 token 序列。
- **语法分析**：按文法规约构建抽象语法树 (AST)，交给中端。
- 头文件接口放在 `include/frontend/`。

> 占位目录，待 B 实现。接口（token 定义、AST 结构）确定后同步到 `docs/`。
