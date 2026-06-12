# OnePlus 8T (kebab) 4.19 -> 5.10 Static Gap Report

Date: 2026-06-12

Scope:
- Source 4.19 tree: `E:\5.10-main\android_kernel_oneplus_sm8250_los_noksu-upstream-latest`
- Target 5.10 tree: `E:\5.10-main\5.10-main`
- Target device: OnePlus 8T / kebab, project IDs `19805` and `20809`
- Method: static source inspection only. No build, no dtc, no kernel compile.

## Executive Summary

The 5.10 tree already contains a partial OnePlus SM8250/kebab port, but the current kebab DT path is not yet equivalent to the 4.19 kebab device tree.

Highest-risk gaps:

1. 5.10 kebab DTBO does not include an equivalent of the 4.19 kebab-specific camera sensor overlay.
2. 5.10 `sm8250-oneplus8t-mtp.dts` includes `sm8250-oneplus8t-19811-sensor.dtsi`, and several touch/fingerprint/sensor nodes still carry 19811 or 8 Pro style data.
3. 5.10 `arch/arm64/boot/dts/oplus/kona-kebab.dts` previously overrode `oplus,dtsi_no` to `<19805>`, while 4.19 kebab and the 5.10 base DTS both use `<19805 20809>`. This was aligned in the first minimal patch.
4. Several enabled DTS nodes have no matching driver compatible in the current 5.10 tree, or have drivers present but are not enabled in `sm8250-oneplus8t_defconfig`.

## DTS Entry Points

4.19 kebab overlay:

- `arch/arm64/boot/dts/vendor/oplus/kona-kebab-overlay.dts`
- Includes `kebab/kona-mtp.dtsi`
- Includes `kebab/kona-camera-sensor.dtsi`
- Uses `oplus,dtsi_no = <19805 20809>`

5.10 kebab overlay:

- `arch/arm64/boot/dts/oplus/kona-kebab.dts`
- Includes only `../qcom/sm8250-oneplus8t-mtp.dts`
- Uses `oplus,dtsi_no = <19805 20809>`

5.10 base DTS:

- `arch/arm64/boot/dts/qcom/sm8250-oneplus8t.dts`
- Uses model `kona MTP 19805 20809`
- Uses compatible `oneplus,kebab`
- Uses `oplus,dtsi_no = <19805 20809>`

Finding: the 5.10 DTBO metadata is now aligned with the base DTS project list. The next review should still confirm whether ABL/bootloader matching expects both Oplus project IDs in the overlay.

## Include Chain Gaps

4.19 kebab `kona-mtp.dtsi` includes:

- `kona-pmic-overlay.dtsi`
- `kona-sde-display.dtsi`
- `kona-audio-overlay.dtsi`
- `kona-thermal-overlay.dtsi`
- `kona-sensor.dtsi`

4.19 kebab overlay additionally includes:

- `kebab/kona-camera-sensor.dtsi`

5.10 `sm8250-oneplus8t-mtp.dts` includes:

- `sm8250-oneplus8t-pmic-overlay.dtsi`
- `sm8250-oneplus8t-sde-display.dtsi`
- `sm8250-oneplus8t-audio-overlay.dtsi`
- `sm8250-oneplus8t-thermal-overlay.dtsi`
- `sm8250-oneplus8t-19811-sensor.dtsi`

Finding: 5.10 has a generic camera include through `qcom/kona-port.dtsi`, but no kebab-specific equivalent to 4.19 `kebab/kona-camera-sensor.dtsi`.

Impact: kebab camera flash, actuator, OIS, EEPROM, rear sensors, and front sensors from 4.19 are not represented in the 5.10 kebab include path.

## 19811 / 8 Pro Residue

Observed in `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-mtp.dts`:

- Includes `sm8250-oneplus8t-19811-sensor.dtsi`
- NFC chipset property still references `chipset-19811`
- Fingerprint chip names use `G_OPTICAL_19811_*`
- Touch node label `mtp_19811:s6sy791_19811@48`
- Samsung touch node uses `project_id = <19811>`
- Samsung touch node uses `platform_support_project = <19811>`
- Synaptics touch node uses `project_id = <19811>`
- Synaptics touch node mixes `platform_support_project = <19161 19163 19805 19811>`

Observed in `arch/arm64/boot/dts/qcom/sm8250-oneplus8t-19811-sensor.dtsi`:

- File name is project-specific to `19811`
- Comment explicitly references OnePlus `19811`
- Sensor data does not match 4.19 kebab sensor overlay.

Impact: project matching and runtime parameter selection can select 19811 behavior on kebab. Touch and sensor are especially risky.

## Touch

4.19 kebab:

- File: `arch/arm64/boot/dts/vendor/oplus/kebab/kona-mtp.dtsi`
- Active touch path uses `synaptics-s3908`
- `platform_support_project = <19805 20809>`
- `platform_support_project_dir = <19805 20809>`
- Panel/display coordinates are kebab 1080x2400 style.

5.10:

- `qcom,i2c-touch-active = "st,fts"`
- Contains `st,fts`
- Contains `sec-s6sy791`
- Contains `synaptics-s3908`
- Samsung node is 19811-only and 1440x3168 style.
- Synaptics node includes 19805 but also has `project_id = <19811>` and mixed project lists.

Driver/config observations:

- `st,fts` compatible exists in `drivers/input/touchscreen/st/fts.c`.
- `TOUCHSCREEN_ST`, `TOUCHSCREEN_S6SY761`, and `TOUCHSCREEN_SYNAPTICS_DSX` Kconfig entries exist.
- `arch/arm64/configs/sm8250-oneplus8t_defconfig` does not explicitly enable those touch configs.

Finding: 5.10 touch DTS is not a clean kebab migration. The active touch controller and project filters need a separate correction plan before changing files.

## Display

4.19 kebab:

- `kebab/kona-sde-display.dtsi` includes only `dsi-panel-oplus20828-samsung-amb655x-1080-2400-120fps.dtsi`
- Default panel is `dsi_oplus20828samsung_amb655x_1080_2400_cmd`

5.10:

- `sm8250-oneplus8t-sde-display.dtsi` includes many generic and Oplus panels.
- It includes `dsi-panel-oplus19811-samsung-1440-3168-dsc-cmd.dtsi`.
- Primary default panel is still `dsi_oplus20828samsung_amb655x_1080_2400_cmd`.
- Secondary DSI default points to `dsi_oplus19065_samsung_1440_3168_dsc_cmd`.

Finding: primary panel appears close to kebab, but secondary DSI and 19811/19065 panel residue should be reviewed to ensure DRM does not enumerate an unintended panel path.

## Camera

4.19 kebab-specific camera overlay:

- File: `arch/arm64/boot/dts/vendor/oplus/kebab/kona-camera-sensor.dtsi`
- Defines camera flash nodes.
- Defines actuator.
- Defines OIS.
- Defines EEPROM nodes.
- Defines rear camera sensors.
- Defines front camera sensors.

5.10:

- No kebab-specific camera sensor overlay was found in the OnePlus 8T/kebab include chain.
- Generic `camera/kona-camera.dtsi` is included from `qcom/kona-port.dtsi`, but that is not a replacement for kebab sensor topology.

Finding: camera is the largest DTS migration gap. Do not attempt a one-line include without first checking labels, regulators, clocks, GPIOs, CCI/I2C bus names, and camera framework differences between 4.19 and 5.10.

## Sensors

4.19 kebab:

- `kebab/kona-sensor.dtsi` uses BMI260 for gsensor.
- Uses `als-type = <2>`.

5.10:

- `sm8250-oneplus8t-19811-sensor.dtsi` uses LSM6DSM for gsensor.
- Uses `als-type = <1>`.
- Comment references OnePlus `19811`.

Finding: the 5.10 sensor overlay appears to be 19811-derived, not kebab-equivalent.

## Fingerprint / Secure / NFC / Haptic / Hall

Enabled or present DTS compatibles without matching static driver compatible found in the searched 5.10 areas:

- `oplus,fp_common`
- `goodix,goodix_fp`
- `oplus,secure_common`
- `oplus-nfc-chipset`
- `qcom,nq-nci`
- `oplus,haptic-feedback`
- `awinic,aw8697_haptic`
- `oplus,hall-ist8801,up`
- `oplus,hall-ist8801,down`
- `oplus,hall-mxm1120,up`
- `oplus,hall-mxm1120,down`
- `oplus,sensor-feedback`
- `oplus,track-charge`

4.19 has related Oplus driver directories that are absent or not obviously ported in 5.10:

- `drivers/input/oplus_fp_drivers`
- `drivers/input/oplus_secure_drivers`
- `drivers/input/touchscreen/oplus_touchscreen`
- `drivers/nfc/oplus_nfc`
- `drivers/misc/aw8697_haptic`

Finding: these nodes should either get their drivers ported/configured, or be explicitly disabled/removed for the first bring-up stage.

## Charging / Power

5.10 DTS contains:

- `oplus,mp2650-charger`
- `oplus,da9313-divider`
- `oplus,bq27541-battery`
- `oplus,stm8s-fastcg`
- disabled wireless charging nodes using `status = "disable"`

5.10 driver availability:

- `oplus,mp2650-charger` driver exists in `drivers/power/oplus/charger_ic/oplus_mp2650.c`.
- `oplus,da9313-divider` driver exists in `drivers/power/oplus/charger_ic/oplus_da9313.c`.
- `oplus,bq27541-battery` driver exists in `drivers/power/oplus/gauge_ic/oplus_bq27541.c`.

5.10 defconfig:

- `CONFIG_OPLUS_GAUGE_IC_BQ27541=y`
- `# CONFIG_OPLUS_CHARGER_IC_MP2650 is not set`
- `# CONFIG_OPLUS_CHARGER_IC_DA9313 is not set`

Finding: BQ27541 is enabled, but MP2650 and DA9313 have DTS nodes with matching drivers that are not enabled in the 8T defconfig.

4.19 kebab fast charge:

- Uses `oplus,rk826-fastcg`

5.10:

- Uses `oplus,stm8s-fastcg`

Finding: fast charge IC identity differs from 4.19 kebab. This must be verified against real hardware variant data before enabling or porting.

## DTS Status String Risks

5.10 `sm8250-oneplus8t-mtp.dts` contains many `status = "ok"` values and several `status = "disable"` values.

Known Linux DT convention is `status = "okay"` or `status = "disabled"`. Downstream Qualcomm trees often tolerate `ok`, but `disable` is more suspicious because it is not the standard disabled spelling.

Observed `status = "disable"` lines in 5.10 OnePlus 8T MTP:

- Wireless charger
- Wireless receiver chip
- Charge pump charger
- `ti,bq2597x-standalone`
- Additional disabled charging-related blocks later in the file

Finding: these should be normalized only as part of a controlled DTS cleanup. Changing status strings can enable or disable hardware paths, so this should not be bundled into unrelated fixes.

## Recommended Next Plan

Do not compile locally.

Phase 1: DTS identity cleanup

- Keep `oplus,dtsi_no = <19805 20809>` in kebab DTBO unless a bootloader reason proves otherwise. This is already applied.
- Split or rename `sm8250-oneplus8t-19811-sensor.dtsi` into a kebab-specific sensor overlay.
- Remove 19811 project IDs from kebab active touch/sensor/fingerprint paths, or gate them so they cannot bind for 19805/20809.

Phase 2: camera migration plan

- Compare 4.19 `kebab/kona-camera-sensor.dtsi` labels against 5.10 `camera/kona-camera.dtsi`.
- Build a kebab camera overlay that only references labels existing in 5.10.
- Keep camera disabled until all label/regulator/CCI dependencies are verified statically.

Phase 3: driver/config alignment

- Decide first-boot hardware scope: minimal boot/display/touch/USB/charging, or full feature parity.
- For minimal bring-up, disable nodes with no 5.10 driver match.
- For feature parity, port missing Oplus drivers in small groups: fingerprint/secure, NFC, haptic, hall, Oplus touchscreen glue.
- Enable only drivers whose DTS nodes are intended to bind for kebab.

Phase 4: defconfig review

- Review touch configs for the actual kebab panel.
- Review `CONFIG_OPLUS_CHARGER_IC_MP2650` and `CONFIG_OPLUS_CHARGER_IC_DA9313`.
- Review `CONFIG_SND_SOC_TFA9879` or the correct speaker amplifier option for the 5.10 audio path.

## Applied First Code Change Set

Applied after this report was first generated:

1. Fixed kebab DTBO metadata to preserve `oplus,dtsi_no = <19805 20809>`.
2. Added a TODO comment documenting that `sm8250-oneplus8t-19811-sensor.dtsi` is not final for kebab.
3. Do not include camera overlay yet.
4. Do not enable missing drivers yet.

This keeps the first patch limited to identity/matching cleanup and avoids accidentally enabling unverified hardware paths.
