# AGENTS.md — SM8250 OnePlus Kernel Port Strict Agent Rules

## 0. Mission

This repo ports OnePlus 8T (`kebab`) and OnePlus 8 Pro (`instantnoodlep`) from Linux 4.19 downstream to **Linux 5.10.240 Android Common Kernel** (`android12-5.10`, KMI gen 9) for Qualcomm SM8250 / Kona / Snapdragon 865.

This is **not** a pristine upstream tree. It contains local device bring-up patches on `main`. Treat it as a fragile hardware port where unsupported guesses can produce non-booting images or Qualcomm 900E crashes.

The agent's primary job is to produce evidence-backed, minimal, reversible kernel-porting work. Do not optimize for speed at the cost of evidence.

---

## 1. Non-Negotiable Rules

### 1.1 No Guessing

Never invent code, DTS properties, Kconfig options, addresses, regulator names, GPIOs, clocks, panel names, charger behavior, or boot arguments.

Every proposed or applied change must cite at least one concrete source:

- Current 5.10 source in this repo.
- 4.19 reference source.
- Factory DTB / decompiled DTB / boot image evidence supplied by the user.
- User-provided patch under `C:\5.10\patch`.
- Authoritative upstream/Linux/Android/Qualcomm documentation.

If no source exists, stop and ask the user for evidence. Do not fill gaps with intuition.

### 1.2 No Lazy Shortcuts

Do not answer with vague instructions like "check logs", "compare DTS", "enable dependencies", or "try building" without exact files, commands, expected output, and failure interpretation.

For any non-trivial task, the response must include:

- Current conclusion.
- Evidence source.
- Exact affected files.
- Minimal action.
- Verification command.
- Expected pass condition.
- Stop condition / rollback point.
- What the user must provide next, if anything.

### 1.3 One Subsystem at a Time

Do not combine unrelated subsystems in one change. Keep DTS, defconfig, and driver changes separate unless a referenced patch proves they must land together.

Preferred commit/change granularity:

- One build/config fix.
- One DTS boot-match fix.
- One reserved-memory/PIL fix.
- One USB fix.
- One charger/gauge fix.
- One display/panel fix.

### 1.4 Logs Before More Drivers

If the device enters 900E, black screen, fastboot fallback, or silent reboot without logs, prioritize observability before feature work.

Before adding or modifying more hardware drivers, establish at least one log path:

- `panic_logstore` to `/dev/block/by-name/metadata`.
- pstore / ramoops.
- recovery `dmesg`.
- serial / early console if available.

No logs means no broad driver porting.

### 1.5 Verify Kconfig Dependencies

Never assume a defconfig line is active. After changing defconfig, require generated `.config` verification.

Example pattern:

```bash
make ARCH=arm64 O=out sm8250-oneplus8pro_defconfig
grep -E 'CONFIG_(NET|INET|AUDIT|SECURITY_SELINUX|SECURITY_SELINUX_DEVELOP|PANIC_LOGSTORE)=' out/.config
```

If Kconfig silently drops an option, inspect its `depends on` chain before changing code.

### 1.6 Treat 8T and 8 Pro Separately

Do not copy 8T addresses, board IDs, panel assumptions, or reserved-memory layouts into 8 Pro without independent 8 Pro evidence.

Known split:

- 8T board-id: `qcom,board-id = <0x10008 0>`.
- 8 Pro board-id: `qcom,board-id = <0x58 0>`.

8T fixes are not automatically valid for 8 Pro.

---

## 2. Working Tree and Reference Tree

| Path | Purpose |
|---|---|
| `C:\5.10\5.10` | **Active working tree**. This is the git repo. Do all source work here. |
| `C:\5.10\android_kernel_oneplus_sm8250_los_noksu-upstream-latest` | **Reference only** 4.19.325 LineageOS/downstream kernel. Do not modify. |
| `C:\5.10\patch` | User-provided patch references. Treat as evidence, not automatically correct. |
| `C:\5.10\migration-plans` | Plans outside the git repo. Use for planning files when the user asks not to write plans into the repo. |

Before editing, confirm the target file is under `C:\5.10\5.10` unless the user explicitly asks for an external plan/config file.

---

## 3. Required Workflow for Every Technical Task

### 3.1 Intake

1. Restate the technical objective in one sentence.
2. Identify the device: 8T, 8 Pro, or both.
3. Identify the current boot/build state.
4. Identify the evidence already available.
5. Identify what must not be touched.

### 3.2 Evidence Collection

Use the most direct evidence first:

1. Current file in 5.10 tree.
2. Matching file in 4.19 reference tree.
3. User-provided patch.
4. DTB/DTBO/boot image decompile.
5. Kconfig dependency chain.
6. Build log / dmesg / pstore / metadata.

Do not proceed from memory if a file or log can be checked.

### 3.3 Change Proposal

Before modifying source, define:

- Files to modify.
- Lines/symbols/nodes to inspect.
- Exact evidence for each intended change.
- Why this is the minimum change.
- How to verify success.
- How to revert if it fails.

### 3.4 Implementation

Apply the smallest possible patch. Do not reformat unrelated code. Do not rename symbols or move files unless required by evidence.

### 3.5 Verification

Use the narrowest verification first, then broader checks:

1. `git diff --check`.
2. Single object / single DTB build if possible.
3. Defconfig generation + `.config` grep.
4. Full `Image.gz` / `dtbs -k` when needed.
5. Boot image packaging.
6. True device boot/log verification.

If Windows cannot run the Linux build reliably, state that verification is pending on the Linux build host and provide exact commands for the user.

### 3.6 Completion Report

Never claim "fixed" unless verified. Use precise states:

- "patched, not build-verified"
- "build-verified only"
- "DTB verified, not boot-verified"
- "boot behavior changed, root cause still under investigation"
- "fixed and verified by logs/build/device"

---

## 4. Current Project Facts

### 4.1 Defconfigs

| File | Device |
|---|---|
| `arch/arm64/configs/sm8250-oneplus8t_defconfig` | OnePlus 8T / kebab |
| `arch/arm64/configs/sm8250-oneplus8pro_defconfig` | OnePlus 8 Pro / instantnoodlep |
| `build.config.gki*` / `build.config.aarch64` | Android GKI / CI paths |

Both device defconfigs are expected to cover `CONFIG_BUILD_ARM64_DT_OVERLAY=y`, `CONFIG_ARCH_KONA=y`, `CONFIG_OPLUS=y`, PSTORE, UFS, SPMI, OPLUS charging features, and any current debug/logging requirements.

### 4.2 Build Commands

Generate 8T config:

```bash
make ARCH=arm64 O=out sm8250-oneplus8t_defconfig
```

Generate 8 Pro config:

```bash
make ARCH=arm64 O=out sm8250-oneplus8pro_defconfig
```

Kernel image:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image.gz
```

DTBs, tolerating known unrelated upstream RB5 breakage:

```bash
make ARCH=arm64 O=out -j$(nproc) dtbs -k
```

Single DTB quick checks:

```bash
make ARCH=arm64 O=out qcom/sm8250-oneplus8t.dtb
make ARCH=arm64 O=out qcom/sm8250-oneplus8pro.dtb
```

Decompile DTB:

```bash
dtc -I dtb -O dts out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb > /tmp/sm8250-oneplus8t.dts
dtc -I dtb -O dts out/arch/arm64/boot/dts/qcom/sm8250-oneplus8pro.dtb > /tmp/sm8250-oneplus8pro.dts
```

### 4.3 DTB / DTBO Packaging

ABL expects **Android DT table** images, not raw `.dtb` files. DT table magic is `0xD7B7AB1E`.

```bash
python3 scripts/mkdtboimg.py create dtb.img arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb
python3 scripts/mkdtboimg.py create dtbo.img arch/arm64/boot/dts/oplus/kona-kebab.dtbo
```

When a device goes straight to fastboot, verify DTB/DTBO matching and packaging before touching drivers.

---

## 5. DTS Entry Points

| File | Role |
|---|---|
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts` | 8T base DTB, SoC skeleton, reserved memory, ABL matching |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-mtp.dts` | 8T hardware aggregate include |
| `arch/arm64/boot/dts/oplus/kona-kebab.dts` | 8T DTBO overlay, board metadata and 8T MTP include |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8pro.dts` | 8 Pro base DTB |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8pro-mtp.dts` | 8 Pro hardware aggregate include |
| `arch/arm64/boot/dts/qcom/kona-port.dtsi` | 4.19 downstream SoC peripheral port |
| `arch/arm64/boot/dts/qcom/sm8250.dtsi` | Upstream SM8250 SoC skeleton |

ABL boot flow:

```text
match base DTB by msm-id -> match DTBO by board-id -> merge -> boot kernel
```

8T include chain:

```text
sm8250-oneplus8t.dts
  ├── sm8250.dtsi
  ├── kona-port.dtsi
  └── pm8150[b/l]/pm8009.dtsi
via DTBO:
  └── kona-kebab.dts
        └── sm8250-oneplus8t-mtp.dts
              ├── sm8250-oneplus8t-pmic-overlay.dtsi
              ├── sm8250-oneplus8t-sde-display.dtsi
              ├── sm8250-oneplus8t-audio-overlay.dtsi
              ├── sm8250-oneplus8t-thermal-overlay.dtsi
              └── sm8250-oneplus8t-19811-sensor.dtsi
```

4.19 8T overlay additionally includes `kebab/kona-camera-sensor.dtsi`; this is a known remaining 5.10 gap and must not be guessed.

---

## 6. Boot-Critical Known Facts

### 6.1 8T ABL Matching Fields

Known 8T fields from 4.19/factory evidence:

```dts
qcom,board-id = <0x10008 0>;
qcom,msm-id = <0x164 0x20001>;
oplus,dtsi_no = <19805 20809>;
oplus,pcb_range = <0 56>;
```

If the device immediately enters fastboot, inspect these fields before driver work.

### 6.2 8T Reserved-Memory / PIL Carveouts

The 8T 900E issue previously traced to upstream/downstream reserved-memory double-stack conflicts. The verified factory-aligned carveouts are:

```text
removed_mem          0x80b00000 / 0x0cd00000
cdsp_mem/pil_cdsp    0x8f200000 / 0x01400000
adsp_mem/pil_adsp    0x91b00000 / 0x02500000
spss_mem/pil_spss    0x94000000 / 0x00100000
cdsp_secure_heap     0x94100000 / 0x04600000
kboot_uboot_logmem   0x9fe00000 / 0x00200000
ramoops              0xb0000000 / 0x00400000
```

The correct DTS pattern is one physical node with both upstream and downstream labels, for example:

```dts
cdsp_mem: pil_cdsp_mem: memory@8f200000 { ... };
adsp_mem: pil_adsp_mem: memory@91b00000 { ... };
spss_mem: pil_spss_mem: memory@94000000 { ... };
```

Do not create two nodes for one physical carveout.

### 6.3 SPSS PIL Address

If SPSS reserved memory is relocated to `0x94000000`, any hardcoded `qcom,pil-addr` referring to old `0x8BE00000` must be reviewed against the DTS consumer and evidence.

### 6.4 GPU Policy

Both 8T and 8 Pro DTS currently disable upstream DRM/MSM `&gpu` and `&gmu`. KGSL (`CONFIG_QCOM_KGSL=y`) is the intended downstream-style GPU path.

Do not re-enable upstream DRM GPU as a shortcut unless the user explicitly asks and a full compatibility plan exists.

---

## 7. Panic Logstore / SELinux Rules

`panic_logstore` must remain based on the user-provided LibXZR patches unless the user explicitly approves a different design.

Reference patches:

- `C:\5.10\patch\1add6c04024489abd189174b65bd18c9ab83d681.patch`
- `C:\5.10\patch\1afe0d1c80cdd26c4b0b0c8a7791d3e2b40c957e.patch`

Current target path:

```c
#define LOG_FILE_PATH "/dev/block/by-name/metadata"
```

Required SELinux dependency chain for `sel_set_enforce()`:

```text
CONFIG_NET=y
CONFIG_INET=y
CONFIG_AUDIT=y
CONFIG_SECURITY=y
CONFIG_SECURITYFS=y
CONFIG_SECURITY_NETWORK=y
CONFIG_SECURITY_SELINUX=y
CONFIG_SECURITY_SELINUX_DEVELOP=y
CONFIG_PANIC_LOGSTORE=y
```

Do not add magic headers, CRCs, custom metadata formats, or new storage protocols unless backed by a supplied patch or explicit user approval.

---

## 8. OPLUS Driver Stack

| Layer | Directory | Key files |
|---|---|---|
| SoC / Platform | `drivers/soc/oplus/` | `oplus_project.c`, `device_info.c` |
| Charging core | `drivers/power/oplus/` | `oplus_chg_core.c`, `oplus_charger.c`, `oplus_chg_voter.c` |
| Charger IC | `drivers/power/oplus/charger_ic/` | `oplus_mp2650.c`, `oplus_da9313.c` |
| Gauge IC | `drivers/power/oplus/gauge_ic/` | `oplus_bq27541.c` |
| SM8250 adapter | `drivers/power/supply/qcom/` | `oplus_battery_kona.c` |
| Kernel utils | `kernel/` | `oplus_logbuf.c`, `sched/oplus_tune.c` |

OPLUS code enters through `arch/arm64/Kconfig.oplus`. Do not enable unknown OPLUS features just because they compile.

Charging/gauge work must be delayed until boot logs and storage/USB basics are stable, unless the current failure log directly implicates charger code.

---

## 9. Subsystem Bring-Up Order

Follow this order unless logs prove a different blocker:

1. Build/config correctness.
2. Panic/log retrieval path.
3. DTB/DTBO bootloader matching.
4. Reserved-memory / PIL / remoteproc carveouts.
5. UFS / ICE / storage.
6. RPMh / regulators / power domains.
7. SPMI / PMIC / pinctrl.
8. USB / PHY / adb.
9. Display / panel / framebuffer.
10. Touch.
11. Charger / gauge.
12. Audio.
13. Sensors.
14. Camera last.

Camera must not be started until clocks, regulators, IOMMU, CCI/I2C, display, and reserved-memory are stable.

---

## 10. Key SM8250 Drivers

| Subsystem | Driver(s) |
|---|---|
| Clocks | `gcc-sm8250.c`, `dispcc-sm8250.c`, `gpucc-sm8250.c`, `camcc-sm8250.c`, `npucc-sm8250.c`, `videocc-sm8250.c` |
| Interconnect | `drivers/interconnect/qcom/sm8250.c` |
| PHY | `phy-qcom-qmp.c`, `phy-qcom-qusb2.c`, `phy-qcom-snps-femto-v2.c` |
| UFS | `ufs-qcom.c`, `ufs-qcom-ice.c`, `ufshcd-crypto-qti.c` |
| USB | `CONFIG_USB_DWC3_MSM/QCOM`, `phy-msm-snps-hs.c` |
| SoC core | `rpmh-rsc.c`, `rpmhpd.c`, `qcom_geni-se.c`, `qcom_aoss.c`, `llcc-qcom.c`, `smem.c`, `smp2p.c`, `cmd-db.c` |
| Crypto | `crypto-qti-common.c`, `crypto-qti-tz.c`, `crypto-qti-hwkm.c` |
| Pinctrl | `pinctrl-sm8250.c` |
| SPMI PMIC | `spmi-pmic-arb.c` |
| Power | `qcom-power-supply.c`, `qcom-tsens.c` |

---

## 11. CI and Toolchain Notes

- CI uses `gcc-aarch64-linux-gnu-` on Ubuntu 24.04.
- `.github/workflows/build.yml` builds both defconfigs, validates critical boot configs, builds `Image.gz`, runs `dtbs -k`, creates DTB/DTBO images, and packages AnyKernel3 zip.
- `build.config.common` references `LLVM=1` and Android prebuilt clang (`clang-r416183b`), but prebuilts are not checked in.
- Hermetic toolchain is enabled by default via `HERMETIC_TOOLCHAIN=1`.

If local Windows cannot perform a reliable Linux kernel build, do not pretend it did. Provide Linux build-host commands and mark verification pending.

---

## 12. Required Output Template for Agents

For investigation or planning:

```text
结论：
证据：
涉及文件：
风险判断：
下一步：
需要用户提供：
```

For code or DTS changes:

```text
目标：
依据：
最小修改：
涉及文件：
验证命令：
预期结果：
失败时停止条件：
回滚点：
```

For build failures:

```text
错误原文：
直接触发点：
Kconfig/Makefile/源码依赖链：
已排除项：
最小修复：
验证命令：
```

Do not skip these sections for non-trivial technical work.

---

## 13. Anti-Failure Checklist

Before final response, verify:

- Did I cite concrete evidence for every technical claim?
- Did I avoid guessing addresses, GPIOs, clocks, panels, regulators, and Kconfig dependencies?
- Did I keep the change to one subsystem?
- Did I provide exact commands instead of vague advice?
- Did I state whether verification was actually run or only proposed?
- Did I avoid writing plans into the repo when the user asked for external planning?
- Did I avoid committing changes unless explicitly requested?
- Did I leave unrelated files untouched?

If any answer is "no", fix the response before sending.

---

## 14. Mnemo / Context

The repo has Mnemo initialized with a large knowledge graph. Use `mnemo_recall` at session start when available, and use Mnemo/ContextStream for prior decisions and code relationships.

Do not use memory as a substitute for checking the actual file when the file is available.
