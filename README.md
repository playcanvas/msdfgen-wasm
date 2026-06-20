# @playcanvas/msdfgen-wasm

Upstream [Chlumsky/msdfgen](https://github.com/Chlumsky/msdfgen) (MIT) compiled to WebAssembly, for generating per-glyph multi-channel signed distance fields **in Node and the browser** — the generation core behind [`@playcanvas/font-tools`](../font-tools). No native binaries, no Editor.

This repo does **not** fork or patch msdfgen. It pins upstream as an unmodified submodule and adds only an Emscripten binding + build. (Build/CI mirror the `playcanvas/ammo.js` pattern: pin the toolchain, build in CI, commit the artifact.)

## Why em-normalized

The font metrics PlayCanvas expects are independent of a font's `unitsPerEm`. Upstream msdfgen provides `-emnormalize` (1 unit = 1 em) — exactly what the Editor pipeline relies on (replacing the dead `playcanvas/msdfgen` fork's `import-font.cpp` patch). The binding renders each glyph with the same parameters the Editor uses:

```
-emnormalize -scale 32 -autoframe -size 64 64 -pxrange 8
```

and reports `advance`, `translate`, `bounds`, `range` in **em units**. `@playcanvas/font-tools` multiplies these by 32 to reach PlayCanvas's 32-unit em.

> Pin an upstream msdfgen recent enough to support `-emnormalize`.

## JS API (the contract `font-tools` consumes)

```ts
import { createMsdfgen } from '@playcanvas/msdfgen-wasm';

const msdfgen = await createMsdfgen();      // loads the .wasm (Node or browser)
const font = msdfgen.loadFont(ttfBytes);    // Uint8Array

// null if the font has no glyph for this codepoint
const glyph = msdfgen.generateGlyph(font, 0x41, { size: 64, pxrange: 8 });
// glyph: { width, height, rgba: Uint8Array,           // size x size RGBA atlas cell
//          advance, translate:[x,y], bounds:[l,b,r,t], range }   // all em units

font.delete();
```

See [`src/loader.ts`](src/loader.ts) for the typed surface. `@playcanvas/font-tools` adapts this to its `GlyphSource` interface.

## Build (CI / local emsdk)

```bash
git submodule update --init --recursive   # vendor/msdfgen (upstream, pinned)
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# emits dist/msdfgen.{mjs,wasm}; committed to the repo (see .github/workflows/build.yml)
```

FreeType is provided by Emscripten's port (`-sUSE_FREETYPE=1`), so there is no separate FreeType build.

## Acceptance

Parity, not vibes: generating the printable-ASCII set from Arial and converting via `font-tools` must reproduce the engine's golden `examples/assets/fonts/arial.json` metrics within tolerance (this is the existing `font-tools` parity test, fed real glyph data).

## Status

Scaffold. The Emscripten binding/CMake/CI here were authored without a local emsdk toolchain and **will need iteration on first CI build**. The JS contract (`src/loader.ts`) and the msdfgen invocation are the stable parts.
