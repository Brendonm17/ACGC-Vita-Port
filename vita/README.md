# Animal Crossing - PS Vita Port

Port of [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) to PS Vita using VitaGL.

Supports widescreen.
Multiple banners for use in 4:3 mode.
Multiple texture packs (visual artifacts, HD packs may cause VRAM thrashing, experimental).
MSAA (poor performance, experimental).
Animal island enabled (no gameboy connection required).

Project is full of issues, currently in alpha release state.
AI used in initial upstream project, and here for tool creation and code analysis.

## Requirements

- PS Vita running HENkaku/Enso
- Game ISO: `Animal Crossing (USA).iso` placed at `ux0:data/AnimalCrossing/rom/`
- VitaCompanion plugin (optional, for fast deploy)

### Build tools

- [VitaSDK](https://vitasdk.org/)
- WSL or Linux (building on NTFS is extremely slow)
- ccache (optional, speeds up rebuilds)
- Python 3 (for shader header generation)

### VitaGL

Build vitaGL with the patches this port needs:

```
bash vita/rebuild_vitagl.sh
```

## Building

All build scripts live in `vita/`. They sync source to ext4 first because NTFS I/O from WSL is too slow.

```bash
# full pipeline (shaders + build + VPK)
bash vita/full_rebuild.sh

# fast iteration: build + deploy eboot.bin to Vita via FTP
bash vita/deploy.sh

# incremental build only
bash vita/build_vita.sh
```

Override defaults:
```bash
export WIN_SRC="/path/to/ACGC-PC-Port"
export EXT4_SRC="$HOME/ac_vita_build/ACGC-PC-Port"
export VITA_IP="192.168.1.100"
```

### Deploy

`deploy.sh` uses VitaCompanion to kill the app, FTP the new eboot.bin, and relaunch.

```bash
bash vita/deploy.sh          # build + deploy
bash vita/deploy.sh full     # build + upload full VPK
bash vita/deploy.sh deploy   # skip build, just deploy
bash vita/deploy.sh kill     # kill running app
bash vita/deploy.sh launch   # launch app
```

## Settings

Stored at `ux0:data/AnimalCrossing/settings.ini`, created on first run.

| Setting | Default | Values | Notes |
|---------|---------|--------|-------|
| render_scale | 100 | 50, 75, 100 | 100 = native 960x544, 50 = PSP res |
| msaa | 2 | 0, 2, 4 | anti-aliasing samples |
| aspect_mode | 0 | 0, 1 | 0 = 16:9 widescreen, 1 = 4:3 pillarbox |
| banner | | filename | PNG in banners/ for pillarbox bars |
| multithread | 1 | 0, 1 | emu64 worker thread on core 1, experimental |
| texture_pack | | name | VTC filename without extension |

### Banners

Place PNG files in `ux0:data/AnimalCrossing/banners/`. Set `banner=filename` (without .png) in settings. Only visible in 4:3 mode.

## HD Texture Packs

Supports Dolphin HD texture packs converted to VTC format for hardware DXT decompression.
Currently very experimental, texture packs have issues with alpha textures and slight bounds issues.

### Building a pack

1. Get a Dolphin HD texture pack (DDS with BC7/BC3/BC1)
2. Convert:
   ```
   python tools/build_vita_texcache.py <pack_dir> vita_texcache.vtc
   ```
3. Copy `.vtc` to `ux0:data/AnimalCrossing/texture_packs/`
4. Set `texture_pack=vita_texcache` in settings.ini

The converter re-encodes to DXT1/DXT5. Square power-of-two textures get PVRTC for extra compression.

Requires Python packages: `texture2ddecoder`, `etcpak`. Optional: PVRTexTool CLI for PVRTC (`PVRTEXTOOL_CLI` env var).

## Shaders

Pre-compiled CG shaders (`.cg` to `.gxp` via psp2cgc), embedded as C arrays in `vita_gxp_shaders.h`.

To recompile after editing:
```bash
cd vita/shaders
bash compile_new_shaders.sh
python3 gen_header.py
```

## Debugging

Errors go to `ux0:data/AnimalCrossing/error.log`.

For verbose logging, add `VITA_DEBUG` back to the defines in `vita/CMakeLists.txt` and rebuild. Performance and texture diagnostics will write to `ac_perf.txt`.

```bash
curl -s ftp://VITA_IP:1337/ux0:/data/AnimalCrossing/error.log
```
