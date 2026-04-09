# Animal Crossing - PS Vita Port

Port of [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) to PS Vita using VitaGL.

## Requirements

- PS Vita running HENkaku/Enso with extended memory plugin
- Game ISO: `Animal Crossing (USA).iso` placed at `ux0:data/AnimalCrossing/rom/`
- VitaCompanion plugin (for fast deploy workflow)

### Build Tools

- [VitaSDK](https://vitasdk.org/)
- WSL or Linux (NTFS build is painfully slow)
- ccache (optional, speeds up rebuilds)
- psp2cgc (shader compiler, in VitaSDK)
- Python 3 (for shader header generation)

### VitaGL

This port depends on a fork of vitaGL ([Brendonm17/vitaGL](https://github.com/Brendonm17/vitaGL), branch `async-compressed-tex-prep`) that adds `vglPrepareCompressedTexture2D` / `vglCommitPendingTexture` for off-thread block-compressed texture uploads. Without it, HD texture loads stall the render thread with multi-millisecond swizzle spikes.

Build and install it with:

```
bash vita/rebuild_vitagl.sh
```

This clones the fork, applies the CG row-major matrix-multiply fix, compiles with all speedhack flags, and installs `libvitaGL.a` and `vitaGL.h` into `$VITASDK/arm-vita-eabi/`. Run it once after cloning the repo (and any time you want to refresh the fork).

## Building

All build scripts live in `vita/`. They sync source to an ext4 filesystem first because NTFS I/O from WSL is 10-50x slower.

```bash
# first time: full pipeline (shaders + build + VPK)
bash vita/full_rebuild.sh

# fast iteration: build + deploy eboot.bin to Vita
bash vita/deploy.sh

# build only (no deploy)
bash vita/build.sh

# just rebuild (already on ext4)
bash vita/build_vita.sh
```

Override paths via environment variables if needed:
```bash
export WIN_SRC="/path/to/ACGC-PC-Port"
export EXT4_SRC="$HOME/ac_vita_build/ACGC-PC-Port"
export VITA_IP="192.168.1.100"
```

### Deploy

`deploy.sh` uses VitaCompanion to kill the app, FTP upload `eboot.bin`, and relaunch. Much faster than reinstalling the full VPK every time.

```bash
bash vita/deploy.sh          # build + deploy
bash vita/deploy.sh full     # build + upload full VPK (first install)
bash vita/deploy.sh deploy   # deploy only (already built)
bash vita/deploy.sh kill     # kill running app
bash vita/deploy.sh launch   # launch app
```

## Settings

Settings are stored at `ux0:data/AnimalCrossing/settings.ini`. Created with defaults on first run.

| Setting | Default | Values | Description |
|---------|---------|--------|-------------|
| `render_scale` | 100 | 50, 75, 100 | Resolution scale (100 = 960x544) |
| `msaa` | 2 | 0, 2, 4 | Anti-aliasing |
| `aspect_mode` | 0 | 0, 1 | 0 = 16:9 widescreen, 1 = 4:3 with pillarbox |
| `banner` | *(empty)* | filename | PNG banner for 4:3 pillarbox bars |
| `multithread` | 1 | 0, 1 | Worker thread for emu64 (keep on for performance) |
| `texture_pack` | *(empty)* | name | HD texture pack name (without .vtc) |

### Resolution

- 100% = 960x544 (native, best quality)
- 75% = 720x408 (good balance)
- 50% = 480x272 (PSP resolution, best performance)

### Banners

Place PNG files in `ux0:data/AnimalCrossing/banners/`. Set `banner=filename` (without .png) in settings. Only used in 4:3 aspect mode.

## HD Texture Packs

The Vita port supports Dolphin-format HD texture packs converted to VTC format for hardware-accelerated decompression.

### Building a Texture Pack

1. Get a Dolphin HD texture pack (DDS files with BC7/BC3/BC1 compression)
2. Run the converter:
   ```
   python tools/build_vita_texcache.py <texture_pack_dir> vita_texcache.vtc
   ```
3. Copy the `.vtc` file to `ux0:data/AnimalCrossing/texture_packs/` on the Vita
4. Set `texture_pack=vita_texcache` in settings.ini (name without .vtc) or using in-game options menu

The converter re-encodes textures as DXT1 (opaque) or DXT5 (alpha) for hardware-accelerated decompression on the Vita GPU.

### Texture Pack Requirements

- Python packages: `texture2ddecoder`, `etcpak`

## Shaders

Shaders are pre-compiled CG programs (`.cg` source -> `.gxp` binary via psp2cgc). The compiled binaries are embedded in `vita_gxp_shaders.h` as C arrays.

To recompile after editing shader source:

```bash
cd vita/shaders
bash compile_new_shaders.sh
python3 gen_header.py
```

27 specialized shader configs handle common TEV combiner setups. An uber shader covers the rest.

## Directory Layout

```
vita/
  src/              # Vita platform layer
  include/          # Vita headers
  shaders/          # CG shader source + compile scripts
  sce_sys/          # LiveArea assets (icon, background)
  CMakeLists.txt    # Build config
  *.sh              # Build/deploy scripts
pc/
  src/              # Shared PC port layer (reused by Vita)
  include/          # Shared headers
tools/
  build_vita_texcache.py   # HD texture pack converter
```

## Debugging

Errors always go to `ux0:data/AnimalCrossing/error.log` (stderr redirect).

For verbose debug output, uncomment `VITA_DEBUG` in `vita/CMakeLists.txt` and rebuild. This writes performance stats and texture cache diagnostics to `ux0:data/AnimalCrossing/debug.log`.

Read logs via FTP:
```bash
curl -s ftp://VITA_IP:1337/ux0:/data/AnimalCrossing/error.log
curl -s ftp://VITA_IP:1337/ux0:/data/AnimalCrossing/debug.log
```
