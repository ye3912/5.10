# SM8250 OnePlus 8T 900E Crash Fix Plan

> **目标**：修复 OnePlus 8T 过 bootloader 后约 1 秒黑屏、约 3 秒后进入 Qualcomm 900E 端口的早期启动崩溃。
>
> **当前结论**：最高优先级不是显示栈，也不是 OPLUS charger，而是 5.10 DTS 中 upstream/downstream reserved-memory 双栈混用导致 PIL carveout 布局不符合 factory DTB。

---

## 0. 三轮交叉验证结果

### Round 1：`dtc` 语法实测

使用当前 Windows 环境的 `dtc.exe`：

```text
Version: DTC 1.6.1
```

实测结果：

| 验证项 | 结果 | 结论 |
|--------|------|------|
| `/delete-node/ memory@1000;` | 编译通过，目标节点被删除 | 按当前子节点名删除可用 |
| `/delete-node/ &old_by_label;` | 编译通过，目标 label 节点被删除 | 按 label 删除可用 |
| 删除旧 label 后复用 label | 编译通过 | 可删除旧节点后重建同名 label |
| 单节点多个 label：`cdsp_mem: pil_cdsp_mem:` | 编译通过，两个 consumer 指向同一 phandle | 可用一个物理 reserved-memory 节点同时满足 upstream/downstream 引用 |

**结论**：原方案中“按子节点名删除可能不可用”的担心被实测推翻；但内核树已有 DTS 先例使用 `/delete-node/ &label`，因此最终方案采用 **label 删除 + 单节点多 label 重建**，更可读、更接近上游风格。

### Round 2：label 引用链验证

全树 grep 结果显示，不能只保留 `pil_*_mem`，也不能只保留 upstream `*_mem`。

必须保留的 downstream label：

| Label | 引用位置 | 用途 |
|-------|----------|------|
| `pil_adsp_mem` | `arch/arm64/boot/dts/qcom/kona-port.dtsi:2962` | 下游 ADSP PIL |
| `pil_cdsp_mem` | `arch/arm64/boot/dts/qcom/kona-port.dtsi:3005` | 下游 CDSP/Turing PIL |
| `pil_spss_mem` | `arch/arm64/boot/dts/qcom/kona-port.dtsi:2562`、`:3096` | SPSS utils / SPSS PIL |

必须保留的 upstream label：

| Label | 引用位置 | 用途 |
|-------|----------|------|
| `cdsp_mem` | `arch/arm64/boot/dts/qcom/kona-cvp.dtsi:93`、`arch/arm64/boot/dts/qcom/sm8250.dtsi:1490` | CVP / upstream CDSP remoteproc |
| `adsp_mem` | `arch/arm64/boot/dts/qcom/kona-port.dtsi:3144`、`arch/arm64/boot/dts/qcom/sm8250.dtsi:2225` | CVPSS / upstream ADSP remoteproc |
| `cdsp_secure_heap` | `arch/arm64/boot/dts/qcom/kona-ion.dtsi:57` | secure heap |

**结论**：每个重定位 carveout 应该是一个节点挂两个 label，例如：

```dts
cdsp_mem: pil_cdsp_mem: memory@8f200000 { ... };
adsp_mem: pil_adsp_mem: memory@91b00000 { ... };
spss_mem: pil_spss_mem: memory@94000000 { ... };
```

这样不会产生两个同地址 reserved-memory 节点，也不会破坏任何 phandle consumer。

### Round 3：源码先例、硬编码地址、CI 路径验证

内核树已有同类先例：

- `arch/arm64/boot/dts/qcom/sdm845-xiaomi-beryllium.dts:16-24` 使用 `/delete-node/ &adsp_mem;`、`/delete-node/ &cdsp_mem;`、`/delete-node/ &spss_mem;`
- 同文件 `:53-108` 在本机 DTS 中重建 reserved-memory 节点

额外发现：

| 项 | 位置 | 结论 |
|----|------|------|
| `qcom,pil-addr = <0x8BE00000>` | `arch/arm64/boot/dts/qcom/kona-port.dtsi:2563` | 如果 SPSS 重定位到 `0x94000000`，这里必须同步 |
| DTBO 打包仍是单 entry | `.github/workflows/build.yml:180-182` | 仅打包 `kona-kebab.dtbo` 一项；可延后做多 entry |
| 8 Pro 也有同类 PIL overlap | `arch/arm64/boot/dts/qcom/sm8250-oneplus8pro.dts:104-125` | 本轮先修 8T，8 Pro 应单独按 factory DTB 验证后修 |

---

## 1. 当前事实结论

### 1.1 已排除项

| 排查项 | 结论 | 证据 |
|--------|------|------|
| OPLUS charger | 不是当前最高优先级 | IIO/GPIO 路径有空值检查，缺 `chgID_voltage_adc` 可安全降级 |
| DWC3 MSM C 代码 | 不是 `androidboot.usbcontroller` mismatch 的内核根因 | 内核 C 源码未解析 `androidboot.usbcontroller` |
| OPLUS logbuf | 不是直接 crash 点 | 节点缺失或 reg parse 失败时安全返回 |
| DTBO container 格式 | 正确 | factory 和 V7 都是 Android DT table，magic `0xD7B7AB1E` |

### 1.2 最高优先级根因

当前 8T DTS 同时继承 upstream `sm8250.dtsi` 和 downstream `kona-port.dtsi`/`sm8250-oneplus8t.dts` 的 reserved-memory 体系：

| 地址 | Upstream label | Downstream label | Factory DTB 应用地址 |
|------|----------------|------------------|----------------------|
| `0x87800000` | `cdsp_mem` | `pil_cdsp_mem` | `0x8f200000` |
| `0x8a100000` | `adsp_mem` | `pil_adsp_mem` | `0x91b00000` |
| `0x8be00000` | `spss_mem` | `pil_spss_mem` | `0x94000000` |
| `0x8bf00000` | `cdsp_secure_heap` | 无 | `0x94100000` |

这会导致：

- `of_reserved_mem.c` 检测到 overlap 并打印 `OVERLAP DETECTED`
- remoteproc / PIL consumer 可能拿到与 factory boot chain 不一致的 carveout
- Qualcomm 早期 remoteproc / TZ / ramdump 路径可能在数秒内触发 900E

---

## 2. 最终修改方案

### Phase 1：修复 8T reserved-memory 布局（必须先做）

#### 文件

- `arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts`
- `arch/arm64/boot/dts/qcom/kona-port.dtsi`

#### 原则

1. 删除 upstream 的旧 reserved-memory label。
2. 在 8T DTS 中按 factory 地址重建。
3. 每个重定位 PIL 节点只创建 **一个物理节点**。
4. 一个物理节点同时挂 upstream label 和 downstream label。
5. 不创建两个同地址节点，避免再次触发 overlap。

#### 具体 DTS 形态

在 `sm8250-oneplus8t.dts` 的 `#include` 之后、`/ { ... }` 主节点之前加入：

```dts
/*
 * OnePlus 8T factory DTB relocates PIL carveouts from upstream SM8250
 * addresses. Delete upstream reserved-memory nodes and rebuild them below
 * with both upstream and downstream labels on a single physical node.
 */
/delete-node/ &removed_mem;
/delete-node/ &cdsp_mem;
/delete-node/ &adsp_mem;
/delete-node/ &spss_mem;
/delete-node/ &cdsp_secure_heap;
```

然后在当前 `reserved_memory: reserved-memory` 中调整：

```dts
reserved_memory: reserved-memory {
	#address-cells = <2>;
	#size-cells = <2>;
	ranges;

	removed_mem: memory@80b00000 {
		compatible = "removed-dma-pool";
		no-map;
		reg = <0x0 0x80b00000 0x0 0xcd00000>;
	};

	/* existing non-overlapping nodes stay as-is */

	cdsp_mem: pil_cdsp_mem: memory@8f200000 {
		compatible = "removed-dma-pool";
		no-map;
		reg = <0x0 0x8f200000 0x0 0x1400000>;
	};

	adsp_mem: pil_adsp_mem: memory@91b00000 {
		compatible = "removed-dma-pool";
		no-map;
		reg = <0x0 0x91b00000 0x0 0x2500000>;
	};

	spss_mem: pil_spss_mem: memory@94000000 {
		compatible = "removed-dma-pool";
		no-map;
		reg = <0x0 0x94000000 0x0 0x100000>;
	};

	cdsp_secure_heap: memory@94100000 {
		no-map;
		reg = <0x0 0x94100000 0x0 0x4600000>;
	};

	kboot_uboot_logmem: memory@9fe00000 {
		compatible = "oplus,xbl_uefi_kbootlog";
		no-map;
		reg = <0x0 0x9fe00000 0x0 0x200000>;
	};
};
```

需要把当前旧节点替换掉：

- `pil_cdsp_mem: pil_cdsp_region@87800000`
- `pil_adsp_mem: pil_adsp_region@8a100000`
- `pil_spss_mem: pil_spss_region@8be00000`

#### 同步 hardcoded SPSS 地址

修改 `arch/arm64/boot/dts/qcom/kona-port.dtsi:2563`：

```diff
-		qcom,pil-addr = <0x8BE00000>; // backward compatible
+		qcom,pil-addr = <0x94000000>; // relocated to factory SPSS PIL address
```

#### Phase 1 验证

编译并反编译后必须满足：

```text
removed_mem          reg = 0x80b00000 size 0xcd00000
cdsp_mem/pil_cdsp   reg = 0x8f200000 size 0x1400000
adsp_mem/pil_adsp   reg = 0x91b00000 size 0x2500000
spss_mem/pil_spss   reg = 0x94000000 size 0x100000
cdsp_secure_heap    reg = 0x94100000 size 0x4600000
kboot_uboot_logmem  reg = 0x9fe00000 size 0x200000
```

且不能再出现同一 `reg` 地址对应两个 reserved-memory 节点。

---

### Phase 2：修复 8T defconfig cmdline（与 Phase 1 同批做）

#### 文件

- `arch/arm64/configs/sm8250-oneplus8t_defconfig`

#### 修改

```diff
-androidboot.usbcontroller=a800000.dwc3
+androidboot.usbcontroller=a600000.dwc3
```

同批补齐 factory 行为：

```text
reboot=panic_warm kpti=off
```

最终 `CONFIG_CMDLINE` 应保留已有调试项，并至少包含：

```text
console=ttyMSM0,115200n8
earlycon=qcom,geni-debug-uart,0xa90000
ignore_loglevel
log_buf_len=1M
video=vfb:640x400,bpp=32,memsize=3072000
msm_rtb.filter=0x237
service_locator.enable=1
androidboot.usbcontroller=a600000.dwc3
swiotlb=2048
loop.max_part=7
cgroup.memory=nokmem,nosocket
reboot=panic_warm
kpti=off
```

说明：`androidboot.usbcontroller` 不是内核 DWC3 probe 的直接输入，但 Android userspace 使用它；修正它不应单独解释 900E，但应和 Phase 1 一起修。

---

### Phase 3：DTBO 多 entry（延后，除非 ABL overlay 匹配仍失败）

当前 `.github/workflows/build.yml:180-182` 只打包：

```bash
python3 scripts/mkdtboimg.py create \
  "$AK3/dtbo.img" \
  arch/arm64/boot/dts/oplus/kona-kebab.dtbo
```

factory DTBO 有 4 个 entry：

| entry | `oplus,dtsi_no` |
|-------|-----------------|
| 0 | `<19821 19855>` |
| 1 | `<19811>` |
| 2 | `<20828>` |
| 3 | `<19805 20809>` |

如果 Phase 1+2 后仍怀疑 overlay 匹配，应新增多个 overlay DTS：

- `kona-kebab-19805-20809.dts`
- `kona-kebab-19811.dts`
- `kona-kebab-19821-19855.dts`
- `kona-kebab-20828.dts`

然后在 CI/打包脚本中一次传入四个 `.dtbo`。

本轮不优先做，因为当前 V7 已覆盖 `19805 20809`，且本次 900E 更符合 reserved-memory / PIL 崩溃。

---

### Phase 4：源码 checkpoint 插桩（仅 Phase 1+2 后仍 900E 时做）

插桩只用于定位，不进入最终版本。

建议插桩点：

| 文件 | 函数 |
|------|------|
| `init/main.c` | `start_kernel()` |
| `init/main.c` | `kernel_init_freeable()` |
| `drivers/of/platform.c` | `of_platform_default_populate_init()` |
| `drivers/spmi/spmi-pmic-arb.c` | `spmi_pmic_arb_probe()` |
| `drivers/scsi/ufs/ufs-qcom.c` | `ufs_qcom_probe()` |
| `drivers/usb/dwc3/dwc3-msm-core.c` | `dwc3_msm_probe()` |
| `drivers/power/supply/qcom/qpnp-smb5.c` | `qpnp_smb5_probe()` |
| `drivers/power/supply/qcom/oplus_battery_kona.c` | `oplus_battery_kona_init()` |

统一格式：

```c
pr_emerg("KEBAB900E: <function-name>\n");
```

---

## 3. 推荐执行顺序

### 推荐顺序

```text
Phase 1 + Phase 2
  ↓
编译 DTB/Image.gz/dtbo
  ↓
反编译 DTB 验证 reserved-memory 地址和 label
  ↓
打包 boot/dtb/dtbo
  ↓
真机测试
  ↓
若仍 900E：Phase 4 插桩
  ↓
若 overlay 匹配仍有疑问：Phase 3 多 entry DTBO
```

### 不推荐顺序

不要先做 display/audio/sensor 大面积禁用，也不要先改 OPLUS charger。现有证据显示它们不是最高优先级根因。

---

## 4. 验证命令

### 编译

```bash
make ARCH=arm64 O=out sm8250-oneplus8t_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out -j$(nproc) Image.gz
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=out -j$(nproc) dtbs -k
```

### 反编译核对

```bash
dtc -I dtb -O dts out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb > /tmp/verify-8t.dts

grep -n "removed_mem\|cdsp_mem\|pil_cdsp_mem\|adsp_mem\|pil_adsp_mem\|spss_mem\|pil_spss_mem\|cdsp_secure_heap\|xbl_uefi_kbootlog" /tmp/verify-8t.dts
```

### DTB/DTBO 打包

```bash
python3 scripts/mkdtboimg.py create dtb.img out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb
python3 scripts/mkdtboimg.py create dtbo.img out/arch/arm64/boot/dts/oplus/kona-kebab.dtbo
```

---

## 5. 最终审核结论

| 项 | 结论 |
|----|------|
| 根因方向 | 正确：reserved-memory / PIL carveout 是 P0 |
| 原 `/delete-node/` 策略 | 实测可用，但最终采用 label delete 更符合内核 DTS 先例 |
| 最干净修法 | 单节点多 label，避免 duplicate reserved-memory |
| 必须同步项 | `kona-port.dtsi:2563 qcom,pil-addr` |
| 可延后项 | DTBO 4-entry、源码插桩 |
| 不建议先动 | OPLUS charger、display/audio/sensor 大面积禁用 |

**一句话结论**：

> 先把 8T 的 reserved-memory 布局改成 factory 同款，并用一个节点同时承载 upstream/downstream label；这才是当前最小、最干净、证据最强的修复路径。
