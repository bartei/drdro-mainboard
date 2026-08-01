#
# PlatformIO extra script: also emit an Intel HEX next to the .elf/.bin.
# (ststm32 produces .elf + .bin by default; .hex is handy for STM32CubeProgrammer
# and is published as a release artifact.)
#
Import("env")

env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(
        "$OBJCOPY -O ihex $BUILD_DIR/${PROGNAME}.elf $BUILD_DIR/${PROGNAME}.hex",
        "Building $BUILD_DIR/${PROGNAME}.hex",
    ),
)
