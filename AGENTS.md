<!-- Shheeel By Zgod -->

The shared rules are canonical in
`~/.claude/projects/-root-droid-kernel/memory/MEMORY.md` (`~/.memory/` and the
qoder copy are symlinks to it). The workspace has no shared `docs/` tree; the
suite's state is in `droid-lkm-suite/docs/status.md` and this repo's state is in
`README.md`. You must read them before any work.

<!-- Tag by Zgod, Project manual memory -->

# AGENTS.md — droid-hook

Independent project: reusable inline-hook framework on the suite's core RW ABI 5.
Own git, own CI. Nothing here is owned by droid-lkm-suite.

## 派发 Action 前的强制本地检查

**规则：没有逐条做完并贴出输出，不许派发。** 未写进提示词的检查等于没做。

1. `make check` 全绿（host suites），`make kcheck` 全绿（库层 TU 在 `-D__KERNEL__` 下），
   `make runtime-check DNSK_CORE_INC=<suite>/core/include` 全绿。贴尾部输出。
2. **改动了 `Kbuild` / `Makefile` / workflow 的 `run:` 块**：
   `grep -n 'make \|Kbuild\|kmake' .github/workflows/*.yml`，把 CI 实际调用的
   **目录 + 目标名**逐个对上；新增任何构建文件必须提供 CI 调用的那个 target。
3. **新增源文件或编译行**：确认它落在**每一个**引用它的 job 里
   （`grep -c <名字> .github/workflows/<wf>` 期望等于引用处数量），不能只加一处。
4. **新增 workflow 输入**：`on.workflow_dispatch.inputs` 已声明，且
   `inputs.<name>` 的透传链完整（workflow → 环境变量 → 脚本 → 断言）。
5. **引用脚本**：路径存在、由 `sh`/`bash` 调用（不依赖可执行位）。
6. **派发前 headSha 断言**：`gh run view <run> --json headSha` 必须包含被测 commit；
   本地未 push 不许派（`workflow_dispatch` 跑远端 ref）。
7. **只在功能完成点派发**：一次变更集一次验证；不许一小步一 Action。
8. 失败：先本地定位，修好后最多重试 1 次；不许连发、不许改断言凑绿。

未覆盖的类别出现一次，就把检查加进本地门禁脚本与本清单。

## 功能测试的标准（load/unload 不算测试）

`insmod` 成功 + `rmmod` 干净 + 自报 marker **只证明模块能进出内存**，不算功能验证。
必须验真实行为：

- 真的装上 hook，调用目标函数，确认**拦截生效**（返回值被改 / 计数增加 / 原逻辑被替换）
- 卸载后再调用目标，确认**恢复原行为**
- RO 写入路由要真写并读回校验

marker 命名建议 `DH_*_INTERCEPT_OK` / `DH_*_RESTORED`，缺一即未完成。

## 状态维护（每步做，不攒到最后）

| 变化 | 更新 |
|---|---|
| 行为 / 默认值 / 参数 | `README.md` 对应段落 |
| 新增或删除文件、API | `README.md` §分层 / 已实现；`include/dh.h` 注释 |
| 门控结果 | `docs/evidence/<gate>-<date>.md`（本地资料，gitignored；run id、headSha、marker 原文） |

`README.md` 必须始终同时有「已验证」和「未验证」两节；未验证项不许写成完成。

## Hard constraints

- 不本地安装工具链；构建在 CI 或 Codespaces
- 不伪造 run 结果；CI 里不许 `|| true` 之类静默通过
- 不 `--force`、不手动填假 CRC；失败如实记录
- 不 reboot 设备；panic 只取当前日志
- Action 限额：3 小时内 ≤9 次、单次 ≤30min；派发前记录时间与已用次数

## 与 suite 的关系

`runtime/` 依赖 suite 的 `core/include/dnsk_core.h`（ABI 5），构建时用
`DNSK_CORE_INC=/root/droid-kernel/droid-lkm-suite/core/include`。suite 的 core ABI 变化时这里必须跟进，
但不要把 suite 的代码复制进来。

## 读源码要落地

读到内核/上游源码并据此做判断时，先把结论写进 `docs/notes/`：
**语义 + `文件:行号` + 为什么这样决定**。只留在对话里等于没做——下一个人得重读同样的代码。

## Notes (index)

- [On inline call-through gap](./docs/notes/On-inline-call-through-gap.md) —
  入口跳转直达 replacement, relo 区不可执行且无公开指针; suite feature 迁移
  需要 origin 调用的结论与决定
