# Phase 1B: DT 绑定对齐 — 让 GPIO/Pinctrl 真正生效 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 oplus_battery_kona.c 中的 GPIO 解析，使其匹配现有 DTS 中的 `qcom,<name>-gpio` 属性。完成后 8T 充电器插入能通过 ccdetect GPIO 检测到，USB 温度监控线程正常启动。

**Architecture:** 4.19 使用 `of_get_named_gpio(node, "qcom,ccdetect-gpio", 0)` 直接读 int。5.10 必须使用 GPIO descriptor API。方案：用 `devm_gpiod_get_index(dev, con_id, idx, flags)` 或直接修复 con_id 命名。但 `devm_gpiod_get` 期望 DT 属性为 `{con_id}-gpios`（复数、无 qcom 前缀），而 DTS 中是 `qcom,ccdetect-gpio`（单数、qcom 前缀）。只能用命名属性的变体。

**正确方案**: 使用 `devm_fwnode_gpiod_get_index(dev, child_node, con_id, index, flags, label)` 或直接通过 `of_get_named_gpio_flags()` → `gpio_to_desc()` 做一次性桥梁转换。或者更简单的——直接修复 DTS 给 `pm8150b_charger` 节点增加 `gpios` 后缀的属性。

**最终采用方案: 修复 DTS**。因为 5.10 的 GPIO descriptor API 强制要求在 DT 中使用标准 `*-gpios` 命名。修改 sm8250-oneplus8t-mtp.dts 的 `pm8150b_charger` 节点，为每个 OPLUS 使用的 GPIO 添加标准命名属性。

**Tech Stack:** DTS, devm_gpiod_get, pinctrl

---

### 交叉验证流程

```
[实现代理] → 修复 DTS + 验证 C 代码 → [审核代理] → 逐属性确认匹配 → [报告]
```

---

### Task 1: DTS — pm8150b_charger 节点添加标准 GPIO 属性

**Files:**
- Modify: `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-mtp.dts`

**4.19 基线 (parse DT 函数在 4.19 oplus_battery_msm8250.c ~11770-12015):**

| 4.19 DT 属性 | GPIO 引脚 | 用途 |
|---------------|-----------|------|
| `qcom,ccdetect-gpio` | `<&tlmm 14 0x00>` | CC 检测 (线1689-1690) |
| `qcom,ship-gpio` | `<&tlmm 22 0x00>` | 船运模式 |
| `qcom,shipmode-id-gpio` | 无 (不在 DTS 中) | shipmode-id |
| `qcom,wired_conn-gpio` | 无 (不在 DTS 中) | 有线连接器 |
| `qcom,otg_en-gpio` | `<&pm8150l_gpios 4 0x00>` | OTG (在 pdphy 节点，不在 charger) |
| `qcom,idt_en-gpio` | `<&pm8150_gpios 10 0x00>` | 无线充电 (在 wlchg_rx 节点) |
| `qcom,wrx_en-gpio` | `<&pm8150l_gpios 8 0x00>` | 无线接收 (在 oneplus_wlchg 节点) |
| `qcom,wrx_otg-gpio` | `<&pm8150l_gpios 11 0x00>` | WRX OTG (在 oneplus_wlchg 节点) |

**5.10 C 代码期望 (oplus_battery_kona.c GPIO parser 中的 con_id → 标准 DT 属性名):**

| con_id | 期望 DT 属性 | 实际 DTS 有? | 解决方案 |
|--------|------------|------------|---------|
| `"ccdetect"` | `ccdetect-gpios` | 无 | 添加 `ccdetect-gpios = <&tlmm 14 0x00>;` |
| `"shipmode-id"` | `shipmode-id-gpios` | 无 | 添加 `shipmode-id-gpios = <&tlmm 22 0x00>;` |
| `"wired-conn"` | `wired-conn-gpios` | 无 | 此 GPIO 在 4.19 DTS 中也不存在，标记为可选 |
| `"otg-en"` | `otg-en-gpios` | 在 pdphy 节点 | 在 charger 节点添加 `otg-en-gpios = <&pm8150l_gpios 4 0x00>;` |
| `"idt-en"` | `idt-en-gpios` | 在 wlchg_rx 节点 | 在 charger 节点添加 `idt-en-gpios = <&pm8150_gpios 10 0x00>;` |
| `"wrx-en"` | `wrx-en-gpios` | 在 oneplus_wlchg 节点 | 在 charger 节点添加 `wrx-en-gpios = <&pm8150l_gpios 8 0x00>;` |
| `"wrx-otg"` | `wrx-otg-gpios` | 在 oneplus_wlchg 节点 | 在 charger 节点添加 `wrx-otg-gpios = <&pm8150l_gpios 11 0x00>;` |

**注意**: 在一个 DT 节点里不能有两个同名的 `{name}-gpios` 属性——但这里没问题，因为这些 GPIO 信号即使物理上关联着其他子系统的功能（无线充电等），在 SMB5 charger 节点层面引用它们是正确做法。4.19 的做法就是从 charger 节点直接 `of_get_named_gpio(node, "qcom,idt_en-gpio", 0)`。

- [ ] **Step 1: 读 DTS 中 pm8150b_charger 节点 (line ~1600-1723) 完整上下文**

- [ ] **Step 2: 在 pm8150b_charger 节点中，现有 pinctrl-names 列表之后，添加标准 gpio 属性行:**

```dts
/* OPLUS Kona charger adapter GPIOs — standard 5.10 descriptor API */
ccdetect-gpios = <&tlmm 14 0x00>;
shipmode-id-gpios = <&tlmm 22 0x00>;
otg-en-gpios = <&pm8150l_gpios 4 0x00>;
idt-en-gpios = <&pm8150_gpios 10 0x00>;
wrx-en-gpios = <&pm8150l_gpios 8 0x00>;
wrx-otg-gpios = <&pm8150l_gpios 11 0x00>;
```

- [ ] **Step 3: 交叉验证 — 审核代理确认每个 gpios 属性引脚编号与 4.19 DTS 中 qcom,*-gpio 一致**

---

### Task 2: C 代码 — 修正 oplus_battery_kona.c 的 devm_gpiod_get con_id

**Files:**
- Modify: `drivers/power/supply/qcom/oplus_battery_kona.c`

修复每个 GPIO parser 中的 con_id，确保与 DTS 中的 `{con_id}-gpios` 属性名匹配。当前 con_id 已经是正确的（ccdetect, shipmode-id, otg-en, idt-en, wrx-en, wrx-otg），不需要改。

**但有一个更关键的修复**: `oplus_kona_parse_shipmode_dt()` 把 shipmode-id GPIO 方向设为 `GPIOD_IN`。在 4.19 中 shipmode-id 是 input 没错（`oplus_shipmode_id_gpio_init` 调 `gpio_direction_input`）。维持不变。

**验证**: 确保 deinit_action 不会在 usbtemp kthread 停止前就清理 gpio。当前 `kthread_stop` 在 `oplus_chg_deinit` 之前——正确。

- [ ] **Step 1: 读每个 parser 函数的 con_id 确认正确**
- [ ] **Step 2: 交叉验证 — 审核代理对比每个 parser 的 con_id 与 DTS 属性**

---

### Task 3: DTS — pinctrl 状态对齐

**Files:**
- Modify: `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-mtp.dts`

**4.19 pinctrl 状态 (oplus_battery_msm8250.c pinctrl 初始化函数):**

| OPLUS GPIO | 4.19 pinctrl states | DTS 中是否存在? |
|------------|-------------------|----------------|
| ccdetect | `ccdetect_active`, `ccdetect_sleep` | ✅ pinctrl-6/7 (lines 1713-1714) |
| usbtemp gpio1 | `gpio1_adc_default` | ✅ pinctrl-10 (line 1717) |
| usbtemp gpio8 | `gpio8_adc_default` | ✅ pinctrl-11 (line 1718) |
| usbtemp gpio5 | `gpio5_adc_default` | ✅ pinctrl-12 (line 1719) |
| shipmode-id | `shipmode_id_active` | ❌ 不存在。只有 `ship_active`/`ship_sleep` (pinctrl-4/5) |
| wired-conn | `wired_con_int_active`, `wired_con_int_sleep` | ❌ 不存在 |
| otg-en | `otg_en_active`, `otg_en_sleep`, `otg_en_default` | ✅ pinctrl-4/5/6 (但需要确认是在 pdphy 节点还是 charger 节点) |
| idt-en | `idt_en_active`, `idt_en_sleep`, `idt_en_default` | ✅ 在 wlchg_rx 节点 |
| wrx-en | `wrx_en_active`, `wrx_en_sleep`, `wrx_en_default` | ✅ 在 oneplus_wlchg 节点 |
| wrx-otg | `wrx_otg_active`, `wrx_otg_sleep` | ✅ 在 oneplus_wlchg 节点 |

**关键问题**: pinctrl states 是绑定在特定设备节点上的。`devm_pinctrl_get(dev)` 返回的是 `dev->of_node` 对应的 pinctrl。所以：
- `pm8150b_charger` 节点的 pinctrl → 只能引用该节点 pinctrl-names 中声明的状态
- `oneplus_wlchg` 节点的 pinctrl → 只有 wrx_en/wrx_otg/usbin_int/dcdc_en 状态

**解决方案**: `pm8150b_charger` 节点需要为缺少的 GPIO 添加 pinctrl 状态：
- shipmode-id: 添加 `shipmode_id_active` 状态（或用 `ship_active` 替代？）
- wired-conn: 不存在于 DTS，保持 C 代码中 -ENOENT 忽略
- otg-en/idt-en/wrx-en/wrx-otg: 这些 pinctrl 分散在不同节点。**简单方案**: C 代码中对分散在其它节点的 GPIO 不做 pinctrl 查找（只做 GPIO descriptor 获取）

- [ ] **Step 1: 读 pm8150b_charger 节点的完整 pinctrl-names 和 pinctrl-* 列表**

- [ ] **Step 2: 对 shipmode-id 要么添加 `shipmode_id_active` pinctrl 状态，要么在 C 代码中将 pinctrl 查找标记为可选**

```bash
grep -A30 "pinctrl-names" E:\5.10-main\5.10-main\arch\arm64\boot\dts\qcom\sm8250-oneplus8t-mtp.dts | head -40
```

- [ ] **Step 3: 对跨节点的 GPIO (otg-en/idt-en/wrx-en/wrx-otg)，在 C 代码中将 pinctrl 查找改为可选 (IS_ERR_OR_NULL → dev_info 跳过)**

- [ ] **Step 4: 交叉验证**

---

### Task 4: C 代码 — pinctrl 查找容错处理

**Files:**
- Modify: `drivers/power/supply/qcom/oplus_battery_kona.c`

**修改**: 在 `oplus_kona_parse_otg_en_dt()`, `oplus_kona_parse_idt_en_dt()`, `oplus_kona_parse_wrx_en_dt()`, `oplus_kona_parse_wrx_otg_dt()` 中，将 pinctrl 查找失败从 `return PTR_ERR(...)` 改为 `dev_info(dev, "... not found, skipping pinctrl\n")` 然后 `return 0`。

**修改**: `oplus_kona_parse_shipmode_dt()` 中如果 `shipmode_id_active` pinctrl state 查找失败，同样改为跳过而非失败。

这样做是因为：
- DTS 中这些 GPIO 资源的 pinctrl 配置分散在多个不同设备节点 (oneplus_wlchg, wlchg_rx, pdphy)
- 5.10 中 `devm_pinctrl_get(dev)` 只能获取 charger 节点自己的 pinctrl
- 让 C 代码容错处理——GPIO descriptor 能获取到就行，pinctrl 让对应的原有节点自行管理

- [ ] **Step 1: 修改 otg/idt/wrx 三个 parser 的 pinctrl 错误处理**
- [ ] **Step 2: 修改 shipmode-id parser 的 pinctrl 错误处理**
- [ ] **Step 3: 交叉验证 — 审核代理确认改动后 probe 不会因为 pinctrl 缺失而失败**

---

### Task 5: 最终验证 — 交叉检查完整 DT-to-C 绑定

**验证清单**:

对于每个 GPIO，确认以下三者一致：
1. DTS 中的 `{con_id}-gpios` 属性 ✓
2. C 代码中 `devm_gpiod_get(dev, "{con_id}", ...)` ✓
3. 4.19 中 `of_get_named_gpio(node, "qcom,{name}-gpio", 0)` 引脚编号 ✓

| GPIO | 4.19 DT 属性 | 4.19 引脚 | 5.10 DT 属性 | 5.10 C con_id | 状态 |
|------|-------------|----------|-------------|-------------|------|
| ccdetect | `qcom,ccdetect-gpio` | tlmm 14 | `ccdetect-gpios` | `"ccdetect"` | 待添加 DT 属性 |
| shipmode-id | `qcom,shipmode-id-gpio` | (不在 DTS) | `shipmode-id-gpios` | `"shipmode-id"` | 待添加 DT 属性 |
| wired-conn | `qcom,wired_conn-gpio` | (不在 DTS) | (可选) | `"wired-conn"` | C 代码已容错 |
| otg-en | `qcom,otg_en-gpio` | pm8150l 4 | `otg-en-gpios` | `"otg-en"` | 待添加 DT 属性 |
| idt-en | `qcom,idt_en-gpio` | pm8150 10 | `idt-en-gpios` | `"idt-en"` | 待添加 DT 属性 |
| wrx-en | `qcom,wrx_en-gpio` | pm8150l 8 | `wrx-en-gpios` | `"wrx-en"` | 待添加 DT 属性 |
| wrx-otg | `qcom,wrx_otg-gpio` | pm8150l 11 | `wrx-otg-gpios` | `"wrx-otg"` | 待添加 DT 属性 |

- [ ] **Step 1: 逐行交叉验证每个 GPIO 的三点一致性**
- [ ] **Step 2: 跑 dtc 编译验证 DT 修改语法正确**
- [ ] **Step 3: 确认所有改动通过**

---

### 实施顺序
Task 1 (DTS 属性) → Task 3 (DTS pinctrl) → Task 4 (C 容错) → Task 2 (C 验证) → Task 5 (终审)
