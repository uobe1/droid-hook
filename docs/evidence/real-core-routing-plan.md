# 真 core 路由集成方案 (runtime backend ↔ dnsk_core ABI 5)

状态: **方案 (0 Action)**。截至 2026-11-21, 真机只验证了 mock-core 的 runtime 自检与
ksud 加载/卸载; 真 core 路由尚未执行。

## 0. 设备实测的前置事实

| 项 | 值 |
|---|---|
| 设备内核 | `6.12.30-android16-5-g1ed949324a3e-ab13881345-4k` |
| 已加载 core | `lsmod`: `dnsk_core 73728 0` |
| core 自报 | `/proc/dnsk/status`: `dnsk_core abi=3`, `patch=ready`, `panic=ready` |
| 我们的适配层 | `runtime/dh_core_backend.c` 按 **`DNSK_CORE_ABI 5`** 编译 (pinned `dnsk_core.h`, `f6d325be`) |
| droid_hook 链接 | `DNSK_CORE_INC` 版 `.ko` 已含 runtime TU (`nm` 4 个 `dh_core_backend_*`), 但**没有代码调用 `dnsk_core_get()`** |

**当前最大阻塞**: 设备 core 是 ABI 3, 适配层是 ABI 5. `dnsk_core_get(5, sizeof)` 在
ABI 3 core 上会返回 `ERR_PTR(-EPROTO)` (见 `core/dnsk_core.c:82-89`). 因此真 core 路由
的第一步不是写 droid_hook 代码, 而是把 **ABI 5 core** 部署到设备 (droid-lkm-suite 的
ABI 5 构建), 或者明确决定"适配层回退支持 ABI 3" (不建议, 会分叉契约).

## 1. 需要什么

1. **ABI 5 `dnsk_core.ko`**, 与设备 KMI (`6.12.30-android16`) 匹配, 先在设备加载;
   `dnsk_core_get(DNSK_CORE_ABI, sizeof(struct dnsk_core_api))` 成功返回 `struct
   dnsk_core_api *`.
2. **符号入口**: `dnsk_core_get` 是 `EXPORT_SYMBOL_NS_GPL(..., "DROID_LKM_SUITE")`
   (`core/dnsk_core.c:91-95`).
   - 硬链接方案: droid_hook 里 `extern` 声明 + `MODULE_IMPORT_NS("DROID_LKM_SUITE")`,
     构建时用 core 的 `Module.symvers` (`KBUILD_EXTRA_SYMBOLS=...`);
   - 软链接方案: 留 `SHN_UNDEF`, 由 ksud/加载器按 kallsyms 绑定 (本仓库真机门控已走
     ksud 绑定路径).
3. **API 校验**: `api->magic == DNSK_CORE_MAGIC`, `api->abi == DNSK_CORE_ABI`,
   `api->rw != NULL` (适配层 `validate_api()` 已做; 调用方先做一遍 fail-closed).

## 2. droid_hook 侧需要加的代码 (尚未实现)

在 `dh_module.c` 的 `DH_WITH_RUNTIME` 分支增加可选 core 绑定路径:

- 模块参数 `require_core=1` (默认 0): 为 1 时, core 不可用则 **拒绝加载**;
  为 0 时, 模块仍可用 mock selftest 加载, 但明确打印 runtime disabled.
- init:
  1. `api = dnsk_core_get(DNSK_CORE_ABI, sizeof(struct dnsk_core_api));`
  2. `IS_ERR(api)` → `require_core` 时 `return PTR_ERR(api)`; 否则
     `pr_warn("dh: core unavailable (%ld), runtime disabled")` 并继续 mock 路径.
  3. `dh_core_backend_init(&core_be, &core_st, api)`; 失败则 fail-closed.
  4. 真实 rw 自检 (仅在 core 路径启用时):
     - READ: `api->resolve("jiffies")` 得到地址, 用 `api->rw` READ 8 字节 (只读, 安全);
     - WRITE+read-back: 只写 **droid_hook 自己的 static `u64`** (地址是合法内核地址),
       再 READ 回来比对; 不碰内核数据.
     - 成功 marker: `DH_SELFTEST_CORE_OK`; 失败 `DH_SELFTEST_FAIL:core_*`.
- exit:
  1. 移除本模块在 core 注册的 hooks/features (若未来使用);
  2. `dh_core_backend_fini(&core_be)`;
  3. `module_put(api->owner)` — `dnsk_core_get()` 内部 `__module_get(THIS_MODULE)`
     (`core/dnsk_core.c:88`), 头文件明确要求用 `module_put(owner)` 释放
     (`core/include/dnsk_core.h:413/426`).

## 3. 加载/卸载顺序

```text
insmod dnsk_core.ko              # 先: core ABI 5 ready
insmod droid_hook.ko require_core=1 selftest=1
...
rmmod droid_hook                 # 先: 释放 api 引用 + 自己的 hooks
rmmod dnsk_core                  # 后: 引用归零才能卸载
```

- core **不 pin** droid_hook (`struct dnsk_hook_req` 的 `owner` 只用于诊断,
  `dnsk_core.h:141-144`), 所以 droid_hook 必须先自己撤掉所有注册项; 顺序只能靠
  用户/脚本保证.
- droid_hook **持有** core 的 module ref; core 在 refcnt>0 时无法 rmmod (内核拒绝).
- 若 droid_hook 忘记 `module_put`, core refcnt 永久 +1, `rmmod dnsk_core` 会失败;
  这是最容易留下的残留.

## 4. 要验证什么

1. core ABI 5: `cat /proc/dnsk/status` 显示 `abi=5`.
2. 绑定: `dnsk_core_get` 成功; `dh_core_backend_init` 成功; `be.name == "dnsk_core_rw5"`.
3. 真实 READ: 读 `jiffies` 8 字节返回 `DH_OK`, 值非零/可递增 (采样两次).
4. 真实 WRITE+read-back: 写 droid_hook 自己的 static `u64`, 读回一致.
5. 失败路径: core 不存在时 `require_core=1` 的 insmod **失败**(真实 errno),
   `require_core=0` 时 runtime disabled 且**不出现** `DH_SELFTEST_CORE_OK`.
6. 卸载: droid_hook rmmod 后 core refcnt 回到加载前; 再 rmmod core 成功; 无残留.
7. 告警: `BUG:/Oops/panic/[permanent]` 与 `version magic/module_layout/...` 均为 0.

## 5. 风险 (按严重度)

1. **ABI 版本错配** (当前实际存在): 设备 core=3, 适配层=5 → `-EPROTO`; 必须先部署
   ABI 5 core. 这是当前最难/最前置的一步.
2. **双 LKM 生命周期**: refcount 漏 put 或 core 先卸载 → core 无法卸载或 UAF/panic;
   卸载顺序必须脚本化+断言.
3. **符号/CRC/vermagic**: core 与 droid_hook 必须同一 kernel build (或加载器绑定);
   GKI `android16-6.12` 的 `module_layout` CRC 稳定, 但 core 与 droid_hook 若跨构建,
   其他导入符号仍可能 CRC 不符.
4. **安全**: rw 自检只写模块自己的变量; 不写内核全局数据.
5. **命名空间**: 硬链接 `dnsk_core_get` 需要 `MODULE_IMPORT_NS("DROID_LKM_SUITE")`,
   否则 modpost 拒绝.

## 6. 验收定义

- 设备上 core ABI 5 + droid_hook `require_core=1` 加载成功, 串口/dmesg 出现
  `DH_SELFTEST_CORE_OK`;
- 真实 READ/WRITE 断言通过;
- 逆序卸载成功, core refcnt 归零, 无残留;
- 全程无 panic/Oops/WARN; **不 reboot**.
