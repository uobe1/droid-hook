# droid-hook 真机加载/卸载门控 — 2026-11-21

本文件记录 `droid_hook.ko` 在真实 Android 设备上通过 **ksud** 加载、执行
`selftest=1`、卸载并确认无残留的证据。只以设备命令原文 / dmesg 原文为准。

**这是本项目首次在 beryl 设备上真实 `insmod` / `rmmod` 成功。**

## 环境

| 项 | 值 |
|---|---|
| 设备内核 | `6.12.30-android16-5-g1ed949324a3e-ab13881345-4k` |
| 非交互 ssh `id` | `uid=10264(u0_a264) ... context=u:r:untrusted_app_27:s0` |
| 提权方式 | `su -c '<cmd>'` → `uid=0(root) ... context=u:r:ksu:s0`, `su_rc=0` |
| ksud | `/data/adb/ksu/bin/ksud -> /data/adb/ksud` (该版本只有 `insmod`, **没有 `rmmod`**) |
| 卸载工具 | `/system/bin/rmmod -> toybox` (`Toybox 0.8.12-android`) |
| 设备已有 core | `dnsk_core 73728 0`; `/proc/dnsk/status`: `dnsk_core abi=3` (与适配层的 ABI 5 不一致, 见 [real-core-routing-plan](./real-core-routing-plan.md)) |

## 产物

| 项 | 值 |
|---|---|
| 来源 | CI build run `44910310411` artifact `droid-hook-android16-6.12` |
| 文件 | `droid_hook-runtime.ko` (含 runtime 适配层) |
| 大小 / sha256 | `317712` B / `97d5a75b1baafb5f990a6a996761efbcdd83216861963e13de99cdcb1bb1b66f` |
| 设备落点 | `/data/local/tmp/droid_hook.ko`, root:root 0644, sha256 与本地一致 |

注意: 该产物构建于 runtime selftest 提交 (`2f84a6e`) **之前**, 因此只有
`DH_SELFTEST_OK`, 没有 `DH_SELFTEST_RUNTIME_*` marker.

## 执行与原文

| 步骤 | 命令/结果 |
|---|---|
| 0 ksud 路径 | `lrwxrwxrwx ... /data/adb/ksu/bin/ksud -> /data/adb/ksud` |
| 1 推文件 | `scp` 到 Termux home; root 下 `cp` 到 `/data/local/tmp` 并 `chmod 644` |
| 2 校验 | `sha256sum` = `97d5a75b…b66f`; `ls -l` = `317712` B |
| 3 基线 | `LSMOD_COUNT=0`; `/sys/module/droid_hook: No such file or directory`; `dmesg -C` → `DMESG_CLEAR_RC=0` |
| 4 加载 | `su -c '/data/adb/ksu/bin/ksud insmod /data/local/tmp/droid_hook.ko selftest=1'` → `Loaded kernel module: /data/local/tmp/droid_hook.ko`, `ksud_rc=0` |
| 5 断言 | `[104806.675229] DH_SELFTEST_OK`; `[104806.675246] droid_hook: loaded (abi 1)`; `lsmod` 有 `droid_hook 135168 0`; `/sys/module/droid_hook` 存在 |
| 6 卸载 | `ksud rmmod` 不存在 (`rmmod_rc=2`, `unrecognized subcommand`); 改用 `/system/bin/rmmod droid_hook` → `rmmod_rc=0`; `LSMOD_COUNT=0`; `/sys/module/droid_hook: No such file or directory`; `[104820.862027] droid_hook: unloaded` |
| 7 告警 | `COUNT=$(dmesg | grep -Ec 'BUG:|Oops|panic|\[permanent\]')` → **0** |
| 8 错误分类 | `VERMAGIC=0` (`version magic|invalid module format`)、`CRC=0` (`disagrees about version|no symbol version`)、`SYMBOL=0` (`Unknown symbol|unresolved symbol`); `LSMOD_COUNT=0`; sysfs 不存在 |

- 未执行 reboot; 未安装任何东西; 未改设备配置.
- 说明: 在最终 ksud 路径之前, 曾用静态 glibc `lkmctl-android` 对同一 `317712` B 产物
  做过一次加载/卸载 (同样 `DH_SELFTEST_OK`、`lsmod` 0、扫描干净); 按指示, 正式证据
  以上面的 ksud 路径为准.

## 加载错误分类 (vermagic / CRC / 布局 / 符号)

这四层不是一回事:

| 层 | 形态 | 谁检查 | 失败文案 |
|---|---|---|---|
| vermagic | `.modinfo` 一行字符串 | 内核 `check_modinfo()` | `version magic ... should be ...` (ENOEXEC) |
| CRC / symbol version | `__versions` 段每符号一条 | 内核 `check_version()` | `disagrees about version of symbol <name>` |
| struct module 布局 | 结构体偏移 | 加载器按 BTF 适配 | 崩溃/字段错位, 不一定有文案 |
| 未定义符号 | ELF `SHN_UNDEF` | 加载器绑定 / 内核解析 | `Unknown symbol ...` |

本次 (dmesg 在加载前已清空, 窗口仅含本次加载): `VERMAGIC=0`、`CRC=0`、`SYMBOL=0`,
**四类错误均未触发**; ksud 完成了符号绑定与加载, `selftest=1` 通过, 卸载干净.

口径 (准确版):
- **允许**加载器按设计做 vermagic 重写、`struct module` 布局适配、未定义符号绑定.
- **禁止**手填假 CRC 强行加载后宣称成功, 也禁止 `--force` 硬闯; 这是证据真实性问题.
- 若将来撞上 CRC, 应作为"加载器支持 `__versions` 处理"的明确功能开发来记录
  (行为/失败文案/回退路径), 不是本次门控的失败.
- 补充: GKI `android16-6.12` KMI 的 `module_layout` CRC 稳定, DDK 同为 6.12.x,
  预期大概率不撞 CRC; 本次实测确未撞.

## 结论

**真机已验证**
- `ksud insmod` 成功加载 DDK 6.12.76 构建的模块到 6.12.30-android16 设备;
  vermagic / CRC / 未定义符号三类 dmesg 错误计数均为 0 (布局由 BTF 适配, 未崩溃).
- `selftest=1` 在真实内核执行并输出 `DH_SELFTEST_OK` (`droid_hook: loaded (abi 1)`).
- 卸载成功 (`rmmod_rc=0`), `lsmod` 计数 0, `/sys/module/droid_hook` 不存在,
  dmesg `droid_hook: unloaded`.
- 加载前清空 dmesg 后, `BUG:|Oops|panic|[permanent]` 扫描计数 0.

**未验证 / 边界**
- 设备上加载的是 `317712` B 旧产物, **没有 runtime selftest marker**; runtime 分割/恢复
  自检只在 QEMU (mock core) 跑过, 未在真机执行.
- runtime 适配层的真实 core 路由、真实 quiesce 窗口仍未验证.
- 只做了一次加载/卸载, 未做压力/并发/异常注入; 未触发 panic, 因而 pstore 路径未验证.

## Action 记账

- 真机门控不消耗 GitHub Action; 本窗口仍 **8/9**, 未派任何 workflow.
