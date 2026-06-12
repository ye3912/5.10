# SM8250 OnePlus 8T fastboot 秒回退修复计划

状态：仅写入计划文件，未修改内核源码。

目标设备：OnePlus 8T / kebab / OPLUS dtsi `19805 20809`。

目标树：`/root/ye3912-5.10/`。

源树参考：`/root/oneplus-sm8250-kernel/`。

## 1. 当前结论

8T 当前“秒进 fastboot”的第一嫌疑不是 charging/display/touch 驱动，而是 bootloader 阶段 DTB 匹配信息不一致。

4.19 源树中 8T 使用 overlay：

`/root/oneplus-sm8250-kernel/arch/arm64/boot/dts/vendor/oplus/kona-kebab-overlay.dts`

关键内容：

```dts
/ {
	model = "Qualcomm Technologies, Inc. kona MTP 19805 20809";
	compatible = "qcom,kona-mtp", "qcom,kona", "qcom,mtp";
	qcom,board-id = <0x10008 0>;
	oplus,dtsi_no = <19805 20809>;
	oplus,pcb_range = <0 56>;
};
```

5.10 目标树当前 8T 使用 monolithic DTB：

`/root/ye3912-5.10/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts`

当前末尾 root node：

```dts
/ {
	model = "OnePlus 8T";
	compatible = "oneplus,kebab", "qcom,kona", "qcom,sm8250";
	chassis-type = "handset";

	qcom,board-id = <0x00 0x00>;
	qcom,msm-id = <0x164 0x20001>;
};
```

差异点：

- `qcom,board-id`：4.19 是 `<0x10008 0>`，5.10 是 `<0x00 0x00>`。
- `compatible`：4.19 是 `qcom,kona-mtp/qcom,kona/qcom,mtp`，5.10 是 `oneplus,kebab/qcom,kona/qcom,sm8250`。
- `oplus,dtsi_no` / `oplus,pcb_range`：4.19 有，5.10 末尾 root node 缺失。
- `qcom,msm-id`：5.10 只覆盖 `356 0x20001`；4.19 overlay base 覆盖 `kona.dtb`、`kona-v2.dtb`、`kona-v2.1.dtb` 三个 base，对应 `356 0x10000`、`356 0x20000`、`356 0x20001`。

## 2. 不建议先动的内容

这轮不建议先改：

- charging / power：还没进入 kernel 驱动阶段时不相关。
- display / DPU / KGSL：如果是 bootloader 秒回 fastboot，通常还没跑到显示驱动 probe。
- touch / fingerprint / camera：同上。
- `Makefile`：当前已生成 `sm8250-oneplus8t.dtb`。
- `sm8250-oneplus8t_defconfig`：当前已启用 `CONFIG_PSTORE=y`、`CONFIG_PSTORE_CONSOLE=y`、`CONFIG_PSTORE_PMSG=y`、`CONFIG_PSTORE_RAM=y`。

## 3. 建议修改范围

只改一个文件：

```text
/root/ye3912-5.10/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts
```

不改 8 Pro。

不改 8T defconfig。

不改 Makefile。

不改驱动源码。

## 4. 推荐补丁方案：strict bootloader-match

将 5.10 8T DTS 末尾 root node 从当前内容改为更贴近 4.19 kebab overlay 的匹配信息。

建议替换：

```dts
/ {
	model = "OnePlus 8T";
	compatible = "oneplus,kebab", "qcom,kona", "qcom,sm8250";
	chassis-type = "handset";

	qcom,board-id = <0x00 0x00>;
	qcom,msm-id = <0x164 0x20001>;
};
```

为：

```dts
/ {
	model = "Qualcomm Technologies, Inc. kona MTP 19805 20809";
	compatible = "qcom,kona-mtp", "qcom,kona", "qcom,mtp";
	chassis-type = "handset";

	qcom,board-id = <0x10008 0>;
	qcom,msm-id = <0x164 0x10000>, <0x164 0x20000>, <0x164 0x20001>;
	oplus,dtsi_no = <19805 20809>;
	oplus,pcb_range = <0 56>;
};
```

理由：

- `qcom,board-id = <0x10008 0>` 与 4.19 8T overlay 完全一致。
- `qcom,msm-id` 覆盖 4.19 的 `kona` / `kona-v2` / `kona-v2.1` 三套 base。
- `oplus,dtsi_no` 和 `oplus,pcb_range` 恢复 OPLUS 项目标识范围。
- 这类秒回 fastboot 优先排查 bootloader DTB 选择/校验，不应先扩大到驱动移植。

## 5. 保守兼容方案：保留 oneplus,kebab

如果担心 userspace、init 或脚本读取 root `compatible` 里的 `oneplus,kebab`，可采用兼容写法：

```dts
/ {
	model = "Qualcomm Technologies, Inc. kona MTP 19805 20809";
	compatible = "oneplus,kebab", "qcom,kona-mtp", "qcom,kona", "qcom,mtp", "qcom,sm8250";
	chassis-type = "handset";

	qcom,board-id = <0x10008 0>;
	qcom,msm-id = <0x164 0x10000>, <0x164 0x20000>, <0x164 0x20001>;
	oplus,dtsi_no = <19805 20809>;
	oplus,pcb_range = <0 56>;
};
```

优先级建议：

1. 若当前问题是 bootloader 直接回 fastboot：先用 strict bootloader-match。
2. 若 strict 能进 kernel 但 userspace 依赖 `oneplus,kebab` 出问题：再切到保守兼容方案。

## 6. 编译验证步骤

在 `/root/ye3912-5.10/` 下执行。

先生成配置：

```bash
make ARCH=arm64 O=out sm8250-oneplus8t_defconfig
```

只编译 8T DTB：

```bash
make ARCH=arm64 O=out qcom/sm8250-oneplus8t.dtb
```

如果上面目标名不被当前 Kbuild 接受，改用：

```bash
make ARCH=arm64 O=out dtbs
```

验证产物存在：

```bash
test -f out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb
```

反编译检查关键字段：

```bash
dtc -I dtb -O dts \
  out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb \
  > /tmp/sm8250-oneplus8t.dts
```

检查：

```bash
grep -n "qcom,board-id\|qcom,msm-id\|oplus,dtsi_no\|oplus,pcb_range\|compatible" \
  /tmp/sm8250-oneplus8t.dts
```

预期至少看到：

```text
qcom,board-id = <0x10008 0x00>;
qcom,msm-id = <0x164 0x10000 0x164 0x20000 0x164 0x20001>;
oplus,dtsi_no = <0x4d5d 0x5149>;   // 十进制 19805 / 20809
oplus,pcb_range = <0x00 0x38>;     // 十进制 0 / 56
```

## 7. 打包验证重点

如果 DTB 编译通过但仍秒回 fastboot，下一步不是改驱动，而是核对打包链：

1. 确认刷入的 boot image 里确实包含新的 `sm8250-oneplus8t.dtb`。
2. 确认没有仍然使用旧的 8 Pro DTB 或 `sm8250-mtp.dtb`。
3. 如果当前 ROM/bootloader 依赖单独 `dtbo.img`，需要恢复 4.19 风格的 `kona-kebab-overlay.dtbo` 生成与打包，而不是只依赖 monolithic DTB。
4. 若用 AnyKernel3，检查 `Image.gz-dtb` 拼接顺序，8T DTB 必须在 bootloader 可匹配的位置。
5. 若使用 mkbootimg v4/GKI boot image，确认 DTB 放置位置符合设备当前 bootloader 预期。

## 8. 启动后取证

如果修复后不再秒回 fastboot，但 kernel panic 或卡 logo：

优先拉 pstore：

```bash
adb shell ls -l /sys/fs/pstore
adb pull /sys/fs/pstore ./pstore-8t
```

若 adb 不可用，进 recovery 后尝试挂载 pstore：

```bash
adb shell ls -l /sys/fs/pstore
```

8T 当前 DTS 已有：

```dts
ramoops@B0000000 {
	compatible = "ramoops";
	reg = <0x0 0xB0000000 0x0 0x00400000>;
	record-size = <0x40000>;
	console-size = <0x40000>;
	ftrace-size = <0x40000>;
	pmsg-size = <0x200000>;
};
```

8T defconfig 已有：

```text
CONFIG_PSTORE=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_PSTORE_PMSG=y
CONFIG_PSTORE_RAM=y
```

所以修复 DTB 匹配后，下一阶段应能拿到 ramoops/pstore 证据。

## 9. 回滚方案

若 strict bootloader-match 方案导致行为变差，直接恢复末尾 root node 为原状态：

```dts
/ {
	model = "OnePlus 8T";
	compatible = "oneplus,kebab", "qcom,kona", "qcom,sm8250";
	chassis-type = "handset";

	qcom,board-id = <0x00 0x00>;
	qcom,msm-id = <0x164 0x20001>;
};
```

## 10. 后续判断树

执行 DTS 匹配修复后：

- 仍秒回 fastboot：继续查 boot image / dtbo image / dtbTool / mkbootimg 打包链。
- 不回 fastboot但黑屏：查 earlycon/pstore，优先 UFS、initramfs、vendor_boot、fstab、dm-verity。
- 卡 logo 或进 recovery：查 pstore panic，再进入具体子系统。
- 能进 adb shell：再开始 charging、display、touch、OPLUS system 子系统移植。
