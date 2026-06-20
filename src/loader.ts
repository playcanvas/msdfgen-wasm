/**
 * @playcanvas/msdfgen-wasm — typed loader for the Emscripten-built msdfgen module.
 *
 * This file defines the stable JS contract that @playcanvas/font-tools consumes. The
 * actual WASM (dist/msdfgen.{mjs,wasm}) is produced by the Emscripten build (see
 * CMakeLists.txt / cpp/binding.cpp) and committed to the repo.
 */

/** A glyph rendered to a square MSDF atlas cell, with metrics in em units (1 em = 1.0). */
export interface GlyphResult {
    /** Cell width in px (== size). */
    width: number;
    /** Cell height in px (== size). */
    height: number;
    /** RGBA pixels, width * height * 4. */
    rgba: Uint8Array;
    /** Horizontal advance, em units. */
    advance: number;
    /** Glyph translation within the cell [x, y], em units. */
    translate: [number, number];
    /** Tight glyph bounds [left, bottom, right, top], em units. */
    bounds: [number, number, number, number];
    /** Distance-field range width, em units. */
    range: number;
}

/** A loaded font; call delete() to free the WASM-side handle. */
export interface FontHandle {
    delete(): void;
}

export interface GenerateOptions {
    /** Atlas cell size in px (default 64). */
    size?: number;
    /** MSDF pixel range (default 8). */
    pxrange?: number;
}

export interface Msdfgen {
    /** Load a TTF/OTF from bytes. */
    loadFont(bytes: Uint8Array): FontHandle;
    /**
     * Render one glyph. Returns null if the font has no glyph for `codepoint`
     * (mirrors charToGlyphIndex === 0). Renders with the Editor-equivalent
     * parameters: -emnormalize -scale 32 -autoframe -size <size> <size> -pxrange <pxrange>.
     */
    generateGlyph(font: FontHandle, codepoint: number, opts?: GenerateOptions): GlyphResult | null;
}

export interface CreateOptions {
    /** Override the .wasm URL (browser); defaults to the bundled artifact. */
    wasmUrl?: string | URL;
}

/**
 * Instantiate the msdfgen WASM module (async; loads + initializes the .wasm).
 * Works in Node and the browser.
 *
 * NOTE: throws until the Emscripten artifact is built (dist/msdfgen.mjs). See README.
 */
export async function createMsdfgen(_opts: CreateOptions = {}): Promise<Msdfgen> {
    throw new Error(
        '@playcanvas/msdfgen-wasm: WASM artifact not built yet. Run the Emscripten build ' +
        '(see README/CMakeLists.txt) to produce dist/msdfgen.{mjs,wasm}.'
    );
}
