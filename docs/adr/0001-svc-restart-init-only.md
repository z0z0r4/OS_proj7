# ADR 0001：服务重启仅限 init

- 状态：已采纳（待人工审核）
- 日期：2026-11-07
- 关联：P0-C；U1

## 背景

`svc_restart` 允许任意用户进程在服务停止期间重新拉起 fssrv。除越权外，检查与 `spawn_fsserver()` 之间无锁，两个调用者可同时通过检查，产生两个 boot 进程；注册失败者退出，属于可触发的服务不稳。

## 决策

1. 内核在 `sys_svc_restart` 中要求 `myproc() == initproc`；init 是 `init.c` 中唯一的监督者。
2. 因只有一个合法调用者，保留现有「检查后 spawn」结构，不加锁。
3. 确定性验收使用仅在 fault 构建中存在的测试钩子：
   - `KEXTRA=-DP0_TEST_HOOKS` 由顶层 `fault` 目标注入 kernel 与 fs.img 两次构建；
   - 钩子用 `__atomic` 访问共享标志，暂停期间不持有任何 spinlock；
   - 普通构建与其他测试构建中钩子被预处理剔除。

## 后果

- 越权重启被拒绝；正常 init 重启行为与返回值不变。
- 用户可见变化：非 init 进程调用 `svc_restart` 恒返回 -1（仅 fsfault 的失败注入依赖 kill，不受影响）。
- 残余风险：孤儿退出与 init 重启交错时，仍存在 spawn→register 窗口；如需彻底关闭，应在内核维护「restart in progress」状态（后续单元评估）。
- 测试钩子不会进入发布/普通构建；但若在 `src` 内手工 `make` 且依赖上一次 fault 构建的产物，可能因 `clean` 缺失而混用，受支持的顶层目标都会先清理。
