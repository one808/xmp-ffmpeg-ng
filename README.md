# xmp-ffmpeg

XMPlay input plugin that plays **anything FFmpeg can decode** — all audio
formats, plus the **audio track of any video file** (video is skipped,
audio-only playback). Powered by a fully **static** FFmpeg build, so the DLL
has zero FFmpeg dependencies — just drop it in and go.

Built for **Windows XP and later** (including old CPUs without SSE2).

## Features

- **All formats**: 231 audio decoders (incl. PCM family), 367 demuxers,
  68 parsers — MP3/AAC/M4A/FLAC/WAV/WMA/APE/WavPack/TTA/TAK/MPC/Opus/Vorbis/
  AC3/E-AC3/DTS/TrueHD/MLP/AMR/GSM/ATRAC/QDM2/shorten/… the full FFmpeg
  audio decoding surface, plus game music (NSF/SPC/VGM… via libgme),
  codec2, and high-quality AAC (libfdk-aac).
- **Video files play their audio**: MKV/MP4/AVI/MOV/WMV/FLV/WebM/TS/M2TS/VOB/
  3GP/RM/… demuxed and the audio stream decoded — the video stream is never
  touched (fast even on an Athlon XP).
- **Network streaming** through XMPlay's own file routines (HTTP/ICY/etc.),
  archive support — anything XMPlay can open, this plugin can decode.
- **Gapless-ish seeking** (millisecond granularity), full tag support
  (title/artist/album/date/track/genre/comment).
- Static build: no FFmpeg DLLs, no runtime redistributables to install.

## Installation

Copy `xmp-ffmpeg.dll` into XMPlay's folder (or its `plugins` folder) and
restart XMPlay.

The plugin claims a large extension list (audio + video groups). If you use
other input plugins for specific formats (e.g. a dedicated FLAC plugin), keep
only one of them enabled — XMPlay's plugin config dialog lets you untick
formats. Module formats (MOD/XM/S3M/IT…) are deliberately **not** claimed:
XMPlay plays those natively, and better.

## Building

Everything is built with GitHub Actions (`.github/workflows/build.yml`) —
on every push, or manually via *Run workflow*:

1. **Cross toolchain** (i686 MinGW-w64 9.0 + GCC 10.4, win32 threads +
   winpthreads) built with the recipe from
   [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers)
   (pinned checkout, `xp-core2` branch) — the same toolchain that produces
   our known-good "FFmpeg for XP" builds. Cached after the first run.
2. **External codec libs** (same pinned commits as our XP FFmpeg builds):
   libfdk-aac, libcodec2, opencore-amr, game-music-emu + zlib/bzip2/lzma.
3. **FFmpeg** (pinned `n9.0.2`), configured decode-only:
   all demuxers/parsers/BSFs, audio decoders only (generated from the FFmpeg
   source at build time by `scripts/gen-component-lists.py`), no video
   decoding, no encoders/filters/programs. XP-compat patches applied
   (bcrypt/CancelIoEx/wcscpy_s).
4. **The plugin**, linked statically against all of the above.
5. **XP audit** (`scripts/check-xp-compat.sh`): PE subsystem ≤ 5.1 and a scan
   of the import table for known Vista+-only APIs — the same check that
   validated our XP FFmpeg builds.

The artifact from the workflow is the finished `xmp-ffmpeg.dll`.

Local build (Linux with the same apt packages as the workflow):

```sh
git clone https://github.com/one808/ffmpeg-windows-build-helpers build-helpers
git -C build-helpers checkout xp-core2
bash scripts/build-ffmpeg.sh    # ~40min first run (toolchain), cached after
bash scripts/build-plugin.sh
bash scripts/check-xp-compat.sh build/xmp-ffmpeg.dll
```

## Notes

- Output is float32 at the source rate/channel layout; XMPlay does its own
  resampling/downmixing to the output device.
- Multichannel audio is passed through with the correct speaker mask.
- Video streams are never decoded — only the container is demuxed.
- Tested on Windows XP SP3 (32-bit, Athlon XP-class hardware) and newer.

## Credits

- [FFmpeg](https://ffmpeg.org) — decoding (LGPL/GPL, built GPL v3 here)
- [XMPlay plugin SDK](https://www.un4seen.com/) by un4seen.com
- [ffmpeg-windows-build-helpers](https://github.com/one808/ffmpeg-windows-build-helpers)
  — toolchain recipe, XP patches and audit tooling

## License

GPL v3 or later (the statically linked FFmpeg is built with
`--enable-gpl --enable-version3`). See `LICENSE`.
