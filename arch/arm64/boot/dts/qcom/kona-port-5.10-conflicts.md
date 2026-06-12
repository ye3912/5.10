# kona-port.dtsi → sm8250.dtsi (5.10) 节点冲突扫描报告

扫描日期：2026-05-23

## sm8250.dtsi 结构
- `soc: soc@0 { ... }` (line 406)，所有外设为其子节点
- kona-port.dtsi 通过 `&soc { ... }` (line 29) 添加子节点
- `&soc` 引用模式在 5.10 是正确做法

---

## 🔴 等级1：直接冲突（相同地址，不同节点名导致 DTC 不合并）

| 节点 | 地址 | sm8250.dtsi | kona-port.dtsi |
|------|------|-------------|----------------|
| GCC | @100000 | `gcc: clock-controller@100000` qcom,gcc-sm8250 (✅ 已修复) | ~~`clock_gcc: qcom,gcc-kona@100000`~~ (已删除) |
| GPUCC | @3d90000 | `gpucc: clock-controller@3d90000` qcom,sm8250-gpucc | `clock_gpucc: qcom,gpucc@3d90000` qcom,gpucc-kona |
| CAMCC | @ad00000 | `camcc: clock-controller@ad00000` qcom,camcc-sm8250 | `clock_camcc: qcom,camcc@ad00000` qcom,camcc-kona |
| DISPCC | @af00000 | `dispcc: clock-controller@af00000` qcom,sm8250-dispcc | `clock_dispcc: qcom,dispcc@af00000` qcom,kona-dispcc |
| AOSS | @c300000 | `aoss_qmp: qmp@c300000` qcom,sm8250-aoss-qmp | `qmp_aop: qcom,qmp-aop@c300000` qcom,qmp-mbox |
| IPCC | @408000 | `ipcc: mailbox@408000` qcom,sm8250-ipcc | `ipcc_mproc: qcom,ipcc@408000` qcom,ipcc |
| WDT | @17c10000 | `watchdog@17c10000` qcom,apss-wdt-sm8250 | `wdog: qcom,wdt@17c10000` qcom,msm-watchdog |
| SLPI | @5c00000 | `slpi: remoteproc@5c00000` qcom,sm8250-slpi-pas | `qcom,ssc@5c00000` qcom,pil-tz-generic |
| Timer | @17c20000 | `timer@17c20000` arm,armv7-timer-mem | `memtimer: timer@17c20000` arm,armv7-timer-mem |

**Timer 特殊**：节点名相同 (`timer@17c20000`)，DTC 会尝试合并，但 child frame@ 节点可能有重复/冲突。

## 🟡 等级2：地址不同（可能 4.19/5.10 寄存器布局差异）

| 节点 | sm8250.dtsi 地址 | kona-port.dtsi 地址 |
|------|-----------------|---------------------|
| NPUCC | @9910000 | @9980000 |
| VIDEOCC | @ab00000 | @abf0000 |

## 🟡 等级3：5.10 规范不符合

1. **`arch_timer: timer` 放在 `&soc` 内** — 5.10 版本 timer 应为顶层节点（非 soc 子节点），且 PPI 12 vs PPI 10 差异
2. **`clock_rpmh: qcom,rpmhclk`** — 与 sm8250.dtsi 的 `rpmhcc: clock-controller` 功能重叠
3. **`clocks { }` 空节点 (line 1835)** — 4.19 残留，应删除
4. **`qcom,msm-watchdog`** — 4.19 旧驱动 compatible 字符串
5. **`disp_rsc: rsc@af20000`** — 5.10 版的 apps_rsc 已移至 @18200000

## 🟢 无冲突（确认通过）

- qfprom, gpi_dma0/gpi_dma1, tspp, wlan
- ipa_fm, ipa_virt, ipa_hw
- ufs_* (ufsphy_mem, ufsice, ufshc)
- sdhc_2, dcc
- GDSC 节点组 (已检查 reg 不冲突)

## 需要逐一扫描的其他文件

23 个 kona-*.dtsi 文件都使用 `&soc` 添加节点，需逐一检查是否有类似冲突：
- kona-pmic.dtsi, kona-pinctrl.dtsi, kona-audio.dtsi
- kona-camera*.dtsi, kona-connectivity.dtsi, kona-display.dtsi
- kona-regulators*.dtsi, kona-thermal*.dtsi 等

## 建议修复方案

### 方案 A（推荐）：标签对齐 + 节点删除

删除 kona-port.dtsi 中与 sm8250.dtsi 冲突的节点定义，所有引用改用上游标签：

| 删除的旧标签 | → 使用上游标签 |
|-------------|---------------|
| `&clock_gpucc` | `&gpucc` |
| `&clock_camcc` | `&camcc` |
| `&clock_dispcc` | `&dispcc` |
| `&clock_videocc` | `&videocc` |
| `&clock_npucc` | `&npucc` |
| `&qmp_aop` | `&aoss_qmp` |
| `&ipcc_mproc` | `&ipcc` |
| `&wdog` | `&watchdog` |
| `&qcom,ssc` | `&slpi` |

处理方式与 clock_gcc → gcc 一致：先用 sed 全局替换引用，再删除节点定义，最后 DTC 编译验证。
