# AGENTS.md — SM8250 OnePlus Kernel Port (5.10 → device)

## What this repo is

Porting OnePlus 8T (kebab) and 8 Pro (instantnoodlep) from Linux 4.19 to **5.10.240 Android Common Kernel** (ACK android12-5.10, KMI gen 9). SoC: Qualcomm SM8250 (kona, Snapdragon 865). This is **not** a pristine upstream — it has local device bringup patches on `main`.

## Tree layout

| Path | Purpose |
|---|---|
| `5.10/` | **Active working tree** — git repo (`origin: github.com/ye3912/5.10.git`, branch `main`) |
| `android_kernel_oneplus_sm8250_los_noksu-upstream-latest/` | **Reference only** — 4.19.325 LineageOS kernel, not a git repo, no local changes |

Reference path shortcuts are relative to `5.10/`. Always do all work in `5.10/`.

## Defconfigs

| File | Device |
|---|---|
| `arch/arm64/configs/sm8250-oneplus8t_defconfig` | OnePlus 8T (kebab) — 335 lines |
| `arch/arm64/configs/sm8250-oneplus8pro_defconfig` | OnePlus 8 Pro — 153 lines |
| `build.config.gki*` / `build.config.aarch64` | Android GKI build (CI only) |

Both defconfigs enable: `CONFIG_BUILD_ARM64_DT_OVERLAY=y`, `CONFIG_ARCH_KONA=y`, `CONFIG_OPLUS=y`, PSTORE, UFS, SPMI, OPLUS_CHG.

## Build commands

```bash
# Generate defconfig
make ARCH=arm64 O=out sm8250-oneplus8t_defconfig

# Kernel Image.gz
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image.gz

# DTBs — use -k to skip upstream RB5 clock_rpmh build breakage
make ARCH=arm64 -j$(nproc) dtbs -k

# Single DTB (for quick verification)
make ARCH=arm64 O=out qcom/sm8250-oneplus8t.dtb

# Decompile DTB to check fields
dtc -I dtb -O dts out/arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb
```

## DTB/DTBO packaging (AnyKernel3-style)

ABL expects **dtb.img** (Android DT table format, magic `0xD7B7AB1E`), not a raw `.dtb` file.

```bash
# Create dtb.img from base DTB
python3 scripts/mkdtboimg.py create dtb.img arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dtb

# Create dtbo.img from overlay
python3 scripts/mkdtboimg.py create dtbo.img arch/arm64/boot/dts/oplus/kona-kebab.dtbo
```

## DTS entry points

| File | Role |
|---|---|
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts` | 8T **base DTB** (SoC skeleton, `qcom,board-id` for ABL matching) |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-mtp.dts` | 8T MTP include — aggregates pmic/display/audio/thermal/sensor overlays |
| `arch/arm64/boot/dts/oplus/kona-kebab.dts` | 8T **DTBO overlay** — board-id metadata + includes `sm8250-oneplus8t-mtp.dts` |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8pro.dts` | 8 Pro base DTB |
| `arch/arm64/boot/dts/qcom/sm8250-oneplus8pro-mtp.dts` | 8 Pro MTP include |
| `arch/arm64/boot/dts/qcom/kona-port.dtsi` | 4.19 downstream SoC peripheral port (4148 lines) |
| `arch/arm64/boot/dts/qcom/sm8250.dtsi` | Upstream SM8250 SoC skeleton (3171 lines) |

ABL boot flow: match base DTB by `msm-id` → match DTBO by `board-id` → merge.

### DTS include chain (8T)

```
sm8250-oneplus8t.dts                          # Base DTB: SoC skeleton + reserved memory
  ├── sm8250.dtsi                             # Upstream QCOM SoC (3171 lines)
  ├── kona-port.dtsi                          # 4.19 downstream port (4148 lines)
  └── pm8150[b/l]/pm8009.dtsi                # PMIC skeletons
via DTBO:
  └── kona-kebab.dts (DTBO overlay)          # Board-id metadata, #include sm8250-oneplus8t-mtp.dts
        └── sm8250-oneplus8t-mtp.dts          # All device hardware (2540 lines)
              ├── sm8250-oneplus8t-pmic-overlay.dtsi    # SPMI, buttons, GPIOs
              ├── sm8250-oneplus8t-sde-display.dtsi     # 10+ DSI panels, Samsung AMB655X default
              ├── sm8250-oneplus8t-audio-overlay.dtsi   # Bolero, WCD938x codec
              ├── sm8250-oneplus8t-thermal-overlay.dtsi # Cooling maps, BCL
              └── sm8250-oneplus8t-19811-sensor.dtsi    # GSensor LSM6DSM, MSensor AKM0991X/MMC5603
```

### 4.19 reference comparison

4.19 overlay at `arch/arm64/boot/dts/vendor/oplus/kona-kebab-overlay.dts` additionally includes `kebab/kona-camera-sensor.dtsi` — this is still a documented gap in 5.10.

## DTBO overlay approach (8T)

**ABL matching fields** in the DTS root node — fastboot troubleshooting priority #1:

```dts
qcom,board-id = <0x10008 0>;      // 8T. 8 Pro uses <0x58 0>
qcom,msm-id = <0x164 0x20001>;     // SoC version
oplus,dtsi_no = <19805 20809>;     // OPLUS project ID pair
oplus,pcb_range = <0 56>;          // PCB revision range
```

When the device goes straight to fastboot, always investigate DTB matching before any driver work. Plan at `SM8250_8T_FASTBOOT_FIX_PLAN.md` has detailed step-by-step.

## GPU: KGSL instead of upstream DRM

Both 8T and 8 Pro DTS have `&gpu { status = "disabled"; }` and `&gmu { status = "disabled"; }`. The KGSL driver (`CONFIG_QCOM_KGSL=y`) replaces the upstream DRM/MSM Adreno, matching the 4.19 downstream approach.

## CI (GitHub Actions)

`.github/workflows/build.yml` — builds both defconfigs, runs on ubuntu-24.04 with `gcc-aarch64-linux-gnu-`. Validates critical boot configs (UFS, SPMI, PSTORE, KGSL, OPLUS_LOGBUF) before building. Build order: `defconfig` → `Image.gz` → `dtbs -k` → `mkdtboimg.py` → `AnyKernel3 zip`.

## OPLUS driver stack (5 layers)

| Layer | Directory | Key files |
|---|---|---|
| **SoC/Platform** | `drivers/soc/oplus/` | `oplus_project.c` (SMEM project ID), `device_info.c` (procfs) |
| **Charging core** | `drivers/power/oplus/` | `oplus_chg_core.c` (module registry), `oplus_charger.c` (L3A/L3B state machine), `oplus_chg_voter.c` (vote arbitration) |
| **Charger IC** | `drivers/power/oplus/charger_ic/` | `oplus_mp2650.c` (stub), `oplus_da9313.c` (charge pump) |
| **Gauge IC** | `drivers/power/oplus/gauge_ic/` | `oplus_bq27541.c` (stub) |
| **SM8250 adapter** | `drivers/power/supply/qcom/` | `oplus_battery_kona.c` (1881 lines, SMB5 adapter, ~120 callbacks) |
| **Kernel utils** | `kernel/` | `oplus_logbuf.c` (reserved mem ring buffer console), `sched/oplus_tune.c` (SchedTune defaults) |

47 OPLUS-related source files across the tree. All entered via Kconfig from `arch/arm64/Kconfig.oplus` (577 lines, `menu "OPLUS vendor features"`).

## Local patches (5 commits on main)

| Commit | Files | What |
|---|---|---|
| `2e4750f01` | 67,437 files | **Initial import** of ACK android12-5.10 + OnePlus DTS/defconfig (+30M lines) |
| `5dfa0379a` | 1,298 files | Restore `.S` assembly sources ignored by gitignore (+370k lines) |
| `c50547474` | 12 files | Fix gitignore for techpack + perf scripts, add techpack/stub (+842 lines) |
| `151d885ab` | 1 file | `crypto-qti-common.c`: fix 29 `pr_err` format strings missing `__func__` arg |
| `d6312a8ce` | 7 files | Fix USB build warnings (`__maybe_unused` in debug-ipc.h, `%lu` in phy-msm-snps-hs.c), delete stale plans |

## Key SM8250 subsystem drivers

| Subsystem | Driver(s) |
|---|---|
| **Clocks** | `gcc-sm8250.c`, `dispcc-sm8250.c`, `gpucc-sm8250.c`, `camcc-sm8250.c`, `npucc-sm8250.c`, `videocc-sm8250.c` |
| **Interconnect** | `sm8250.c` (drivers/interconnect/qcom/) |
| **PHY** | `phy-qcom-qmp.c` (USB3/PCIe/UFS), `phy-qcom-qusb2.c` (USB2), `phy-qcom-snps-femto-v2.c` |
| **UFS** | `ufs-qcom.c`, `ufs-qcom-ice.c`, `ufshcd-crypto-qti.c` |
| **USB** | `dwc3` via `CONFIG_USB_DWC3_MSM/QCOM`, `phy-msm-snps-hs.c` (HS PHY + charger detection) |
| **SoC core** | `rpmh-rsc.c`, `rpmhpd.c`, `qcom_geni-se.c`, `qcom_aoss.c`, `llcc-qcom.c`, `smem.c`, `smp2p.c`, `cmd-db.c` |
| **Crypto** | `crypto-qti-common.c`, `crypto-qti-tz.c`, `crypto-qti-hwkm.c` |
| **Pinctrl** | `pinctrl-sm8250.c` |
| **SPMI PMIC** | `spmi-pmic-arb.c` |
| **Power** | `qcom-power-supply.c`, `qcom-tsens.c` |

## Toolchain notes

- CI uses `gcc-aarch64-linux-gnu-` from apt
- `build.config.common` references `LLVM=1` with `clang-r416183b` — prebuilts are **not checked in**, local builds need either the Android prebuilt toolchain or equivalent cross-compiler
- Hermetic toolchain is enabled by default (`HERMETIC_TOOLCHAIN=1`)

## Mnemo

The repo has Mnemo initialized (58196 nodes in knowledge graph). Use `mnemo_recall` at session start to load context. The `.amazonq/rules/mnemo.md` has full Mnemo usage reference.

## Development plans

Historical plans in `docs/superpowers/plans/` were cleaned up in the latest commit. Active planning should use `mnemo_plan` for task tracking.
