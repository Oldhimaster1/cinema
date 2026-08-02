# Build status

The feature source passes host structural compilation, strict warnings, simulations, and UBSan. The authentic CE Toolchain is not installed in the execution environment, so no genuine `.8xp` could be produced here. Do not rename a host binary to `.8xp`; build with CEdev using `make`, then preserve the resulting `bin/CINEMA.8xp` hash before emulator or hardware testing.
