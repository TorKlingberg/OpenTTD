# Building OpenTTD on macOS 26.2

This guide covers building OpenTTD from source on macOS 26.2, including workarounds for platform-specific issues.

## Prerequisites

### Required Tools
- Xcode Command Line Tools
- Homebrew package manager
- Git

### Required Dependencies

Install via Homebrew:
```bash
brew install lzo pkg-config libpng xz
```

These provide:
- `lzo` - LZO compression library
- `pkg-config` - Helper tool for compiling
- `libpng` - PNG image library
- `xz` - LZMA compression library (provides liblzma)

## Getting the Source Code

Clone the OpenTTD repository:
```bash
git clone https://github.com/OpenTTD/OpenTTD.git
cd OpenTTD
```

## Platform-Specific Fixes

### macOS Atomic Library Fix

On macOS, atomics are built-in to the compiler and don't require libatomic. Apply this fix to `cmake/3rdparty/llvm/CheckAtomic.cmake`:

Change line 52 from:
```cmake
if(MSVC)
```
to:
```cmake
if(MSVC OR APPLE)
```

And change line 75 from:
```cmake
if(MSVC)
```
to:
```cmake
if(MSVC OR APPLE)
```

This prevents CMake from trying to link against the non-existent libatomic library on macOS.

## Building

### Configure with CMake

Create a build directory and configure:
```bash
mkdir build
cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

**Note:** The explicit C++ include paths are needed on macOS 26.2 due to SDK header discovery issues. Adjust the SDK path if using a different version.

### Compile

Build using all available CPU cores (replace 8 with your core count):
```bash
make -j8
```

The build will take several minutes and produce:
- `openttd` - The main executable
- `openttd_test` - Test executable

### Sign the Executable

On macOS, you need to ad-hoc sign the binary:
```bash
codesign -s - --deep --force openttd
```

## Setting Up Game Data

OpenTTD requires original Transport Tycoon Deluxe data files or free alternatives.

### Using Original TTD Files

If you have the original game, copy these files to the `baseset/` directory in your build folder:
- `TRG1.GRF` (or `trg1r.grf`)
- `TRGC.GRF` (or `trgcr.grf`)
- `TRGH.GRF` (or `trghr.grf`)
- `TRGI.GRF` (or `trgir.grf`)
- `TRGT.GRF` (or `trgtr.grf`)
- `SAMPLE.CAT` (or `sample.cat`)

Example:
```bash
cp /path/to/ttd/*.GRF baseset/
cp /path/to/ttd/SAMPLE.CAT baseset/
```

### Using Free Graphics (Alternative)

You can download free graphics sets from:
- OpenGFX: https://www.openttd.org/downloads/opengfx-releases/latest
- OpenSFX: https://www.openttd.org/downloads/opensfx-releases/latest

## Running OpenTTD

From the build directory:
```bash
./openttd
```

### Useful Command-Line Options

- `-f` - Run in fullscreen mode
- `-r WIDTHxHEIGHT` - Set resolution (e.g., `-r 1920x1080`)
- `-g savegame.sav` - Load a specific savegame
- `-h` - Show help and all available options

Example:
```bash
./openttd -f -r 1920x1080
```

## Configuration Files

OpenTTD creates configuration and save files in:
- Config: `~/Documents/OpenTTD/openttd.cfg`
- Saves: `~/Documents/OpenTTD/save/`
- Screenshots: `~/Documents/OpenTTD/screenshot/`

## Troubleshooting

### Build fails with "algorithm file not found"

This happens if the C++ include paths aren't set correctly. Make sure you're using the full cmake command with `-DCMAKE_CXX_FLAGS` and `-DCMAKE_OBJCXX_FLAGS` as shown above.

### Build fails with "cannot find libatomic"

Apply the atomic library fix to `CheckAtomic.cmake` as described in the Platform-Specific Fixes section.

### Game won't start - missing graphics

Ensure you have either the original TTD data files or OpenGFX installed in the `baseset/` directory.

## Building for Development

For development builds with debug symbols and no optimization:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1" \
  -DCMAKE_OBJCXX_FLAGS="-I/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk/usr/include/c++/v1"
```

## OpenTTD Coordinate System

OpenTTD uses an isometric coordinate system where tiles are arranged in a diamond grid. **See `coords.md` for detailed explanation.**

### Map Coordinates
-   **(X, Y)**: Integer tile coordinates, both starting at 0
-   **Isometric projection**: The rectangular grid appears rotated 45° as a diamond on screen
-   **Orthogonal movement**: Changes only one coordinate at a time

### Map Corners (for 100×100 map)
-   **(0, 0)**: Top corner of diamond
-   **(99, 0)**: Right corner of diamond (max X)
-   **(0, 99)**: Left corner of diamond (max Y)
-   **(99, 99)**: Bottom corner of diamond

### How Coordinates Map to Screen Directions

Based on empirical testing (see `coords.md`):

| Coordinate Change | Viewport Appearance |
|-------------------|---------------------|
| X increases (+1, 0) | Right-ish diagonal |
| Y increases (0, +1) | Down-right diagonal |
| X decreases (-1, 0) | Left-ish diagonal |
| Y decreases (0, -1) | Up-left diagonal |

**Important:** Don't use compass directions (N/S/E/W) for viewport appearance - use screen-relative terms (up/down/left/right) to avoid confusion. The isometric view makes all coordinate-aligned movements appear as diagonals on screen.

Each tile is 16×16 units in game coordinates (`TILE_SIZE`).

## Additional Resources

- Official compilation guide: `COMPILING.md` in the repository
- OpenTTD Wiki: https://wiki.openttd.org/
- GitHub Issues: https://github.com/OpenTTD/OpenTTD/issues
