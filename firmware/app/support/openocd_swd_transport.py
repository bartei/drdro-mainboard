#
# PlatformIO extra script: fix the ST-Link transport for modern OpenOCD.
#
# ststm32@19.4.0 hardcodes `transport select hla_swd` for ST-Link uploads
# (platform.py, _add_default_debug_tools), but the OpenOCD that same platform
# pins (tool-openocd@~3.1200.0 == OpenOCD 0.12) removed the legacy HLA adapter
# driver: interface/stlink.cfg now selects the `st-link` driver, which provides
# dapdirect_swd rather than hla_swd. The stock upload therefore aborts with
#
#     Debug adapter doesn't support 'hla_swd' transport
#
# Unpinning the platform would fix it upstream but would also move the
# HAL/CMSIS/toolchain versions off the ones the drdro-firmware-f4 baseline was
# validated against, so instead we keep the pin and rewrite just this one flag.
#
# Only the transport string is touched — the interface/target cfgs, the
# `program ... verify reset` sequence and the resolved openocd path all stay
# exactly as the platform generated them. No-op if a future platform bump
# already emits a working transport.
#
Import("env")

_BROKEN = "transport select hla_swd"
_FIXED = "transport select swd"

flags = env.get("UPLOADERFLAGS") or []

if _BROKEN in flags:
    env.Replace(UPLOADERFLAGS=[_FIXED if f == _BROKEN else f for f in flags])
    print("openocd_swd_transport: rewrote '%s' -> '%s'" % (_BROKEN, _FIXED))
