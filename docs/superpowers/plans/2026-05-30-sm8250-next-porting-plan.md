# SM8250 Next Porting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `sm8250-kernel-upgrade` first. Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` only after the user explicitly approves code changes. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the OnePlus SM8250 Android 5.10 GKI port from boot-hang debugging into the next functional migration stage with the lowest regression risk.

**Architecture:** Keep boot-hang debug infrastructure as the current safety net, then migrate subsystems in dependency order: OPLUS SoC base layer → minimal charging power_supply closure → touchscreen input → display OPLUS features → fingerprint/camera/filesystem/network. Each stage must compile in CI before the next stage starts.

**Tech Stack:** Linux 5.10.240 Android GKI, ARM64, Qualcomm SM8250/Kona, OPLUS downstream 4.19 source tree, GitHub Actions CI. Local kernel compile is explicitly disallowed for this project session.

---

## Current Baseline

- Target repo: `/root/ye3912-5.10`
- Source 4.19 repo: `/root/oneplus-sm8250-kernel/`
- Latest pushed debug/display fixes:
  - `3b8a740f8` (`kernel: add oplus_screenlog DRM probe diagnostics`)
  - `fdd73606e` (`drm/msm: add SDE-KMS bind bridge and safe KMS_SDE init path`)
  - `00e2fe616` (`drm/msm: allow SDE-KMS probe to succeed without KMS backend`)
- Current boot debug stack:
  - `CONFIG_PANIC_LOGSTORE=y`
  - `CONFIG_OPLUS_LOGBUF=y`
  - `CONFIG_OPLUS_SCREENLOG=y`
  - reserved-memory printk mirror + hung_task dump + DRM screen renderer
- Constraint: do not run local kernel builds; use static checks locally and GitHub Actions CI remotely.

## 2026-05-31 Display DRM Bootstrap Addendum

This addendum supersedes the old assumption that display work starts only at the OPLUS feature layer. Runtime debugging showed a lower-level blocker first: the 5.10 MSM DRM driver did not bind to the Kona downstream `qcom,sde-kms` display node, so `oplus_screenlog` could not obtain a registered DRM device.

### Current Display Bridge State

**Files already modified:**
- `kernel/oplus_screenlog.c`
- `drivers/gpu/drm/msm/msm_drv.c`

**Current behavior after `00e2fe616`:**
- `qcom,sde-kms` is matched by `msm_platform_driver`.
- `connectors = <&sde_dp &sde_wb &sde_dsi &sde_dsi1 &sde_rscc>;` is consumed through the downstream SDE connector phandle path.
- Placeholder component bridge drivers bind these downstream connector compatibles:
  - `qcom,dp-display`
  - `qcom,dsi-display`
  - `qcom,wb-display`
  - `qcom,sde-rsc`
- `KMS_SDE` is an independent type and skips `dpu_mdss_init()` because the flat Kona SDE node has `mdp_phys/vbif_phys/regdma_phys/...` resources, not upstream `mdss`.
- `KMS_SDE` currently uses `kms = NULL`, so DRM can register for diagnostics while the real KMS backend is not ported.

**Expected device-side diagnostic signals:**

```text
using downstream SDE connector binding: 5 components
bound downstream SDE connector bridge
qcom,sde-kms: running without KMS backend
```

If these messages do not appear, stop and fix the bridge/bind path before starting the KMS backend.

---

## Task 0D: Flat SDE KMS Backend Minimal Bring-up

**Goal:** Replace the diagnostic `kms = NULL` path with the smallest real KMS backend needed for `oplus_screenlog` and DRM client modeset creation.

**Files:**
- Compare source: `/root/oneplus-sm8250-kernel/techpack/display/msm/sde/sde_kms.c`
- Compare source: `/root/oneplus-sm8250-kernel/techpack/display/msm/sde/sde_hw_catalog.c`
- Compare source: `/root/oneplus-sm8250-kernel/techpack/display/msm/dsi/dsi_display.c`
- Compare target: `/root/ye3912-5.10/drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c`
- Compare target: `/root/ye3912-5.10/drivers/gpu/drm/msm/disp/dpu1/dpu_kms.h`
- Compare target: `/root/ye3912-5.10/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_catalog.c`
- Compare target: `/root/ye3912-5.10/drivers/gpu/drm/msm/dsi/`
- Likely modify: `/root/ye3912-5.10/drivers/gpu/drm/msm/msm_drv.c`
- Likely modify: `/root/ye3912-5.10/drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c`
- Likely modify: `/root/ye3912-5.10/drivers/gpu/drm/msm/disp/dpu1/dpu_kms.h`

- [ ] **Step 0D.1: Gate on current bridge evidence**

Flash a kernel containing `00e2fe616` only after CI is green. Capture dmesg or `oplus_logbuf` and confirm the three display bridge messages listed above.

Expected result: DRM master reaches the diagnostic no-KMS path without `failed to ioremap mdss`, NULL dereference, or component bind timeout.

- [x] **Step 0D.2: Read-only flat SDE vs DPU init comparison** — COMPLETED

**Mapping table (source-verified):**

| 4.19 flat SDE responsibility | 5.10 DPU equivalent | Action |
|---|---|---|
| `sde_kms_init()` L3927: `kzalloc` + `msm_kms_init()` | `dpu_bind()` L1083: `devm_kzalloc` + OPP + clocks + `msm_kms_init()` | **Reuse 5.10 alloc pattern** — devm safer |
| `_sde_kms_hw_init_ioremap()`: `"mdp_phys"` | `dpu_kms_hw_init()` L890: `"mdp"` | **Flat reg-name adapter** |
| `_sde_kms_hw_init_ioremap()`: `"vbif_phys"` | `dpu_kms_hw_init()`: `"vbif"` | **Flat reg-name adapter** |
| `_sde_kms_hw_init_ioremap()`: `"regdma_phys"` | `dpu_kms_hw_init()`: `"regdma"` | **Flat reg-name adapter** |
| `_sde_kms_hw_init_ioremap()`: `"sid_phys"` | `dpu_kms` has no `sid` field | **Skip for minimal KMS** |
| `_sde_kms_hw_init_ioremap()`: `"sde_imem_phys"` | `dpu_kms` has no `imem` field | **Skip for minimal KMS** |
| `sde_hw_catalog_init()` → `SDE_HW_VER_600` → `sm8250_cfg_init()` | `dpu_hw_catalog_init()` → `DPU_HW_VER_600` → `sm8250_cfg_init()` | **Direct reuse** — same hardware, same catalog entry |
| `_sde_kms_hw_init_blocks()`: RM/VBIF/perf/intr | `dpu_kms_hw_init()`: RM/VBIF/perf/intr | **Direct reuse** — same API signatures |
| `_sde_kms_hw_init_power_helper()` genpd | `dpu_bind()` OPP table | **Reuse 5.10 OPP** — simpler |
| `sde_kms->dsi_displays[]` downstream array | `_dpu_kms_initialize_dsi()` via bridge encoder | **Stub connector** — fixed modes for minimal KMS |
| No MDSS init (flat node) | `dpu_mdss_init()` maps `"mdss"` | **Skip** — Kona DTS has no `"mdss"` reg |

**Reuse decision: YES — reuse 5.10 DPU internals with flat reg-name adapter.**

Reasons: same hardware SM8250/Kona, same catalog `DPU_HW_VER_600`, `struct dpu_kms` covers all core fields, only reg-names and MDSS init need conditional handling. Avoids porting ~1700 lines of downstream SDE code.

- [ ] **Step 0D.3: `dpu_sde_kms_init()` detailed design**

**Design decision on calling timing (self-resolved):**

Two-phase split — `dpu_sde_kms_init()` in kms switch does alloc + basic setup, `dpu_sde_kms_hw_init()` via `kms->funcs->hw_init()` does hardware init. This matches the 5.10 DPU conceptual pattern (alloc/setup vs hw_init) while adapting to the flat SDE reality (no component bind phase for the KMS itself).

**Phase 1: `dpu_sde_kms_init(struct drm_device *ddev)` — called in kms switch**

```c
/**
 * dpu_sde_kms_init - allocate and set up flat SDE KMS backend
 * @ddev: DRM device (master is qcom,sde-kms platform device)
 *
 * Called from msm_drm_init() KMS_SDE switch case. Allocates struct dpu_kms,
 * sets up OPP table and clocks, initializes msm_kms vtable, maps IRQ,
 * and enables pm_runtime. Does NOT map hardware registers — that happens
 * in dpu_sde_kms_hw_init() via kms->funcs->hw_init().
 *
 * Return: &dpu_kms->base on success, ERR_PTR() on failure.
 */
struct msm_kms *dpu_sde_kms_init(struct drm_device *ddev);
```

Responsibilities:
1. `devm_kzalloc(struct dpu_kms)` — reuse 5.10 allocation pattern
2. OPP table setup — `devm_pm_opp_set_clkname()`, `devm_pm_opp_of_add_table()` (reuse `dpu_bind()` pattern)
3. Clocks setup — `msm_clk_get()` (reuse `dpu_bind()` pattern)
4. `msm_kms_init(&dpu_kms->base, &sde_kms_funcs)` — variant vtable with `hw_init = dpu_sde_kms_hw_init`
5. `dpu_kms->dev = ddev`, `dpu_kms->pdev = to_platform_device(ddev->dev)` — flat device is DRM master
6. `priv->kms = &dpu_kms->base` — set early like `dpu_bind()` does
7. `pm_runtime_enable(&pdev->dev)` — enable but don't resume (resume in hw_init)
8. IRQ mapping — `irq_of_parse_and_map(pdev->dev.of_node, 0)`
9. Return `&dpu_kms->base` or `ERR_PTR()` on failure

Error handling: `goto err_disable_xxx` single-exit pattern, `dev_err_probe()` for probe-defer logging.

**Phase 2: `dpu_sde_kms_hw_init(struct msm_kms *kms)` — called via `kms->funcs->hw_init()`**

```c
/**
 * dpu_sde_kms_hw_init - initialize flat SDE KMS hardware
 * @kms: msm_kms pointer (cast to dpu_kms internally)
 *
 * Called by DRM core after dpu_sde_kms_init() succeeds. Maps flat Kona
 * register resources by downstream reg-names, reads core_rev, creates
 * SM8250 catalog, initializes RM/VBIF/perf/intr, creates stub DSI
 * connector with fixed modes, then calls _dpu_kms_drm_obj_init() for
 * plane/CRTC creation.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int dpu_sde_kms_hw_init(struct msm_kms *kms);
```

Responsibilities:
1. `pm_runtime_resume_and_get(&pdev->dev)` — power up hardware
2. ioremap flat reg-names:
   - `msm_ioremap(pdev, "mdp_phys", "mdp_phys")` → `dpu_kms->mmio` [REQUIRED]
   - `msm_ioremap(pdev, "vbif_phys", "vbif_phys")` → `dpu_kms->vbif[VBIF_RT]` [REQUIRED]
   - `msm_ioremap_quiet(pdev, "vbif_nrt_phys")` → `dpu_kms->vbif[VBIF_NRT]` [OPTIONAL]
   - `msm_ioremap_quiet(pdev, "regdma_phys")` → `dpu_kms->reg_dma` [OPTIONAL]
3. ICC path — `dpu_kms_parse_data_bus_icc_path(dpu_kms)` [reuse]
4. `core_rev = readl_relaxed(dpu_kms->mmio)` — hardware revision read
5. `dpu_hw_catalog_init(core_rev)` — reuse 5.10 catalog (expects `DPU_HW_VER_600` for SM8250)
6. `_dpu_kms_mmu_init()` — reuse
7. `dpu_rm_init()` — reuse
8. `dpu_hw_mdptop_init()` — reuse
9. VBIF init — reuse
10. `dpu_core_perf_init()` — reuse
11. `dpu_hw_intr_init()` — reuse
12. **Stub DSI connector creation** (see below)
13. `_dpu_kms_drm_obj_init()` — reuse for plane/CRTC creation (same-file static call)
14. `pm_runtime_put_sync(&pdev->dev)` — power down after init
15. Return 0

**Variant vtable: `sde_kms_funcs`**

```c
static const struct msm_kms_funcs sde_kms_funcs = {
    .hw_init         = dpu_sde_kms_hw_init,    /* ONLY difference */
    .irq_preinstall  = dpu_irq_preinstall,      /* reuse */
    .irq_postinstall = dpu_irq_postinstall,     /* reuse */
    .irq_uninstall   = dpu_irq_uninstall,       /* reuse */
    .irq             = dpu_irq,                 /* reuse */
    .enable_commit   = dpu_kms_enable_commit,   /* reuse */
    .disable_commit  = dpu_kms_disable_commit,  /* reuse */
    .vsync_time      = dpu_kms_vsync_time,      /* reuse */
    .prepare_commit  = dpu_kms_prepare_commit,  /* reuse */
    .flush_commit    = dpu_kms_flush_commit,    /* reuse */
    .wait_flush      = dpu_kms_wait_flush,      /* reuse */
    .complete_commit = dpu_kms_complete_commit, /* reuse */
    .enable_vblank   = dpu_kms_enable_vblank,   /* reuse */
    .disable_vblank  = dpu_kms_disable_vblank,  /* reuse */
    .check_modified_format = dpu_format_check_modified_format, /* reuse */
    .get_format      = dpu_get_msm_format,      /* reuse */
    .round_pixclk    = dpu_kms_round_pixclk,    /* reuse */
    .destroy         = dpu_kms_destroy,          /* reuse */
    .set_encoder_mode = _dpu_kms_set_encoder_mode, /* reuse */
#ifdef CONFIG_DEBUG_FS
    .debugfs_init    = dpu_kms_debugfs_init,    /* reuse */
#endif
};
```

All function pointers identical to `kms_funcs` (L798) except `.hw_init`. Same-file static definition — no export needed.

**Stub DSI connector design:**

For minimal KMS, `drm_client_modeset_create()` needs at least 1 connector + 1 mode. Since placeholder bridge components don't set `priv->dsi[]`, `_dpu_kms_initialize_dsi()` returns 0 without creating encoders. Solution: create stub encoder + connector BEFORE calling `_dpu_kms_drm_obj_init()`.

```c
static int _dpu_sde_kms_setup_stub_connector(struct dpu_kms *dpu_kms)
{
    struct drm_device *dev = dpu_kms->dev;
    struct msm_drm_private *priv = dev->dev_private;
    struct drm_encoder *encoder;
    struct drm_connector *connector;
    int ret;

    /* 1. Stub DSI encoder */
    encoder = dpu_encoder_init(dev, DRM_MODE_ENCODER_DSI);
    if (IS_ERR(encoder))
        return PTR_ERR(encoder);
    priv->encoders[priv->num_encoders++] = encoder;

    /* 2. Stub DSI connector with fixed modes */
    connector = devm_kzalloc(dev->dev, sizeof(*connector), GFP_KERNEL);
    ret = drm_connector_init(dev, connector, &sde_stub_connector_funcs,
                             DRM_MODE_CONNECTOR_DSI);
    drm_connector_register(connector);

    /* OnePlus 8/8T: 1080x2400@60Hz */
    drm_mode_probed_add(connector, sde_stub_fixed_mode(dev, 1080, 2400, 60));

    /* OnePlus 8 Pro: 1440x3120@60Hz */
    drm_mode_probed_add(connector, sde_stub_fixed_mode(dev, 1440, 3120, 60));

    connector->status = connector_status_connected;
    drm_connector_attach_encoder(connector, encoder);
    priv->connectors[priv->num_connectors++] = connector;

    return 0;
}
```

Then in `dpu_sde_kms_hw_init()`:
1. Call `_dpu_sde_kms_setup_stub_connector()` — creates 1 encoder + 1 connector
2. Call `_dpu_kms_drm_obj_init()` — `_dpu_kms_setup_displays()` finds no `priv->dsi[]`, adds 0 encoders; then creates planes + 1 CRTC (min(mixer_count, num_encoders=1)); sets `possible_crtcs` on stub encoder

**msm_drv.c modification:**

```c
/* In kms init switch, replace: */
case KMS_SDE:
    kms = NULL;
    break;
/* With: */
case KMS_SDE:
    kms = dpu_sde_kms_init(ddev);
    break;
```

No other msm_drv.c changes needed. MDSS init switch remains `case KMS_SDE: ret = 0; break;`.

**What we explicitly DO NOT do:**
- Do not modify upstream `dpu_kms_hw_init()` — keep it untouched
- Do not add `sid/imem` fields to `struct dpu_kms` — minimal KMS doesn't need them
- Do not port downstream DSI display array mechanism — use stub connector
- Do not modify `include/linux/` headers
- Do not export `kms_funcs` or `_dpu_kms_drm_obj_init()` — same-file static access only

- [x] **Step 0D.4: Implement the flat KMS skeleton** — DONE 2026-05-31

Patch scope limited to:
- `drivers/gpu/drm/msm/msm_drv.c` — replace `kms = NULL` with `kms = dpu_sde_kms_init(ddev)`
- `drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c` — add `dpu_sde_kms_init()`, `dpu_sde_kms_hw_init()`, `sde_kms_funcs`, `_dpu_sde_kms_setup_stub_connector()`, `sde_stub_connector_funcs`, `sde_stub_fixed_mode()`
- `drivers/gpu/drm/msm/disp/dpu1/dpu_kms.h` — add `dpu_sde_kms_init()` declaration

Do not port AOD, HBM, DC dimming, SEED, CABC, on-screen fingerprint, DP, WB, or RSC in this task.

Expected first success criterion: `qcom,sde-kms: running without KMS backend` disappears and the new flat KMS init path logs its resource mapping result. `drm_client_modeset_create()` succeeds for `oplus_screenlog`.

- [ ] **Step 0D.5: Bring up one real connector path only**

After the KMS skeleton maps registers safely and stub connector works, prioritize DSI0 only:
- source reference: `/root/oneplus-sm8250-kernel/techpack/display/msm/dsi/dsi_display.c`
- target reference: `/root/ye3912-5.10/drivers/gpu/drm/msm/dsi/`

First runtime target:

```text
/sys/class/drm/card0-DSI-1 exists
oplus_screenlog renders to physical panel
```

Replace stub connector with real DSI bridge when DSI0 driver is ready. Do not start DP, WB, RSC, AOD/HBM/DC dimming, or panel feature parity before DSI0 can create a modeset.

- [ ] **Step 0D.6: Static verification before every push**

Allowed local commands only:

```bash
git diff --check -- drivers/gpu/drm/msm/msm_drv.c drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c drivers/gpu/drm/msm/disp/dpu1/dpu_kms.h
git diff -- drivers/gpu/drm/msm/msm_drv.c drivers/gpu/drm/msm/disp/dpu1/dpu_kms.c drivers/gpu/drm/msm/disp/dpu1/dpu_kms.h | perl scripts/checkpatch.pl --strict --no-tree -
git diff --name-only
git status --short --branch
```

Forbidden locally:

```bash
make
```

Expected: whitespace clean, checkpatch diff clean, only intended display files modified unless a later approved subtask expands scope.

## Decision: Next Migration Target

The next migration target is **OPLUS SoC base layer**, not charging directly.

Reason: charging, touchscreen, fingerprint, display feature code commonly depends on project ID, boot mode, device info, board identification, and OPLUS common headers. Porting charging first without a stable SoC base would force scattered stubs and make later regressions harder to isolate.

## Priority Order

| Priority | Subsystem | Status | Why next / why later |
|---|---|---|---|
| P0 | OPLUS SoC base | Partial | Required by charging/touch/fingerprint/display feature logic |
| P0 | Charging minimal closure | Partial | Battery/USB state can block Android userspace progress |
| P1 | Touchscreen base | Missing | Needed after userspace/display progresses far enough for interaction |
| P2 | Display OPLUS features | Missing | Touch/FP UX depends on panel events, but DRM risk is high |
| P2 | Fingerprint secure/input | Missing | Depends on touch/display/secure path readiness |
| P3 | Camera | Missing | Very large surface; not boot-critical |
| P4 | Filesystem/network OPLUS | Minimal/missing | Not the first blocker for boot-to-UI |

---

## Task 0: Gate on Current Debug CI and Boot Evidence

**Files:**
- Read only: GitHub Actions result for latest push
- Runtime evidence expected from device: screen log, `oplus_logbuf`, panic/hung output if available

- [ ] **Step 0.1: Confirm CI for `469f02198`**

Expected result: GitHub Actions build for commit `469f02198` is green. If red, stop this migration plan and fix CI first.

- [ ] **Step 0.2: Flash only after CI is green**

Expected result: device either still hangs with visible `oplus_screenlog`, or progresses further. Record the last visible subsystem/function before hang.

- [ ] **Step 0.3: Decide whether boot-hang root cause blocks subsystem migration**

If the hang is clearly inside an already touched debug path, fix debug first. If the hang is in a missing OPLUS dependency such as power, project, boot mode, or input, continue to Task 1.

---

## Task 1: OPLUS SoC Base Layer Audit

**Files:**
- Compare source: `/root/oneplus-sm8250-kernel/drivers/soc/oplus/`
- Compare source: `/root/oneplus-sm8250-kernel/include/soc/oplus/`
- Compare target: `/root/ye3912-5.10/drivers/soc/oplus/`
- Compare target: `/root/ye3912-5.10/include/soc/oplus/`
- Output report: `docs/superpowers/plans/2026-05-30-sm8250-soc-base-audit.md`

- [ ] **Step 1.1: Inventory source and target SoC files**

Use dedicated file/search tools first. Do not use local compile commands.

Expected output categories:
- already ported and usable
- missing but required by P0/P1/P2 subsystems
- 4.19-only legacy code that should be stubbed or dropped
- unsafe core-kernel dependency that must be replaced by vendor_hook or standard 5.10 API

- [ ] **Step 1.2: Identify exported interfaces used by charging and touch**

Search source tree references to OPLUS SoC symbols from:
- `/root/oneplus-sm8250-kernel/drivers/power/oplus/`
- `/root/oneplus-sm8250-kernel/drivers/input/touchscreen/oplus_touchscreen/`
- `/root/oneplus-sm8250-kernel/drivers/input/oplus_fp_drivers/`

Expected output: symbol dependency table with provider file, consumer subsystem, and 5.10 migration action.

- [ ] **Step 1.3: Choose minimal SoC base patch scope**

Patch scope must be limited to interfaces required by P0/P1. Do not port unrelated factory/test/debug modules unless a consumer requires them.

- [ ] **Step 1.4: Get user confirmation before modifying code**

Before edits, present exact file list and risk level. Do not implement until confirmed.

---

## Task 2: OPLUS SoC Base Minimal Patch

**Files:**
- Likely modify: `drivers/soc/oplus/Kconfig`
- Likely modify: `drivers/soc/oplus/Makefile`
- Likely modify/create under: `drivers/soc/oplus/`
- Likely modify/create under: `include/soc/oplus/`
- Likely modify: `arch/arm64/configs/gki_defconfig`
- Likely modify: `arch/arm64/configs/sm8250-oneplus8pro_defconfig`

- [ ] **Step 2.1: Port only confirmed base providers**

Use 5.10-native APIs. Do not add `#if LINUX_VERSION_CODE`. New shared symbols must be `EXPORT_SYMBOL_GPL` only if a module consumer requires them.

- [ ] **Step 2.2: Stub non-critical factory/debug behavior explicitly**

If a required interface has no safe 5.10 implementation yet, add a conservative stub that returns a documented safe value and logs once where appropriate. Do not fake hardware state.

- [ ] **Step 2.3: Static verification**

Run only allowed local static checks:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 2.4: Commit and push only after static check passes**

Commit message format:

```text
soc: oplus: add minimal SM8250 base providers
```

Expected CI: GitHub Actions build passes.

---

## Task 3: Charging Minimal Closure Plan

**Files:**
- Source: `/root/oneplus-sm8250-kernel/drivers/power/oplus/`
- Target: `/root/ye3912-5.10/drivers/power/oplus/`
- Target configs: `arch/arm64/configs/gki_defconfig`, `arch/arm64/configs/sm8250-oneplus8pro_defconfig`

- [ ] **Step 3.1: Do not bulk-copy 524 files**

Start with a minimal power_supply path:
- battery psy
- usb psy
- charger/gauge abstraction sufficient for boot
- VOOC/SuperVOOC disabled or stubbed until normal charging works

- [ ] **Step 3.2: Identify HAL-visible properties**

Minimum properties expected for Android health/battery service:
- capacity
- status
- health
- voltage_now
- current_now if available
- usb online/type where available

- [ ] **Step 3.3: Avoid blocking probe**

Probe must not wait indefinitely for charger IC, gauge IC, or VOOC firmware. Use deferred probe only for real missing dependencies and make all waits bounded.

- [ ] **Step 3.4: CI gate before runtime test**

No local compile. Use `git diff --check`, commit, push, and wait for GitHub Actions.

---

## Task 4: Touchscreen Base Framework

**Files:**
- Source: `/root/oneplus-sm8250-kernel/drivers/input/touchscreen/oplus_touchscreen/`
- Target likely create: `/root/ye3912-5.10/drivers/input/touchscreen/oplus_touchscreen/`
- Target likely modify: `drivers/input/touchscreen/Kconfig`
- Target likely modify: `drivers/input/touchscreen/Makefile`

- [ ] **Step 4.1: Port common touch framework before individual IC features**

Bring up reset, pinctrl, IRQ, input device registration, and event reporting first.

- [ ] **Step 4.2: Convert obsolete 4.19 primitives**

Required conversions:
- `tasklet_init()` → threaded IRQ or `tasklet_setup()` only if truly necessary
- old timer setup → `timer_setup()`
- old proc file operations → `struct proc_ops`

- [ ] **Step 4.3: Runtime target**

First success criterion is input events visible to Android, not full gesture/factory mode parity.

---

## Task 5: Display OPLUS Feature Layer

**Files:**
- Target: `drivers/gpu/drm/msm/`
- Target: panel DTS/DTSI under `arch/arm64/boot/dts/qcom/`

- [ ] **Step 5.1: Do not overwrite 5.10 DPU with 4.19 SDE code**

Migrate feature deltas only: AOD, HBM, DC dimming, ADFR, CABC, SEED, on-screen fingerprint hooks.

- [ ] **Step 5.2: Prefer DRM panel / DPU extension points**

Avoid direct core DRM ABI changes. If a hook is unavoidable, isolate it behind OPLUS config and document why vendor_hook or existing panel callback is insufficient.

- [ ] **Step 5.3: Keep debug screenlog separate**

`oplus_screenlog` is a boot debug tool. Do not build normal display feature logic on top of it.

---

## Task 6: Deferred Subsystems

**Files:**
- Fingerprint: `/root/oneplus-sm8250-kernel/drivers/input/oplus_fp_drivers/`
- Camera: `/root/oneplus-sm8250-kernel/techpack/`
- Filesystem: `/root/oneplus-sm8250-kernel/fs/`
- Network: `/root/oneplus-sm8250-kernel/net/oplus/`

- [ ] **Step 6.1: Fingerprint after touch/display**

Do not start fingerprint until touch IRQ/input and display panel events are stable.

- [ ] **Step 6.2: Camera after boot-to-UI**

Camera is too large for the current boot stabilization phase. Start only after basic UI boot and power stack are stable.

- [ ] **Step 6.3: Filesystem/network last**

Only port OPLUS filesystem/network changes if runtime evidence shows they are required for target ROM behavior.

---

## Verification Policy

- Local compile: forbidden by user instruction.
- Required before every push: `git diff --check`.
- Required after every push: GitHub Actions CI result.
- Required before runtime claims: device evidence from screen log, `oplus_logbuf`, dmesg, or Android-visible behavior.
- Never claim “fixed” based only on code review or intent.

## Immediate Next Action

Start **Task 0D.4**: implement the flat KMS skeleton based on the 0D.3 design. Files to modify: `msm_drv.c`, `dpu_kms.c`, `dpu_kms.h`. Run `git diff --check` before every push. Do not run local kernel builds.
