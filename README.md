# @playcanvas/msdfgen-wasm

Upstream [Chlumsky/msdfgen](https://github.com/Chlumsky/msdfgen) (v1.13, MIT) compiled to **WebAssembly** — generates per-glyph multi-channel signed distance fields in **Node and the browser**. It's the generation core behind [`@playcanvas/font-tools`](https://github.com/playcanvas/font-tools).

No fork, no patches: upstream msdfgen is pinned as a submodule and built with a thin Emscripten/embind binding. (Build/CI mirror the `playcanvas/ammo.js` pattern — build in CI, commit the artifact.)

## Install

```bash
npm install @playcanvas/msdfgen-wasm
```

## Usage

```js
import { createMsdfgen } from '@playcanvas/msdfgen-wasm';

const msdfgen = await createMsdfgen();       // loads the .wasm (Node or browser)
const font = msdfgen.loadFont(ttfBytes);     // Uint8Array of a TTF/OTF

// codepoint, cell size (px), pxrange. null if the font lacks the glyph.
const glyph = font.generateGlyph(0x41, 64, 8);
// glyph: { width, height, rgba,                       // size x size RGBA atlas cell
//          advance, translateX, translateY,           // em units
//          boundsL, boundsB, boundsR, boundsT, range }
font.delete();
```

Glyphs are rendered **em-normalized** (`-emnormalize`, so 1 unit = 1 em, independent of the font's `unitsPerEm`) and autoframed into a `size`×`size` cell. Metrics are reported in em units; PlayCanvas's 32-unit em is applied downstream by `font-tools`.

## Building the WASM (maintainers)

```bash
git submodule update --init --recursive          # vendor/msdfgen (upstream, pinned)
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                               # emits dist/msdfgen.{mjs,wasm}
```

FreeType is provided by Emscripten's port (`-sUSE_FREETYPE=1`); msdfgen's variable-font, SVG and PNG paths are disabled (we only load fonts and emit raw RGBA). CI builds and commits `dist/` (see `.github/workflows/build.yml`).

## License

MIT — see [LICENSE](LICENSE).

The distributed `dist/msdfgen.wasm` statically includes:

- **msdfgen** © Viktor Chlumsky — MIT ([source](https://github.com/Chlumsky/msdfgen))
- **FreeType** — The FreeType Project License (FTL). *This software is based in part on the work of the FreeType Team* ([freetype.org](https://freetype.org)).
