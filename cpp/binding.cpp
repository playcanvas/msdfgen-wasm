// Emscripten/embind binding around upstream msdfgen.
//
// Exposes loadFont + generateGlyph to JS (see src/loader.ts for the contract).
// Renders each glyph em-normalized (1 unit = 1 em), matching the Editor pipeline's
//   -emnormalize -scale 32 -autoframe -size 64 64 -pxrange 8
// and reports advance/translate/bounds/range in em units.
//
// STATUS: skeleton authored without a local emsdk toolchain. The msdfgen library API
// has shifted across versions (FontCoordinateScaling, Projection, Range, generateMSDF
// overloads), so the exact calls below — and especially the autoframe scale/translate
// math and the bitmap Y-orientation — MUST be validated against the engine's golden
// examples/assets/fonts/arial.json (the font-tools parity test, fed real glyph data).

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <vector>

#include "msdfgen.h"
#include "msdfgen-ext.h"

using namespace emscripten;

namespace {

// One initialized FreeType instance for the module's lifetime.
msdfgen::FreetypeHandle *g_ft = nullptr;

msdfgen::FreetypeHandle *freetype() {
    if (!g_ft) g_ft = msdfgen::initializeFreetype();
    return g_ft;
}

} // namespace

// Opaque font handle returned to JS (delete() frees it).
class Font {
public:
    explicit Font(const std::string &bytes) {
        handle = msdfgen::loadFontData(freetype(),
            reinterpret_cast<const msdfgen::byte *>(bytes.data()), (int)bytes.size());
    }
    ~Font() { if (handle) msdfgen::destroyFont(handle); }

    // Returns a JS object matching GlyphResult, or null if the glyph is absent.
    val generateGlyph(int codepoint, int size, double pxrange) {
        if (!handle) return val::null();

        msdfgen::Shape shape;
        double advance = 0.0;
        // EM_NORMALIZED == the CLI's -emnormalize (coords independent of unitsPerEm).
        if (!msdfgen::loadGlyph(shape, handle, (msdfgen::unicode_t)codepoint,
                                msdfgen::FontCoordinateScaling::EM_NORMALIZED, &advance)) {
            return val::null(); // no glyph for this codepoint
        }
        if (shape.edgeCount() == 0) {
            // whitespace etc. — emit an empty cell but keep the advance/metrics
        }

        shape.normalize();
        msdfgen::edgeColoringSimple(shape, 3.0);
        const msdfgen::Shape::Bounds b = shape.getBounds();

        // TODO(parity): replicate `-scale 32 -autoframe` framing here — derive the
        // per-glyph scale + translate that fit the em-normalized shape into a size x size
        // cell with `pxrange` padding, then validate translate/bounds/range against arial.json.
        const double scale = 32.0;                 // px per em (provisional; see TODO)
        const msdfgen::Vector2 translate(0.0, 0.0); // provisional
        const double rangeEm = pxrange / scale;

        msdfgen::Bitmap<float, 3> msdf(size, size);
        msdfgen::generateMSDF(msdf, shape,
            msdfgen::Projection(scale, translate), rangeEm);

        // Pack to RGBA (a=255). TODO(parity): confirm Y orientation matches the engine's
        // _getUv convention (msdfgen bitmaps are bottom-up).
        std::vector<unsigned char> rgba((size_t)size * size * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float *px = msdf(x, size - 1 - y);
                size_t o = ((size_t)y * size + x) * 4;
                rgba[o + 0] = (unsigned char)msdfgen::clamp(int(px[0] * 256.f), 255);
                rgba[o + 1] = (unsigned char)msdfgen::clamp(int(px[1] * 256.f), 255);
                rgba[o + 2] = (unsigned char)msdfgen::clamp(int(px[2] * 256.f), 255);
                rgba[o + 3] = 255;
            }
        }

        val out = val::object();
        out.set("width", size);
        out.set("height", size);
        out.set("rgba", val(typed_memory_view(rgba.size(), rgba.data())));
        out.set("advance", advance);
        out.set("translate", val::array(std::vector<double>{ translate.x, translate.y }));
        out.set("bounds", val::array(std::vector<double>{ b.l, b.b, b.r, b.t }));
        out.set("range", rangeEm);
        return out;
    }

private:
    msdfgen::FontHandle *handle = nullptr;
};

EMSCRIPTEN_BINDINGS(msdfgen_module) {
    class_<Font>("Font")
        .constructor<std::string>()
        .function("generateGlyph", &Font::generateGlyph);
}
