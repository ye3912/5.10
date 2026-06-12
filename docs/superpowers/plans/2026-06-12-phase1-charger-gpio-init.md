# Phase 1: 8T 充电底层初始化 实施计划——多轮交叉验证

> **For agentic workers:** Each task below is independently verifiable. Verify BEFORE marking complete.

**Goal:** 让 8T 的 SMB5 charger 在 5.10 内核中完成 GPIO/Pinctrl/IRQ 初始化，插入充电器后 dmesg 可识别充电类型（USB/PD/QC）。

**Architecture:** 5.10 已有 Kona SMB5 adapter（oplus_battery_kona.c，9 个 P0 回调）。本次在 adapter 上扩展：新增 GPIO/pinctrl 初始化 → 充电器插拔检测 → USB 温度监控线程。所有函数签名、DT 解析、实现均从 4.19 oplus_battery_msm8250.c 移植，适配 5.10 的 SMB5 API。

**Tech Stack:** devm_gpiod_get, pinctrl, power_supply, SMB5 regmap API

---

## 验证流程（每任务两轮交叉验证）

```
[实现代理] --写代码--> [审核代理] --读代码+对比4.19--> [报告分歧]
     ↑                                                        |
     └──────────── 修复分歧后重新验证 ←────────────────────────┘
```

---

### Task 1: GPIO 数据结构扩展

**4.19 基线**: smb_charger struct 在 oplus_battery_msm8250.h 定义了 ~20 个 GPIO 字段（ccdetect_gpio, usbtemp N 个, shipmode, wired_conn, wireless_conn, otg_en, idt_en, wrx_en 等）加对应的 pinctrl 字段。

**5.10 现状**: oplus_battery_kona.c 的 oplus_kona_chg struct 只有 dev/chg/oplus_chip 三个字段。无 GPIO 字段。

**实现**:
- 在 oplus_battery_kona.h 新增 `struct oplus_kona_gpio` 聚合结构体
- 包含：ccdetect_gpio/irq, usbtemp_gpio1/5/8, shipmode_gpio, wired_conn_gpio/irq, otg_en_gpio
- 每个 gpio 配一个 pinctrl + active/sleep/default 状态
- 在 oplus_kona_chg 中添加指向此结构体的指针

- [ ] **Step 1: 读 4.19 smb_charger 中 GPIO 字段定义**
- [ ] **Step 2: 读 5.10 oplus_battery_kona.h + .c 现有结构**
- [ ] **Step 3: 写 oplus_kona_gpio 结构体 + 字段扩展**
- [ ] **Step 4: 交叉验证 — 对比两步确认字段全覆盖**

---

### Task 2: GPIO DT 解析函数

从 4.19 oplus_battery_msm8250.c 移植以下独立解析函数（每个函数解析一类 GPIO）：

1. **ccdetect GPIO 解析**: 读 DT `oplus,ccdetect-gpio` → devm_gpiod_get
2. **usbtemp GPIOs 解析**: 3 个 GPIO (usbtemp_gpio_1/5/8) + pinctrl states
3. **shipmode GPIO 解析**: `oplus,ship-gpio`
4. **wired_conn GPIO 解析**: `oplus,wired_conn_gpio`
5. **otg_en / idt_en / wrx_en GPIO 解析**: 各一个 gpio

**关键 API 迁移**: 4.19 用 `of_get_named_gpio()` (返回 int)。5.10 必须用 `devm_gpiod_get()` (返回 gpio_desc *)。

- [ ] **Step 1: 提取 4.19 中每个 GPIO 的 DT property name 和解析逻辑**
- [ ] **Step 2: 写 5.10 版本的 DT 解析函数（一个函数一个 gpio 类）**
- [ ] **Step 3: 交叉验证 — 审核代理对比 4.19 vs 5.10 GPIO property name 一致性**

---

### Task 3: OPLUS GPIO 管理函数（get/set 封装）

从 4.19 移植以下封装函数（被 ops 表和充电状态机调用）：

1. `oplus_get_wired_chg_present()` — GPIO 读取
2. `oplus_get_otg_switch_status()` / `oplus_set_otg_switch_status()` — OTG
3. `oplus_set_idt_en_val()` / `oplus_get_idt_en_val()` — 无线充电使能
4. `smbchg_get_chargerid_switch_val()` / `smbchg_set_chargerid_switch_val()` — 充电器 ID 检测
5. `smbchg_get_chargerid_volt()` — iio 读 charger ID 电压
6. `oplus_get_usbtemp_volt()` — iio 读 USB 温度

**关键 API 迁移**: 旧 `gpio_get_value(int)` → 新 `gpiod_get_value(struct gpio_desc *)`

- [ ] **Step 1: 读 4.19 的每个 get/set 函数实现**
- [ ] **Step 2: 写 5.10 版本，替换 gpio→gpiod 和 iio 调用**
- [ ] **Step 3: 交叉验证** 

---

### Task 4: USB 温度监控线程移植

从 4.19 移植 USB 温度监控子系统（usbtemp thread）：

1. `oplus_usbtemp_thread_init()` — kthread_run 启动监控线程
2. `oplus_wake_up_usbtemp_thread()` — 唤醒线程
3. `oplus_usbtemp_monitor_common()` — 主监控循环
4. `oplus_usbtemp_condition()` — 条件检查
5. `oplus_usbtemp_dischg_action()` / `oplus_usbtemp_clear_dischg()` — 保护动作

**关键 API 迁移**: `wake_lock` → `pm_wakeup_event`（已在 sm8250-kernel-porting skill API 速查表中列出）

- [ ] **Step 1: 读 4.19 USB 温度监控的完整实现**
- [ ] **Step 2: 写 5.10 版本，注意 wakelock→pm_wakeup 迁移**
- [ ] **Step 3: 交叉验证 — 审核代理确认所有 wake_lock 已替换**

---

### Task 5: 充电器 ops 表扩展

将新移植的 get/set 函数注册到 kona_smb5_chg_ops 表中（当前 11/46 字段 → 目标 25+/46）。新增字段：

- `get_usbtemp_volt` → oplus_get_usbtemp_volt
- `set_typec_sinkonly` → oplus_set_typec_sinkonly
- `get_chargerid_volt` → smbchg_get_chargerid_volt
- `set_chargerid_switch_val` → smbchg_set_chargerid_switch_val
- `get_chargerid_switch_val` → smbchg_get_chargerid_switch_val
- `get_boot_mode` → get_boot_mode
- `get_boot_reason` → smbchg_get_boot_reason
- `get_rtc_soc` → oplus_chg_get_shutdown_soc
- `set_rtc_soc` → oplus_chg_backup_soc
- `oplus_usbtemp_monitor_condition` → oplus_usbtemp_condition

- [ ] **Step 1: 读 4.19 smb5_chg_ops 中每个字段对应的函数**
- [ ] **Step 2: 在 5.10 kona_smb5_chg_ops 添加已验证可用的字段**
- [ ] **Step 3: 交叉验证 — 审核代理确认每个 ops 字段有对应的实现函数**

---

### Task 6: oplus_battery_kona_init() 扩展

在 5.10 的 init 函数中添加 GPIO/DT 解析调用链：

```
oplus_battery_kona_init()
  ├── oplus_kona_gpio_parse_dt(dev, kona)    // 所有 GPIO DT 解析
  ├── oplus_kona_gpio_init(kona)              // GPIO 方向设置
  ├── oplus_kona_ccdetect_init(kona)          // CC detect IRQ 注册
  ├── oplus_kona_usbtemp_init(dev)            // USB 温度线程启动
  ├── (现有) oplus_chg_chip 初始化
  └── (现有) oplus_chg_init()
```

- [ ] **Step 1: 读 4.19 smb5_probe() 中的 GPIO 初始化调用链**
- [ ] **Step 2: 在 5.10 oplus_battery_kona_init() 中添加新调用**
- [ ] **Step 3: 交叉验证 — 审核代理对比 4.19 probe vs 5.10 init 函数调用顺序**

---

### Task 7: KMI 合规检查

所有新导出符号必须使用 EXPORT_SYMBOL_GPL。新增的 GPIO 函数如被其他模块调用，需要加入 abi_gki_aarch64_oplus KMI 符号列表。

- [ ] **Step 1: 扫描所有新增的 EXPORT_SYMBOL**
- [ ] **Step 2: 确认使用 EXPORT_SYMBOL_GPL**

---

## 实施顺序

Tasks 1-2 → Tasks 3-4（可并列）→ Task 5 → Task 6 → Task 7
