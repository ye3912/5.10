# SPDX-License-Identifier: GPL-2.0
# OnePlus SM8250 DTBO overlays.
#
# scripts/Makefile.dtbo expects overlay sources as $(src)/%.dts and
# emits $(obj)/%.dtbo when CONFIG_BUILD_ARM64_DT_OVERLAY=y.  Keep this
# file declarative so O= out-of-tree builds use the source directory
# correctly instead of depending on generated $(obj)/%.dtso files.

dtbo-$(CONFIG_ARCH_KONA) += kona-kebab.dtbo

kona-kebab.dtbo-base := ../qcom/sm8250-oneplus8t.dtb

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
