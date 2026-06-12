# Phase 0: DTS 冲突彻底解决 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 解决 kona-port.dtsi 与 sm8250.dtsi 之间所有 DTS 节点冲突，扫完 23 个 kona-*.dtsi 文件，确保 DTB + DTBO 编译通过且携带正确的 bootloader 匹配字段。

**Architecture:** 分批进行：先修复 Level 2/3 冲突（6 项），再扫描 23 个子 dtsi 文件的标签引用，最后编译验证 8T 的完整 DTB + DTBO 链路。

**Tech Stack:** Device Tree Compiler (dtc), bash, grep/sed

---

### Task 1: Level 2 (地址差异) 验证 — NPUCC & VIDEOCC

**Files:**
- Read: `arch/arm64/boot/dts/qcom/kona-port.dtsi` (lines near 1526, 1544)
- Read: `arch/arm64/boot/dts/qcom/sm8250.dtsi` (lines near npucc@ and videocc@)
- Verify: `arch/arm64/boot/dts/vendor/qcom/kona.dtsi` (4.19 source of truth)

- [ ] **Step 1: 在 4.19 源中查 NPUCC 真实地址**

```bash
grep -n "npucc\|NPUCC\|npuss" E:\5.10-main\android_kernel_oneplus_sm8250_los_noksu-upstream-latest\arch\arm64\boot\dts\vendor\qcom\kona.dtsi | head -20
```

- [ ] **Step 2: 在 5.10 上游中查 NPUCC 地址**

```bash
grep -n "npucc\|npu@" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\sm8250.dtsi
```

Expected: 找到 @9910000 或 @9980000，哪个是硬件正确地址。

- [ ] **Step 3: 验证 kona-port.dtsi 中的 NPUCC 引用**

```bash
grep -n "&npucc\|npucc" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi | head -10
```

- [ ] **Step 4: 同样验证 VIDEOCC 地址**

```bash
grep -n "videocc\|video_cc\|video@" E:\5.10-main\android_kernel_oneplus_sm8250_los_noksu-upstream-latest\arch\arm64\boot\dts\vendor\qcom\kona.dtsi | head -10
grep -n "videocc\|video@" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\sm8250.dtsi | head -10
```

- [ ] **Step 5: 确认结论并记录**

如果地址一致：kona-port.dtsi 中无独立节点定义，用上游的即可。如果 4.19 地址与 5.10 不同：确认以 5.10 sm8250.dtsi 为准（上游经过更严格验证），记录到冲突文档。

---

### Task 2: Level 3 (规范违反) — 逐项修复

**Files:**
- Modify: `arch/arm64/boot/dts/qcom/kona-port.dtsi`
- Read: `arch/arm64/boot/dts/qcom/kona-port-5.10-conflicts.md`

- [ ] **Step 1: 查找 arch_timer 在 kona-port.dtsi 中的定义**

```bash
grep -n "arch_timer\|arm,armv7-timer" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi
```

Expected: 如果在 &soc 内有定义，需要移到顶层。

- [ ] **Step 2: 查找 clock_rpmh 定义**

```bash
grep -n "clock_rpmh\|rpmhclk\|qcom,rpmh" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi | head -20
```

Expected: 如果与 sm8250.dtsi 的 rpmhcc 重叠，删除 kona-port 中的 rpmhclk 节点，引用改往 &rpmhcc。

- [ ] **Step 3: 查找空 clocks{} 节点**

```bash
grep -n "clocks {\n};" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi
```

如果存在（原文档说 line 1835），直接删除。

- [ ] **Step 4: 验证 watchdog compatible 已迁移**

```bash
grep -n "qcom,msm-watchdog\|qcom,apss-wdt" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi
```

Expected: kona-port.dtsi 不应再定义 watchdog 节点（上游 sm8250.dtsi 有 &watchdog）

- [ ] **Step 5: 查找 disp_rsc 定义**

```bash
grep -n "disp_rsc\|rsc@af20000\|apps_rsc" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\kona-port.dtsi
```

Expected: 如有旧 rsc@af20000 定义，删除；引用改用 5.10 的 &apps_rsc。

- [ ] **Step 6: Commit**

```bash
git add arch/arm64/boot/dts/qcom/kona-port.dtsi
git commit -m "fix: resolve Level 2/3 DTS conflicts in kona-port.dtsi

- Verify NPUCC/VIDEOCC addresses against upstream sm8250.dtsi
- Remove arch_timer from &soc (5.10 requires top-level)
- Remove clock_rpmh (overlaps with upstream rpmhcc)
- Remove empty clocks{} node (4.19 artifact)
- Remove disp_rsc@af20000 (moved to @18200000 in 5.10)"
```

---

### Task 3: 23 个 kona-*.dtsi 子文件标签扫描

**Files:**
- Scan: `arch/arm64/boot/dts/qcom/kona-*.dtsi` (23 files)
- Reference: `arch/arm64/boot/dts/qcom/kona-port-5.10-conflicts.md`

- [ ] **Step 1: 对全部 23 个 kona-*.dtsi 做旧标签扫描**

```bash
cd E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom
for f in kona-*.dtsi; do
  matches=$(grep -c "&clock_gpucc\b\|&clock_camcc\b\|&clock_dispcc\b\|&clock_videocc\b\|&clock_npucc\b\|&qmp_aop\b\|&ipcc_mproc\b\|&wdog\b" "$f" 2>/dev/null || true)
  if [ "$matches" -gt 0 ] 2>/dev/null; then
    echo "$f: $matches old-label hits"
    grep -n "&clock_gpucc\b\|&clock_camcc\b\|&clock_dispcc\b\|&clock_videocc\b\|&clock_npucc\b\|&qmp_aop\b\|&ipcc_mproc\b\|&wdog\b" "$f"
  fi
done
```

- [ ] **Step 2: 有冲突的替换**

对每个有旧标签的文件，用 sed 替换：
```
&clock_gpucc → &gpucc
&clock_camcc → &camcc
&clock_dispcc → &dispcc
&clock_videocc → &videocc
&clock_npucc → &npucc
&qmp_aop → &aoss_qmp
&ipcc_mproc → &ipcc
&wdog → &watchdog (仅当引用的是标签，不是 interrupt-names 中的字符串)
```

- [ ] **Step 3: Commit 每个受影响的文件**

```bash
git add arch/arm64/boot/dts/qcom/kona-*.dtsi
git commit -m "fix: migrate old DTS labels to upstream names in kona-*.dtsi

Replace &clock_* → &gpucc/&camcc/&dispcc/&videocc/&npucc
Replace &qmp_aop → &aoss_qmp
Replace &ipcc_mproc → &ipcc
Replace &wdog → &watchdog"
```

---

### Task 4: 编译验证 DTB + DTBO

**Files:**
- Verify: `arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb`
- Verify: `arch/arm64/boot/dts/oplus/kona-kebab.dtbo`

- [ ] **Step 1: DTC 语法检查 — 8T 单体 DTB**

```bash
dtc -I dts -O dtb -o /dev/null \
  -i E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom \
  -i E:\5.10-main\5.10-main\include \
  E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\sm8250-oneplus8t.dts 2>&1
```

- [ ] **Step 2: DTC 语法检查 — 8T DTBO**

```bash
dtc -I dts -O dtb -o /dev/null \
  -i E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom \
  -i E:\5.10-main\5.10-main\arch\arm64\boot\dts\oplus \
  -i E:\5.10-main\5.10-main\include \
  E:\5.10-main\5.10-main\arch\arm64\boot\dts\oplus\kona-kebab.dts 2>&1
```

- [ ] **Step 3: 反编译验证 bootloader 匹配字段**

```bash
dtc -I dtb -O dts <generated-8t.dtb> 2>/dev/null | grep -A2 "qcom,board-id\|qcom,msm-id\|oplus,dtsi_no\|oplus,pcb_range\|compatible"
```

预期包含:
```
qcom,board-id = <0x10008 0x00>;
qcom,msm-id = <0x164 0x10000>, <0x164 0x20000>, <0x164 0x20001>;
oplus,dtsi_no = <19805 20809>;
oplus,pcb_range = <0 56>;
compatible = "oneplus,kebab", "qcom,kona-mtp", "qcom,kona", "qcom,mtp", "qcom,sm8250";
```

- [ ] **Step 4: 更新冲突文档**

标记已解决的冲突为 ✅，更新 kona-port-5.10-conflicts.md。

- [ ] **Step 5: Commit**

```bash
git add arch/arm64/boot/dts/qcom/kona-port-5.10-conflicts.md
git commit -m "docs: update DTS conflict report — mark Level 1-3 resolved"
```

---

### Task 5: 完整内核编译验证

- [ ] **Step 1: 8T defconfig 编译**

```bash
# 仅做 DTB 编译验证（全内核编译需要交叉工具链）
# 验证 sm8250-oneplus8t.dtb 是 Makefile target
grep "sm8250-oneplus8t" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\Makefile
```

- [ ] **Step 2: 确认 KMI 基线**

验证 `build.config.msm.gki` 中 8T defconfig 路径正确配置。

- [ ] **Step 3: 记录 Phase 0 完成状态**

写入 `docs/superpowers/plans/2026-06-12-phase0-completion.md`
