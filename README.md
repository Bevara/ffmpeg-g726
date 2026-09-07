# ffmpeg-g726
This filter decodes raw ITU-T G.726 ADPCM at 16, 24, 32 or 40 kbit/s, using a reduced version of ffmpeg carrying those two decoders only.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute ffmpeg-g726_1.wasm with your universal tags.

## Rebuilding the ffmpeg libraries

```bash
emconfigure $FFMPEG_SRC/configure --target-os=none --arch=x86_32 \
    --enable-cross-compile --disable-x86asm --disable-inline-asm \
    --disable-stripping --disable-programs --disable-doc \
    --disable-runtime-cpudetect --disable-autodetect --disable-pthreads \
    --pkg-config-flags="--static" --nm="$EMSDK/upstream/bin/llvm-nm" \
    --ar=emar --ranlib=emranlib --cc=emcc --cxx=em++ --objcc=emcc --dep-cc=emcc \
    --enable-pic --disable-everything \
    --enable-decoder=adpcm_g726 --enable-decoder=adpcm_g726le
emmake make
```

## Why not the Sun g72x code already in the tree

libg711 ships Sun's public domain g72x, and G.726 is the 1990 merge of G.721
and G.723, so the obvious move was another rate there. It does not hold.
Measured on the same bitstreams, against the source:

| rate | Sun g72x | ffmpeg |
|---|---|---|
| 16 kbit/s | 11.6 dB | 11.8 dB |
| 24 kbit/s | 17.1 dB | 17.7 dB |
| 32 kbit/s | 21.4 dB | 23.4 dB |
| 40 kbit/s | **-3.4 dB** | 28.5 dB |

And the reverse holds too: on a Sun `.au` file coded at 40 kbit/s the Sun code
gives 28.6 dB and ffmpeg 8.0 dB. Above 24 kbit/s they are simply not the same
bitstream, so `libg711`'s `audec` stays what it is — a reader of Sun `.au`
files — and G.726 proper lives here.

## Options

A raw G.726 stream has no header: no magic, no rate, no bit rate. So the three
things a decoder must know are options.

| option | default | |
|---|---|---|
| `bits` | 4 | code word size: 2, 3, 4 or 5, i.e. 16, 24, 32 or 40 kbit/s |
| `srate` | 8000 | sampling rate in Hz |
| `le` | false | code words packed LSB first (WAV, Sun) rather than MSB first (ITU-T, RTP) |

The defaults are what ffmpeg's own `.g726` muxer writes.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
