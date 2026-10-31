# AI 函数增强：实现与本地验证

记录日期：2026-11-01。此记录不是官方评分，也没有触发大赛提测。

## 版本与范围

- 官方比赛测试分支：`5bce9e06d1281347ad61344e4b01fab9158c58a2`。
- 保留团队提交、合并官方测试后的基线：`ed5968fe812281686ce3498a6b2226bb57cd5d8a`。
- 实际编译并运行的修复代码：`769f361bdeadfc734e033182be9b316877206ec0`。
- 任务分支：`feat/competition-ai-functions`；团队 PR 目标：`2026-competition`。

实现覆盖 AI_PROMPT 的 JSON/LOB 参数及递归模板渲染，AI_COMPLETE 的 NULL/JSON 配置与非空响应校验，Ollama 原生非流式协议及可选密钥，embedding 响应验证和跨行批量请求，以及 rerank 分批后的全局索引和稳定排序。批量 embedding 按模型和维度分组，每个 HTTP 请求最多 128 条输入，保留过滤、NULL 和原始行的对应关系。

未改动官方测试或测试数据。产品修改仅位于 `src/`；本文件是新增说明。没有针对模型名、查询内容、测试文档或响应答案硬编码。

## 实际验证

在专用 Linux Docker 容器中，使用相同依赖、Rust 1.98.1、LLVM 17 和 release 构建配置分别完整编译基线与修复版。每次仅运行一个数据库实例，使用独立数据目录、2 GiB 内存预算、2 个 CPU 的数据库配置和 2 GiB 日志盘。

编译命令为 `bash build.sh release --init`（首次准备依赖）及 `make -C build_release -j4 seekdb`。SQL 通过编译所得真实 seekdb 执行，HTTP 请求由本地协议 Mock 服务响应；没有使用真实模型凭据。

| 未修改的官方 mysqltest | 基线 | 修复版 |
| --- | --- | --- |
| `ai_prompt.test` | 失败：不支持 JSON 参数 | 通过 |
| `ai_function_enhancement.test` | 失败：不支持 JSON 参数 | 通过 |
| `ai_model_ddl.test` | 通过 | 通过 |
| `ai_model_endpoint_ddl.test` | 失败：未知 `max_concurrency` 参数 | 同一位置失败 |

相同的 12 组补充运行回归，基线通过 1/12，修复版通过 12/12。覆盖：

1. JSON Array/Object 保留结构。
2. LONGTEXT 模板、长 Unicode 参数及转义。
3. Completion 的 NULL 配置及 temperature/max_tokens 请求内容。
4. 长文本重复占位符、嵌套 Prompt、非法及溢出下标。
5. 空、NULL、对象类型 Completion 和错误顶层响应。
6. 默认/显式 embedding 维度、数值内容、NULL、非法维度及响应元素。
7. 300 行输入中的过滤、LIMIT、NULL、多请求批次及乱序响应；逐行校验向量内容，并检查 HTTP 请求确实包含多条输入。
8. 重复 embedding index 导致查询失败，后续正常查询恢复且映射正确。
9. 同一查询中动态变化的 embedding 维度。
10. HTTP 404 错误传播，后续正常请求可用。
11. 47 条 rerank 文档跨三个批次的全局排序、同分顺序、对象 ID 保留及无效响应拒绝。
12. Ollama 空密钥、Provider 大小写及 ALTER、强制非流式、原生 embedding 和非法向量拒绝。

构建产物 SHA-256：

- 基线：`bbc7097c0889a29e53615554b4b2362618de487a5f00d215eb8d05eac7b9ba74`
- 修复版：`9f56d477ea533d2e4c09babd814dd9dd92d07d0cfffbef301bd99b424fcd2722`

## 已知限制与后续工作

`ai_model_endpoint_ddl.test` 在第 38 行创建带 `max_concurrency` 的 Endpoint 时失败（错误 11115）。该字段与限流机制尚未实现，后续语句没有在本轮通过完整验证；不能将此测试报告为通过。

本文只证明上述本地编译和测试结果，不推断隐藏用例结果或官方 60 分。文件 SQL、异步 AI 列与全文索引性能优化不属于本次代码改动。官方文档列出的两份 AI 函数单元测试源码在本次上游提交中不存在，未声称执行这些测试。

补充回归、Mock 服务、构建命令封装及原始结果保存在任务工作区的独立验证目录，作为交付附件提供；未将测试夹具放入产品 `src/`。
