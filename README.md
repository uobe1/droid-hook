# Droid-Hook

可嵌入 LKM 的 inline-hook / RW 库, 加一层可替换的运行时. 独立 public 仓库
(https://github.com/uobe1/droid-hook, 默认分支 `main`), 不进 droid-lkm-suite
的 `features/*/Kbuild`, 不改变其产物清单.

路线: 以 suite core 的 RW ABI 5 单入口 `rw` / `patch_pointer` 为能力核,
综合 KernelPatch 的 hotpatch / inline hook / kstorage 与上游 livepatch /
static_call / text_poke 的设计, 自行实现库与运行时, 不引入 KernelPatch
作为第二套 root 栈.

## 状态

| 项 | 状态 |
|---|---|
| host 门禁 `make check` | 6 套单测全绿, 生产参数零 warning |
| 内核语法代理 `make kcheck` | 库层 TU 在 `-D__KERNEL__` 下全部通过 |
| DDK 模块构建 | `droid_hook.ko` 构建成功, `modinfo` 名字与 vermagic 正确 |
| QEMU 门控 | 加载/卸载/自检通过, 无 `BUG`/`Oops`/`panic`/`[permanent]` |
| 真实拦截/恢复 | `jiffies_to_msecs` 被替换后恢复原返回值 |
| RO 路由 | ALIAS / PTE 在只读页写入并读回一致 |
| 真 core ABI 5 路由 | `dnsk_core_get` 经 kprobe 解析, `rw` 读/写读回成功 |

详细门控记录在 `docs/evidence/` (本地资料).

### 未验证 / 已知边界

- DMA 路由 fail-closed `DH_E_OPNOTSUPP`: 普通 LKM 无 EL2/Stage-2, 也没有保证的
  dmaengine 通道
- core 适配层不提供 `icache_flush`/`tlb_flush`/`quiesce_*`: 裸 backend 被
  `dh_ctx_init` 以 `DH_E_INVAL` 拒绝; 用 no-op 包装能通过初始化并执行安装/卸载,
  但没有真实 quiesce 窗口, 经它安装的 hook 不具备安全卸载保证
- 真实 quiesce 窗口未验证; runtime 自检跑在 mock core 上
- 未做压力/并发/异常注入; pstore 路径未验证
- 真 core 路由的写验证只写 droid_hook 自己的 `u64`, 未写内核全局数据
- 设计证据里的 6 个待决定点仍待用户决定: KPM 迁移节奏, 运行时载体,
  许可证边界, EL2 面策略, force 卸载语义, 性能基线口径

## 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 库层 `libdroidhook` | `include/dh.h`, `src/` | 只依赖注入的 `struct dh_backend`, 不持有全局状态, 不 include core 头 |
| 运行时 `dh_runtime` | `runtime/` | 把 core RW ABI 5 适配成 backend; 换载体 = 换 backend 实例, 库层零改动 |

## 已实现

| 文件 | 职责 | 关键点 |
|---|---|---|
| `src/dh_rw.c` | ctx 生命周期与 RW 包装 | 256B/页切分; 每次 WRITE 读回校验; 事务内 backend 错误或校验读失败置 `txn.failed`; 8 字节对齐检查; backend 缺失或 abi 不符返回 `DH_E_INVAL` |
| `src/dh_txn.c` | 事务与回滚 | undo 静态 8 槽零分配; 存原字节; 逆序 replay; 满槽 `DH_E_NOSPC`; 读回不一致 `DH_E_VERIFY` 并让 commit 自动回滚; 回滚在单个 quiesce 窗口内 |
| `src/dh_insn.c` | arm64 编码与引用重定位 | 绝对跳转 `LDR X17,#8; BR X17;<literal>`; ADRP/ADR/B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/LDR literal(含 SIMD) 重编码; 成对 ADRP+ADD/LDR 重定位 fail-closed; ±4GiB 与 ±1MiB 范围检查; 批量补丁开自身事务、逐站读回校验、失败只回滚自身写入 |
| `src/dh_hook.c` | 函数级 trampoline | 覆盖前 4 条指令 + relo 区 + 跳回第 5 条; key 与 target 双重唯一; 地址仲裁; 入口写入在单个 quiesce 窗口内并读回校验; 回滚失败时不释放 trampoline/chain 且保留 registry 占用; 卸载逆序且在 quiesce 窗口内且校验后才释放; backend 提供 `alloc_exec` 时把 relo 块放进可执行内存, `dh_hook_origin()` 返回该块 (DH_ABI 2) |
| `src/dh_shadow.c` | `<obj,id>` 侧表 | 不改父对象 layout; get-or-alloc 幂等; 延迟回收至最后一个引用 |
| `src/dh_preflight.c` | 预检与保护探测 | `dh_preflight` 校验 ctx/backend/abi/未决事务; `dh_probe_protection` 以同字节写 + 读回校验探测 RO 路由, 读/写/校验失败原样返回真实 errno |
| `runtime/dh_core_backend.c` | ABI 5 适配 | `rw`/`patch_pointer`/`resolve` 映射; magic/abi 校验; `F_TEXT`/`F_RO` 走会睡眠的 patch 后端; 数据路径 `flags=0` 不进 stop_machine; 16 槽 trampoline |
| `runtime/dh_ro_routes.c` | RO 写入路由 | ALIAS = 目标页 `vmap` 可写别名写入; PTE = kprobe 解析 `set_memory_rw/ro` 后改写 PTE 再恢复; FIXMAP 交 core; DMA fail-closed `DH_E_OPNOTSUPP`; 写完 `flush_icache_range` |

### 调用穿透 (call-through)

入口路径把目标前 4 条指令改写为绝对跳转, 直达 `replacement`; 它不经过
chain. `before` 正序 / `after` 逆序 / `skip_origin` 只存在于 `dh_hook_chain_run()`,
那是库内函数, 安装路径不会调用它. 该函数里每个 before 都按注册顺序执行, 首个
非零 before 结果作为跳过 origin 的返回值.

需要继续原逻辑的 replacement 必须与 target 原型完全一致, 并调用
`dh_hook_origin(hook)` 继续执行原函数. 该指针指向 relocated origin 块 (原前 4 条
指令的搬迁序列 + 跳回 target+16). 只有 backend 提供 `alloc_exec`/`free_exec` 时
该块存在于可执行内存并由 `dh_hook_origin()` 返回; backend 未提供时保持旧布局
(relo 块在 trampoline 内, 不可执行), `dh_hook_origin()` 返回 NULL. 安装时 exec
块分配失败以 `DH_E_NOSPC` 失败, 不回退到不可执行布局; 卸载在校验入口恢复后才
释放 exec 块, 校验失败的卸载保留它. 安装一旦已发出入口写入, 之后即使回滚成功也
不释放 exec 块 (可能有 CPU 正在其中执行), 只释放 chain/trampoline 并释放
registry 占用; 只有入口写入之前的失败才释放 exec 块.

DH_ABI 2 因 `struct dh_backend` 末尾追加 `alloc_exec`/`free_exec` 而递增.
host 单测验证 origin 指针指向 exec 块、块内容等于搬迁序列加跳回、无
`alloc_exec` 时 origin 为 NULL、分配失败不泄漏、卸载恰好释放一次、失败卸载不
释放; 未在 AArch64 上实际执行 origin, 也未经真机 call-through 验证.

### 指令编码契约

- `ADD`(immediate) 的 imm12 在 bits[21:10], `sh` 在 bit22, 正确掩码是
  `0x003FFC00`; `0x00FFFC00` 会把 `sh` 与 bit23 当数据位
- `LDR`/`STR` unsigned-offset 的 `size`/`opc` 位必须保留, imm12 按访问宽度缩放
- 上述两条由 host 测例锁定: `test_add_imm12_and_sh_preserved`,
  `test_pair_add_rewrites_imm12_and_keeps_sh`, `test_ldr_opc_bits_not_cleared`,
  `test_pair_ldr_keeps_size_opc_and_rescales_imm12`

### 保护探测契约

`dh_probe_protection` 用"以同字节写一次再读回"验证 RO 写路由: 写回的正是刚从
同一地址读到的字节, 成功时目标内容与指令流不变, 唯一可观测结果是该路由接受了
写入; 路由写坏时读回不一致返回 `DH_E_VERIFY`, 路由拒绝时返回其真实 errno.

当前只探测 `DH_ROUTE_FIXMAP`, 因为 ABI 5 适配层目前只真正实现 `F_RO ->
patch_map`; ALIAS/PTE/DMA 仍是契约占位.

## 构建与验证

```sh
make check          # 编译每个库层 TU(host 参数) + 跑全部 host 单测
make kcheck         # 用 AOSP 头在 -D__KERNEL__ 下逐 TU 语法检查
make runtime-check DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include
                    # runtime/ 适配层对真实 core 头做 -D__KERNEL__ 语法检查
make module KDIR=/path/to/kernel/tree
                    # 打包库 + dh_module.c (不需要 core 头)
make module KDIR=/path/to/kernel/tree \
     DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include
                    # 额外把 runtime/dh_core_backend.o 编入 droid_hook.ko
make clean
```

- 不带 `DNSK_CORE_INC`: 编入 `src/*.c` + `dh_module.c`, 不含 runtime 适配层
- 带 `DNSK_CORE_INC=<core>/include`: Kbuild 检测到 `dnsk_core.h` 后追加
  `runtime/dh_core_backend.o` 并加 `-I<core>/include`; 路径非空但不含该头则
  `$(error)` 硬失败
- `dh_module.c` 依赖 `<asm/cpucap-defs.h>` 等生成头, 只能由真实内核构建验证;
  本机用 `make kcheck` 代理库层 TU 的内核干净度

## 证据

门控 run id, 日志与逐项结论在 `docs/evidence/` (本地资料, 不随仓库分发).
