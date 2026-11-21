# Action 窗口核算 — 2026-11-21

规则: 本仓库每滚动 3 小时最多 9 次 workflow run。派发前用 `gh run list` 实算,
不按记忆。

## 最近 9 条 run (2026-11-21T15:25:28Z 拉取)

| # | run id | createdAt (UTC) | workflow | 结果 |
|---|---|---|---|---|
| 1 | `44911364668` | 13:56:41 | qemu-gate | success |
| 2 | `44911184191` | 13:54:29 | qemu-gate | failure |
| 3 | `44910470845` | 13:45:44 | qemu-gate | success |
| 4 | `44910310411` | 13:43:53 | build | success |
| 5 | `44909471977` | 13:33:42 | qemu-gate | failure |
| 6 | `44909207598` | 13:30:39 | qemu-gate | failure |
| 7 | `44909065331` | 13:28:59 | build | failure |
| 8 | `44908866295` | 13:26:29 | build | failure |
| 9 | `44876857725` | 06:42:58 | build | success |

注: 更早的 `44875487955` / `44875690763` 已从 `gh run list` 消失 (API 404),
当前可见共 9 条.

## 两种算法

1. **按"第 9 近 + 3h"** (用户口径的字面定义):
   第 9 近 = `44876857725` @ 06:42:58Z → 到期 **09:42:58Z**, 已过.
   当前 3h 窗口 `[12:25:28, 15:25:28]` 内实际只有 **8** 条 (13:26:29–13:56:41),
   此刻再派发是窗口内第 9 条, 按滚动规则仍 ≤9.
2. **按"最新 run + 3h"** (保守口径):
   最新 = `44911364668` @ 13:56:41Z → **16:56:41Z**.

## 决定

为避免两种口径的歧义, 取较晚者: **2026-11-21T16:57:00Z 之前不派发**.
在此之前 3h 窗口内不会新增 run; 到点后派发 1 次 `build.yml` (最新 head),
仍远低于 9 次上限.

## 下一轮计划 (到期后)

1. `build.yml` ×1, latest head (含 runtime selftest), 产物
   `droid-hook-android16-6.12` 中的 `droid_hook-runtime.ko`.
2. 真机 (0 Action): `gh run download` → `scp` Termux home → `su -c cp/chmod/sha256sum`
   → `su -c '/data/adb/ksu/bin/ksud insmod … selftest=1'`.
3. 目标 marker: `DH_SELFTEST_RUNTIME_OK` + `DH_SELFTEST_RUNTIME_CHUNKS=3`
   (+ 原有 `DH_SELFTEST_OK`).
4. 卸载: `/system/bin/rmmod` (该 ksud 无 rmmod); 断言 `lsmod` 0、无 sysfs、
   `BUG:/Oops/panic/[permanent]` 0; **不 reboot**.
5. 失败: 最多修后重试 build 1 次; 设备侧失败只取证并停.

## 用户授权提前 flush (一次性)

- 时间: **2026-11-21T15:34:44Z**
- 性质: 用户**一次性**授权提前 flush 本窗口 (每次 Action 都有明确用途且做了本地检查);
  **不是策略变更**, 3h ≤9 的规则本身不变.
- 记账: flush 后计数从 **1** 起算; 本次 `build.yml` 为 flush 后第 1 次.
- flush 只解除时间限制, **不解除**"一次变更集一次验证": build 失败最多修后重试 1 次.
