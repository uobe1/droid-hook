# Droid-Hook 首个内核模块构建门 (build gate) — 2026-11-21

本文件记录 droid-hook 独立 public 仓库建立后, **首次由 CI 的 DDK 镜像真实构建
`droid_hook.ko`** 的证据。只以 run id / 日志 / artifact 为证; 未加载执行, 未 QEMU/真机门控。

## 仓库与变更

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/uobe1/droid-hook (public, 默认分支 `main`) |
| 首推 commit | `3928ffe` Add kernel module packaging, real preflight/probe, and the module CI |
| 修复 commit | `f4e952f` dh_module: move selftest ctx to .bss to fit 2KB kernel stack |
| 构建头 commit (成功) | `f4e952f943101dc8080b3b4c284c3733e26dd79c` |

## 构建环境

| 项 | 值 |
|---|---|
| workflow | `.github/workflows/build.yml` (`workflow_dispatch` / `workflow_call`) |
| runner | `ubuntu-latest`, build job 跑在容器 `ghcr.io/ylarod/ddk-min:android16-6.12-20260828` (与 droid-lkm-suite 的 ci-fast 一致) |
| KDIR | `/opt/ddk/kdir/android16-6.12` |
| 命令 | `make -C "$KDIR" M="$GITHUB_WORKSPACE" modules` |

## Run 汇总

| run id | headSha | conclusion | created → updated (UTC) | host job | build job |
|---|---|---|---|---|---|
| `44875487955` | `3928ffe` | **failure** | 06:25:39 → (fail) | 6s 通过 | 1m10s 失败 |
| `44875690763` | `f4e952f` | **success** | 06:28:18 → 06:29:47 | 11s (06:28:22→06:28:33) | 1m11s (06:28:35→06:29:46) |
| `44876857725` | `fcaff43` | **success** | 06:42:58 → 06:44:15 | 8s (06:43:03→06:43:11) | 1m01s (06:43:13→06:44:14) |

- `44876857725` 是当时漏记的一次重复门控 (同一 commit `fcaff43`, 无代码差异), 2026-11-21
  补记; 它不改变结论, 但说明该 workflow 可重复通过.

## runtime 接入后的模块门控 (2026-11-21 下午)

变更集: `447cded` Add QEMU load gate and wire runtime/ into the module build

| run id | headSha | conclusion | 失败/通过点 |
|---|---|---|---|
| `44908866295` | `447cded` | **failure** | host 8s 通过; `Build droid_hook.ko` 通过; 新增 `Build with runtime adapter` 以 curl 404 失败 |
| `44909065331` | `d1a0f19` | **failure** | host 通过; 默认模块构建通过; runtime TU 编译失败: `dh_core_rw_slot` 栈帧 17152 > 2048 (`-Wframe-larger-than`) |

- 失败 1 原因: 工作流 pin 的 commit `2828828` 只在本地分支, 未推到 public 远端; raw URL 404.
  public `main` 的 `dnsk_core.h` 仍是 ABI 2, 不能拿来冒充 ABI 5. 修复: 在 public 远端扫描,
  找到 `f6d325be` (`origin/feat/control-plane-iteration`) 的 `core/include/dnsk_core.h` 与本地
  ABI 5 头字节一致 (sha256 `7b4fb703…a01a21`, 已用 raw URL 实测), 改为 pin 该 commit.
- 失败 2 原因: runtime 分割路径把 `size_t chunks[64]` + `u8 saved[256*64]` 放栈上, 合计 ~17KB,
  超内核 2KB 栈帧上限. 修复: 暂存移入 `dh_core_binding` (模块 `.bss`, 每槽 ~16.9KB),
  分割路径按槽用 `__atomic_exchange_n` 做 single-flight, 并发分割同槽返回 `DH_E_BUSY`;
  `<=256` 字节快路径不触碰暂存、不加串行.
- 该变更集已用满"修好后重试 1 次"额度, **本轮不再重派 build.yml**; 栈帧修复仅做了
  `make check` (host 90 项) + `make runtime-check` (真实 ABI 5 头) 本地验证, CI 复验待下一轮.

- 成功 run 总墙钟: 约 **1 分 29 秒** (06:28:18Z → 06:29:47Z)。
- artifact: **`droid-hook-android16-6.12`**, 134772 B (上传自 `out/`)。
- 产物: `droid_hook.ko` **290216 B**。

## 首次失败与修复 (真实失败, 非走过场)

run `44875487955` 的 build job 在 `Build droid_hook.ko` 步骤以退出码 2 失败:

```
CC [M] .../src/dh_rw.o ... dh_preflight.o ... dh_module.o
error: .../dh_module.c:211:0: stack frame size (2416) exceeds limit (2048)
       in function 'init_module' [-Werror,-Wframe-larger-than]
1 error generated.
make[2]: *** [.../scripts/Makefile.build:240: .../dh_module.o] Error 1
```

原因: 模块内自检 `dh_run_selftest()` 在栈上声明 `struct dh_ctx` (~2.3 KB: 8 个 256 字节
undo 槽), 且被内联进 `init_module`, 超过内核 2 KB 栈帧上限。host 侧 `make check` 不编译
`dh_module.c`, 因此本地无法发现; 这正说明该 gate 的价值。

修复 (`f4e952f`): 把该 `struct dh_ctx` 改为函数内 `static` (落在模块 .bss, 不进栈);
一次性 load-time 自检、load 串行, 无重入问题。改后本地 `make check`/`make kcheck` 仍全绿,
再派发一次即成功。

## 成功 run `44875690763` 构建日志关键行

```
CC [M]  src/dh_rw.o
CC [M]  src/dh_txn.o
CC [M]  src/dh_insn.o
CC [M]  src/dh_hook.o
CC [M]  src/dh_shadow.o
CC [M]  src/dh_preflight.o
CC [M]  dh_module.o
LD [M]  droid_hook.o
MODPOST droid_hook/Module.symvers
CC [M]  droid_hook.mod.o
CC [M]  .module-common.o
LD [M]  droid_hook.ko
BTF [M] droid_hook.ko
```

CI 内的硬断言 (无 `|| true`):

```
+ test -s out/droid_hook.ko
+ modinfo -F name out/droid_hook.ko | grep -qx droid_hook      # 通过
+ modinfo -F vermagic out/droid_hook.ko
6.12.76-4k SMP preempt mod_unload modversions aarch64
+ ls -l out
-rw-r--r-- 1 root root 290216 ... droid_hook.ko
```

## 本地门禁 (同 commit `f4e952f`)

- `make check` → `droid-hook: host gate clean (7 TUs, 6 suites)`; 90 项:
  contract 6 / hook 19 / insn 19 / preflight 10 / runtime 24 / rw 12, 全 OK
- `make kcheck` → 6 个库层 TU (dh_hook/dh_insn/dh_preflight/dh_rw/dh_shadow/dh_txn) 全 `ok`

## Action 记账

- 本仓库 Action 限额独立: 3 小时内 ≤9 次, 单次 ≤30min。
- 本轮共派发 **2 次** (`44875487955` 失败 + `44875690763` 成功), 时间跨度 ~06:25Z→06:29Z,
  距 9 次上限与 30min 单次上限均有大量余量。
- 失败重试严格为 1 次 (符合"同一变更集最多失败重试一次")。

## runtime 接入后的成功复验 (run `44910310411`)

| 项 | 值 |
|---|---|
| run id | `44910310411` |
| headSha | `9436b66` (与 qemu-gate 复验同一 commit) |
| created → updated | 13:43:53Z → 13:45:25Z |
| host job | 8s (13:43:56→13:44:01) |
| build job | 1m23s (13:44:02→13:45:25) |
| conclusion | **success** |

两个构建都通过: 默认构建 (无 runtime) 与 `DNSK_CORE_INC` 构建
(`CC [M] runtime/dh_core_backend.o` → `LD [M] droid_hook.ko`).
runtime 版 `droid_hook-runtime.ko` 317712 B.

runtime 链接证据 (run log 原文, `llvm-nm` 输出):

```
--- runtime symbols in droid_hook-runtime.ko ---
0000000000003c04 T dh_core_backend_fini
0000000000003ab4 T dh_core_backend_init
0000000000003d04 T dh_core_backend_page_size
0000000000003d14 T dh_core_backend_set_page_size
--- plain symbols (must be empty) ---
```

- plain `.ko` 的同一 `nm` 过滤输出为空 → runtime 未被误编入默认模块
- `modinfo -F name`=`droid_hook`; vermagic
  `6.12.76-4k SMP preempt mod_unload modversions aarch64` (与 DDK 内核一致)
- 这证明 `.bss` 栈帧修复后 runtime TU **真实链接进 `.ko`**, 不再只是"配置就绪"

## 首次门控时的已验证 / 未验证 (历史记录)

**已验证**
- `droid_hook.ko` 在 android16-6.12 DDK 内核头下真实编译、链接、MODPOST、BTF 生成成功
- `modinfo` 名称与 vermagic 合法; artifact 已上传

**未验证 (仅配置就绪或未做)**
- 模块**未加载执行**: CI 只编译, 未 `insmod`, `selftest=1` 自检未在真实内核运行
- `runtime/` 的内核构建方式未提供 (需 droid-lkm-suite core 头), 不在本 Kbuild 内
- QEMU 门控、真机 `insmod`/`rmmod`、告警/panic 扫描均未做

## 复现方式

```sh
gh workflow run build.yml --repo uobe1/droid-hook --ref main
gh run watch <run-id> --repo uobe1/droid-hook --exit-status
gh run view <run-id> --repo uobe1/droid-hook --log
```
