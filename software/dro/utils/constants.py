"""Hardware-shape constants shared across the dispatcher layer."""

# The V1.5 mainboard has 5 encoder scale inputs (firmware registry arrays are 5 wide). This is
# the default/initial input count; the live count is read from the board's `scales.count` on
# connect and the input list is resized to match (see Board._apply_scale_count).
SCALES_COUNT = 5
SERVOS_COUNT = 3
