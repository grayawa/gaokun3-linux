# Linux 7.2.15-bore source-priority port

Pinned source: https://github.com/gaokun3/kernel/tree/7.2.15-bore
Source commit: 
Destination base: 
Synthetic common base: Linux .

## Merge policy

This candidate prefers the complete upstream 7.2.15-bore implementation for
overlapping hunks (, with upstream as the first side).
Non-overlapping local changes are retained. This avoids the mixed-version
UCSI and battery implementations produced by the earlier local-priority draft.

The following local changes were explicitly restored:
- gaokun3 I2C-HID touchscreen and dual-Himax touchscreen description
- EC wakeup-source property (paired with the local EC suspend/IRQ quiescing code)
- CONFIG_SCHED_BORE and the requested KPROBES/FTRACE/KPROBE_EVENTS/TRACING/TRACEFS_FS options

## Manual review still required

The initial strict merge reported these overlapping paths:
- `arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3-camera.dtsi`
- `arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts`
- `arch/arm64/boot/dts/qcom/sc8280xp.dtsi`
- `arch/arm64/configs/gaokun3_defconfig`
- `drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c`
- `drivers/gpu/drm/msm/disp/dpu1/dpu_encoder_phys_vid.c`
- `drivers/media/i2c/hi846.c`
- `drivers/media/platform/qcom/iris/iris_hfi_common.c`
- `drivers/media/platform/qcom/iris/iris_hfi_gen1_command.c`
- `drivers/net/wireless/ath/ath11k/core.c`
- `drivers/net/wireless/ath/ath11k/core.h`
- `drivers/net/wireless/ath/ath11k/debugfs.c`
- `drivers/net/wireless/ath/ath11k/dp_rx.c`
- `drivers/net/wireless/ath/ath11k/mac.c`
- `drivers/net/wireless/ath/ath11k/mhi.c`
- `drivers/net/wireless/ath/ath11k/wow.c`
- `drivers/platform/arm64/huawei-gaokun-ec.c`
- `drivers/power/supply/huawei-gaokun-battery.c`
- `drivers/usb/typec/ucsi/ucsi_huawei_gaokun.c`
- `net/mac80211/pm.c`

This is a build candidate, not a validated device release. Review the conflict
paths, complete the kernel/DTB build, and test suspend, touchscreen, cameras,
Wi-Fi, USB-C/DP altmode, battery, and FastRPC on hardware before merging.

Assisted by ChatGPT.
