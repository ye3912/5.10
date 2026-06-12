# Phase 0: DTS 冲突解决 — 完成报告

日期: 2026-06-12

## 验证结果

### Level 2 (地址差异)
- **NPUCC**: 4.19 @9980000, 5.10 @9910000 → 上游地址为准 ✅。kona-port.dtsi 引用 `&npucc` 已对齐上游。
- **VIDEOCC**: 4.19 @abf0000, 5.10 @ab00000 → 上游地址为准 ✅。kona-port.dtsi 引用 `&videocc` 已对齐上游。
- 两者在 kona-port.dtsi 中均无独立节点定义，仅通过标签引用上游节点。

### Level 3 (规范违反)
- **arch_timer**: 无在 &soc 内的定义 → ✅ 已修复
- **clock_rpmh**: 无 rpmhclk 节点定义，仅引用头文件 → ✅ 已清理
- **empty clocks{}**: 无空时钟节点 → ✅ 已删除
- **watchdog**: kona-port.dtsi 中仅在 interrupt-names 文本中使用 "wdog" 字符串（非标签引用），无 wdog 节点定义或标签引用 → ✅ 已安全
- **disp_rsc**: 无旧 rsc@af20000 定义 → ✅ 已删除

### kona-*.dtsi 标签扫描
- 全部 23 个 kona-*.dtsi 文件中未发现旧标签引用（&clock_gpucc、&qmp_aop 等）
- 所有引用已使用 5.10 上游标签（&gpucc、&npucc、&aoss_qmp 等）

### 编译配置验证
- DTB: `sm8250-oneplus8t.dtb` → Makefile 已注册 ✅
- DTBO: `kona-kebab.dtbo` → oplus/Makefile 已注册，dtbo-base 指向正确 DTB ✅
- Bootloader 匹配字段: sm8250-oneplus8t.dts 包含完整的 board-id/msm-id/dtsi_no/pcb_range ✅

## 未完成项

Linux workspace 磁盘空间不足，无法运行 dtc 编译语法检查。需在环境恢复后验证：
```bash
dtc -I dts -O dtb -o /dev/null \
  -i arch/arm64/boot/dts/qcom \
  -i include \
  arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts
```

## 下一步

Phase 1: 8T 充电底层初始化 — GPIO/DTS/充电器 ops 表移植。
