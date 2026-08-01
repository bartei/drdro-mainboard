#
# PlatformIO extra script: enable Cortex-M4F hard-float across the whole build.
#
# The `genericSTM32F411CE` board + `stm32cube` framework only set
# `-mthumb -mcpu=cortex-m4` (no FPU), so the build defaults to soft-float.
# FreeRTOS port.c (configENABLE_FPU=1) emits hard-FPU asm (vstmdb {s16-s31}),
# which the assembler rejects without -mfpu. The original CMake project built
# hard-float on BOTH compile and link, so the produced ABI / libgcc multilib
# match. PlatformIO routes bare `build_flags` only to compilation, never to the
# linker, so we append the FPU flags to every stage here for a single, uniform
# ABI (framework HAL + vendored libs + app + libgcc/libm).
#
Import("env")

fpu_flags = ["-mfloat-abi=hard", "-mfpu=fpv4-sp-d16"]

env.Append(
    ASFLAGS=fpu_flags,
    ASPPFLAGS=fpu_flags,
    CCFLAGS=fpu_flags,
    LINKFLAGS=fpu_flags,
)
