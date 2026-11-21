# Droid-Hook

状态: **库层 + 运行时适配 + preflight/probe 已实现; host 侧 90 项单测通过; `droid_hook.ko`
已在 CI 的 DDK 镜像中真实构建成功; QEMU 门控已跑通"启动→insmod→自检"路径并抓到一处
selftest 真实缺陷 (已修, 复验待下一轮); runtime 已接入 `DNSK_CORE_INC` (CI 链接复验待下一轮)**.

Droid-Hook 为独立 public 仓库 (https://github.com/uobe1/droid-hook, 默认分支 `main`),
不进 droid-lkm-suite 的 `features/*/Kbuild`, 不改变其产物清单, 也不影响任何现有 LKM 的行为.

## 当前状态

| 项 | 状态 |
|---|---|
| 仓库 | https://github.com/uobe1/droid-hook (public, 默认分支 `main`) |
| host 门禁 `make check` | 90 项 / 6 suites / 7 TUs 全绿 |
| 内核语法代理 `make kcheck` | 6 个库层 TU 全 ok |
| 内核模块构建 | **已验证**: CI DDK run `44875690763` 产出 `droid_hook.ko` (290216 B, `modinfo -F name`=`droid_hook`, vermagic `6.12.76-4k SMP preempt mod_unload modversions aarch64`), artifact `droid-hook-android16-6.12`; 同 commit 还有一次未记录的成功 run `44876857725` |
| QEMU 门控 | 基础设施**已可用**: run `44909471977` 真实启动 DDK `Image`、`insmod` 模块、执行 `selftest=1`、上传串口日志; 但 selftest 在 NOP 重定位期望上失败 (已修, 整体通过待下一轮复验) |
| 真机加载/卸载 | 未做 |
| runtime 层内核构建 | 已接入 `DNSK_CORE_INC` (`runtime/dh_core_backend.o` 按 core 头存在与否编入模块); 本机 `make runtime-check` 对真实 core 头通过; 首次 CI 构建暴露栈帧 17152 > 2048, 已把暂存移入 `.bss`, 复验待下一轮 |

### 已验证 / 未验证

**已验证 (有 run id / 日志为证)**

- `make check` 90 项全绿、`make kcheck` 6 TU 全 ok (本地)
- CI DDK 真实构建 `droid_hook.ko`: run `44875690763`, head `f4e952f`, host job 11s +
  build job 1m11s (总墙钟 06:28:18Z→06:29:47Z), `modinfo -F name`=`droid_hook`,
  vermagic `6.12.76-4k SMP preempt mod_unload modversions aarch64`, artifact 134772 B.
  证据: [build-gate-2026-11-21](./docs/evidence/build-gate-2026-11-21.md)
- 首次 run `44875487955` 真实失败 (clang `-Wframe-larger-than`: `init_module` 栈帧 2416 > 2048),
  修复后重派成功. 这说明 CI 真的编译了 `dh_module.c`, 不是走过场.
- 未记录的 run `44876857725` (head `fcaff43`, 06:42:58Z→06:44:15Z, host 8s + build 1m01s,
  success) 已补记进 [build-gate-2026-11-21](./docs/evidence/build-gate-2026-11-21.md)
- `make runtime-check DNSK_CORE_INC=/root/droid-lkm-suite/core/include` 通过: `runtime/dh_core_backend.c`
  在 `-D__KERNEL__` 下用真实 core 头 `dnsk_core.h` 编译干净. 该检查立刻抓到一处真实缺口:
  适配层引用的 `DNSK_RW_MAX` 只存在于 host 仿造头, 公开 core 头只在注释里写"at most 256"
  (`core/include/dnsk_core.h:291`), 实际常量在实现里 (`core/dnsk_rw.c:46`, 校验于 `:69`).
  现在适配层自持该值并用 `#ifndef` 守卫, 不再依赖未公开符号
- `sh -n tests/qemu/init-droid-hook` 与两份 workflow 的 YAML 解析通过 (非门控证据, 仅语法)
- QEMU 门控基础设施已被真实 run 证明可用: run `44909471977` (head `f77a90e`) 在
  `-M virt` 下启动 DDK `Image`, 成功 `insmod` 模块并执行 `selftest=1`, 串口捕获
  `DH_SELFTEST_FAIL:relocate_ret` 后 `DH_QEMU_FAIL insmod`, 串口日志以 artifact 上传.
  它抓到一处 host 90 项测试覆盖不到的真实缺陷: `dh_module.c` 自检把 NOP 重定位期望成
  `DH_OK`, 而库契约是 `DH_E_OPNOTSUPP` (host `tests/host/test_dh_insn.py` 亦断言 -95)

**仅配置就绪 / 未验证**

- QEMU 门控整体仍未通过 (`DH_SELFTEST_OK` 尚未出现): selftest 的 NOP/B 重定位断言已按真实
  契约修正 (B 正向重定位 `0x17FFFD00` + NOP 拒绝), 复验待下一轮
- `runtime/dh_core_backend.o` 尚未在 CI 中成功链接: 首次尝试 (`44909065331`) 暴露
  `dh_core_rw_slot` 栈帧 17152 > 2048 (`-Wframe-larger-than`); 已把分割路径暂存移入模块
  `.bss` 并加同槽 `DH_E_BUSY` 串行保护, 本机 `make check`/`make runtime-check` 通过,
  CI 复验待下一轮 (该变更集重试额度已用)
- 真机 `insmod`/`rmmod`、崩溃/告警扫描未做

## 这是什么

Droid-Hook 是一个可嵌入 LKM(或其它内核模块)的 Patch / Hook / RW 库, 加一层可替换的
运行时. 路线由用户 2026-11-14 授权并写入设计证据:
以本仓库 core 的 RW ABI 5 单入口 `rw` / `patch_pointer` 为能力核, 综合 KernelPatch 的
hotpatch / inline hook / kstorage 与上游 livepatch / static_call / text_poke 等设计,
自行实现库 + 运行时, **不引入 KernelPatch 作为第二套 root 栈**.

## 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 库层 `libdroidhook` | `include/dh.h`, `src/` | 只依赖注入的 `struct dh_backend`, 不持有全局状态, 不 include core 头 |
| 运行时 `dh_runtime` | `runtime/` | 把 core RW ABI 5 适配成 backend; 换载体 = 换 backend 实例, 库层零改动 |

## 已实现(host 侧验证)

| 文件 | 职责 | 关键点 |
|---|---|---|
| `src/dh_rw.c` | ctx 生命周期与 RW 包装 | 256B/页切分; 每次 WRITE 读回校验; 8 字节对齐检查; backend 缺失或 abi 不符返回 `DH_E_INVAL` |
| `src/dh_txn.c` | 事务与回滚 | undo 静态 8 槽零分配; 存原字节; 逆序 replay; 满槽 `DH_E_NOSPC`; 读回不一致 `DH_E_VERIFY` 并让 commit 自动回滚; 回滚在单个 quiesce 窗口内 |
| `src/dh_insn.c` | arm64 编码与引用重定位 | 绝对跳转 `LDR X17,#8; BR X17;<literal>`; ADRP/ADR/B/BL/B.cond/CBZ/TBZ/LDR literal 重编码; 成对 ADRP+ADD/LDR 重定位(fail-closed); ±4GiB 与 ±1MiB 范围检查 |
| `src/dh_hook.c` | 函数级 trampoline | 覆盖前 4 条指令 + relo 区 + 跳回第 5 条; key 与 target 双重唯一; 地址仲裁; before 正序 / after 逆序 / skip_origin; 卸载逆序且在 quiesce 窗口内 |
| `src/dh_shadow.c` | `<obj,id>` 侧表 | 不改父对象 layout; get-or-alloc 幂等; 延迟回收至最后一个引用 |
| `src/dh_preflight.c` | 预检与保护探测 | `dh_preflight` 校验 ctx/backend/abi/未决事务; `dh_probe_protection` 以同字节写 + 读回校验探测 RO 路由, 读/写/校验失败原样返回真实 errno |
| `runtime/dh_core_backend.c` | ABI 5 适配 | `rw`/`patch_pointer`/`resolve` 映射; magic/abi 校验; `F_TEXT`/`F_RO` 走会睡眠的 patch 后端; 数据路径 `flags=0` 不进 stop_machine |

### 指令编码的两个实证结论

- `ADD`(immediate) 的 imm12 在 bits[21:10], `sh` 在 bit22, 正确掩码是 `0x003FFC00`.
  `0x00FFFC00` 覆盖 bits[10:23], 会把 `sh` 与 bit23 当数据位; 旧原型靠"整片取出再原样放回"
  的巧合才没出错. 本库按 ARM ARM 拆分解码/编码, 并由 `test_add_imm12_and_sh_preserved`
  与 `test_pair_add_rewrites_imm12_and_keeps_sh` 锁死.
- `LDR`/`STR` unsigned-offset 的 `size`/`opc` 位必须保留, 且 imm12 按访问宽度缩放;
  对应 `test_ldr_opc_bits_not_cleared` 与 `test_pair_ldr_keeps_size_opc_and_rescales_imm12`.

## 验证

- `make check` 全绿: 库层 TU 以生产参数(`-std=c11 -Wall -Wextra -Werror`)零 warning 编译,
  6 套 host 单测共 **90 项**: contract 6 / rw+txn 12 / insn 19 / hook 19 / preflight 10 / runtime 24
- `make kcheck` 全绿: 6 个库层 TU 在 `-D__KERNEL__` 下用 AOSP 头
  (`/usr/local/share/aosp-6.12.30`)逐个语法编译通过, 证明库层不偷用 libc 头
- 关键断言做过变异验证(故意改坏实现 → 测试必须失败 → 还原后 `cmp` 字节一致):
  hook 侧 4 个、insn 侧 2 个, 以及本轮 preflight 的 5 个变异(去掉 busy 守卫、跳过读回校验、
  `ro_write` 失败却假成功、吞掉读错误、忽略 `initialized`)全部被捕获, 证明断言不是空转
- 测试 seam `-DDH_INS_TEST_SEAM` 只在测试构建定义; 生产目标文件已用 `nm` 确认不含该符号

### preflight 与保护探测的设计取舍

`dh_probe_protection` 用"以同字节写一次再读回"来验证 RO 写路由: 写回的正是刚从同一地址读到
的字节, 成功时目标内容与指令流完全不变, 唯一可观测结果是该路由确实接受了写入; 若路由写坏,
读回不一致会返回 `DH_E_VERIFY` 而非假成功; 若路由拒绝, 返回其真实 errno. 因此它既安全
(幂等、不改状态)又诚实(不伪造 `DH_OK`), 读失败/写失败/校验失败都原样向上传播.

当前只探测 `DH_ROUTE_FIXMAP`, 因为 ABI 5 适配层目前只真正实现 `F_RO -> patch_map` 这一条
(ALIAS/PTE/DMA 仍是契约占位); 待适配层区分更多路由后, 这里应改为按优先级依次探测.

### CI 的 DDK 构建验证 (已完成)

本机无内核构建树, 以下由 `.github/workflows/build.yml` 的 DDK job 验证 (run `44875690763` 通过):
- `dh_module.c` 在真实内核头下编译(依赖 `<linux/module.h>` 与生成的 `<asm/*>` 头)
- `Kbuild` 组合出 `droid_hook.ko` (`LD [M] droid_hook.ko` + BTF), 且 `obj-m`/`droid_hook-y`
  列表、`-I$(src)/include` 正确
- `modinfo -F name` 为 `droid_hook` 且带合法 vermagic (`build.yml` 内为硬断言, 无 `|| true`)

本机只能用 `make kcheck` 代理证明库层 TU 内核干净; 它不覆盖 `dh_module.c` 与链接/打包.

## 明确未做(不要误读为已完成)

- `dh_module.c` 与 `droid_hook.ko` 已由 CI DDK 构建验证 (run `44875690763`); QEMU 已能真实
  `insmod` 并执行自检, 但 **门控整体尚未通过** (`DH_SELFTEST_OK` 未出现, 见 run `44909471977`),
  且 **没有真机加载/卸载**
- `runtime/` 虽已接入 `DNSK_CORE_INC` 并本机通过内核语法检查, 但 **CI 中的真实链接尚未验证**
  (栈帧修复待下一轮复验)
- 4 条 RO 写入路由(ALIAS / PTE / FIXMAP / DMA)目前只有 `FIXMAP` 经 core 的
  `F_RO` 后端真实存在; 其余三条是契约占位, 适配层尚未区分
- `dh_insn` 的成对重编码是内部助手 + `dh_scan_refs` 的 fail-closed 预检,
  **没有公开入口把重编码后的指令交给调用方**(需要改 `dh.h`, 属 ABI 变更)
- `dh_hook_install` 固定按 4 条原始指令预留 relo 槽, 依赖
  "relocate 不会把一条指令展开成多条"; 若将来支持 MOVZ/MOVK 序列, 需要
  `dh.h` 增加 out-len 参数
- 运行时适配用固定 4 槽 trampoline 解决"backend 回调无 userdata"的多实例隔离,
  超过 4 个 backend 实例会返回 `DH_E_NOSPC`
- 设计证据里的 6 个待决定点(KPM 迁移节奏, 运行时载体, 许可证边界, EL2 面策略,
  force 卸载语义, 性能基线口径)仍待用户决定

## 已知的同类隐患(不在本目录, 未改)

`features/cgpids/native.c` 的 ADD 分支同样使用 `0x00FFFC00`. 它靠"先清该字段再写低 12 位"
得到正确结果, 行为正确, 但把 `sh` 与 opcode 位当数据位处理, 属同类 latent hazard.
那是已门控的生产代码, 本轮不动.

## 设计证据与计划

- `docs/evidence/droid-hook-design-research.md`(本地资料, 945 行, 89 条一手 URL)
- `docs/plans/optional-future-2026-11-14.md` §4
- `docs/evidence/rw-memory-mutation-research.md`(RW 扩容/改值配方与 prior art)

## 本地验证

```sh
make check          # 编译每个库层 TU(host 参数) + 跑全部 host 单测
make kcheck         # 用 AOSP 头在 -D__KERNEL__ 下逐 TU 语法检查(无内核树时最强代理)
make runtime-check DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include
                    # runtime/ 适配层对真实 core 头做 -D__KERNEL__ 语法检查
make module KDIR=/path/to/kernel/tree
                    # 只打包库 + dh_module.c (不需要 core 头)
make module KDIR=/path/to/kernel/tree \
     DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include
                    # 额外把 runtime/dh_core_backend.o 编入 droid_hook.ko
make clean
```

### 两种内核构建方式

- 不带 `DNSK_CORE_INC`: 编入 `src/*.c` + `dh_module.c`, 不含 runtime 适配层; Kbuild 打印
  `droid-hook: DNSK_CORE_INC unset -> runtime/dh_core_backend.o skipped`.
- 带 `DNSK_CORE_INC=<core>/include`: Kbuild 检测到 `<core>/include/dnsk_core.h` 后追加
  `runtime/dh_core_backend.o` 并加 `-I<core>/include`; 路径非空但不含该头则 `$(error)` 硬失败,
  不会静默降级. CI (`build.yml`) 用 pinned commit 的 `dnsk_core.h` (sha256 校验) 建第二个
  `.ko`, 并用 `nm` 断言 `dh_core_backend_init` 只在带 runtime 的那个里出现.

`make check` / `make kcheck` / `make runtime-check` 是本机能做到的验证. `dh_module.c` 依赖
`<asm/cpucap-defs.h>` 等生成头, 只能由真实内核构建(`build.yml` 的 DDK 镜像)编译验证.
`Kbuild` 默认只把库层 `src/*.c` 与 `dh_module.c` 打进 `droid_hook.ko`; `runtime/` 仅在
`DNSK_CORE_INC` 给出 core 头时一并编入.
