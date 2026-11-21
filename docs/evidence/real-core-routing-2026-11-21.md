# 真 core 路由 (dnsk_core ABI 5) — 2026-11-21

状态: **已验证**。第 3 步真实 rw (读 jiffies + 写读回模块内 u64) 成功, 满足
"只有真 rw 成功才可写真 core 路由已验证"的口径。

## 1. ABI 5 core 产物核实

| 项 | 值 |
|---|---|
| 来源 | droid-lkm-suite run `44867252448`, artifact `core-android16-6.12` |
| 构建 commit | `fb10ac9a5c19167ec86384f9d05...` (`git show fb10ac9a:core/include/dnsk_core.h`: `#define DNSK_CORE_ABI 5`) |
| 文件 | `dnsk_core.ko` 756464 B, sha256 `27d1cb3659310480889a6b983cf229a5cbf72b5a0593b7ea6b4f36f3d462de59` |
| `.modinfo` | `vermagic=6.12.76-4k SMP preempt mod_unload modversions aarch64` |
| 符号 | `T dnsk_core_get`, `EXPORT_SYMBOL_NS_GPL(..., "DROID_LKM_SUITE")` |

设备原有 core 为 ABI 3 (`/data/adb/modules/droid_lkm_suite/kmi/android16-6.12/dnsk_core.ko`,
747872 B), 作为回滚源保留.

## 2. 部署与失败定位

1. `rmmod dnsk_core` (ABI3) → `rc=0`, `/proc/dnsk/status` 消失.
2. `ksud insmod /data/local/tmp/dnsk_core_abi5.ko` → 成功;
   `/proc/dnsk/status`: **`dnsk_core abi=5`**, `kmi=6.12.30-android16-...`, patch/panic ready.
3. 第一次 droid_hook (build `44923717212`, head `ef534a3`, 351072 B) 加载:
   `DH_SELFTEST_RUNTIME_OK` 后 `DH_SELFTEST_FAIL:core_get_missing`, ksud 报
   `init_module failed ... No such file or directory` (-ENOENT).

原因 (逐字核验): Linux 6.12 的 `__symbol_get()` 只搜 builtin `__ksymtab`
(`kernel/module/main.c:1314-1338`, `find_symbol()` 的静态 `symsearch arr[]`);
模块导出的 `dnsk_core_get` 不在其中, 所以 `symbol_get` 必然失败, **不是 ABI 错配**.

修复: 改用 `register_kprobe({ .symbol_name = "dnsk_core_get" })` 经 kallsyms 解析
模块符号, 取 `kp.addr` 后立刻 `unregister_kprobe`; 设备 `CONFIG_KPROBES=y`.
build `44924121975` (head `7480994`, 354032 B, sha256 `85a60608…fa9b`) 加载成功.

## 3. 真 rw 验证 (droid_hook `selftest=1 require_core=1`)

设备 dmesg 原文 (加载前 `dmesg -C`):

```
[112875.981037] DH_SELFTEST_RUNTIME_CHUNKS=3
[112875.981052] DH_SELFTEST_RUNTIME_OK
[112875.981055] DH_SELFTEST_OK
[112875.981786] DH_CORE_GET=ok abi=5 size=80
[112875.981852] DH_CORE_JIFFIES=4323111291,4323111291
[112875.981862] DH_CORE_WRITE_READ=ok value=1234605616436508552
[112875.981869] DH_SELFTEST_CORE_OK
[112875.981875] droid_hook: loaded (abi 1)
```

| 断言 | 结果 |
|---|---|
| `dnsk_core_get(DNSK_CORE_ABI=5, sizeof=80)` | `DH_CORE_GET=ok abi=5 size=80` (无 `-EPROTO`) |
| 读 `jiffies` 两次 | `4323111291,4323111291`, 单调不减 (同 jiffy 内两次读, 相等合法) |
| 写读回模块内 `u64` | 写入 `0x1122334455667788` (=1234605616436508552), 读回一致 → `DH_CORE_WRITE_READ=ok` |
| 汇总 marker | `DH_SELFTEST_CORE_OK` |

## 4. 卸载顺序 / 引用计数

1. 加载后 `lsmod`: `droid_hook 159744 0`, `dnsk_core 73728 **1**` (droid_hook 通过
   `dnsk_core_get` 内部的 `__module_get` 持有 core 引用).
2. `rmmod droid_hook` → `droid_rmmod_rc=0`, `LSMOD_DROID=0`, sysfs 消失,
   `dmesg`: `[112883.853849] droid_hook: unloaded`; `dnsk_core Used by` 回到 **0**
   → `module_put(api->owner)` 无泄漏.
3. `rmmod dnsk_core` (ABI5) → `abi5_rmmod_rc=0`, `/proc/dnsk/status` 消失.
4. 恢复设备原状态: `ksud insmod /data/adb/modules/droid_lkm_suite/kmi/android16-6.12/dnsk_core.ko`
   → `restore_rc=0`, `/proc/dnsk/status`: `dnsk_core abi=3`.

## 5. 扫描

- `BUG:|Oops|panic|[permanent]` → **0**
- `version magic|invalid module format|disagrees about version|Unknown symbol|unresolved symbol|no symbol version` → **0**
- 未 reboot; 未改设备持久配置; 测试后已恢复 ABI3 core + 无 droid_hook.

## 6. 边界

- core ABI5 来自 suite ci-fast 产物 (build `fb10ac9a`), 不是设备默认安装的 ABI3; 本次是
  **临时替换→验证→恢复**, 不代表设备日常状态.
- 写验证只写 droid_hook 自己的 `u64`, 未写内核全局数据; 读验证是 `jiffies`.
- 真 core 路由的 bootstrap 依赖 kprobe 解析 (因为 `symbol_get` 只搜 builtin); 这是实现细节,
  不是 ABI 能力.
- 未做压力/并发/异常注入; 未触发 panic.

## 7. Action 记账

- 一次性 flush @15:34:44Z 后: `44919948478` (1/9), `44923717212` (2/9, 失败),
  `44924121975` (3/9, 修复后重试成功). 真机操作 0 Action.
