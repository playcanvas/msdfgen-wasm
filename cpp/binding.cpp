// Emscripten/embind binding around upstream msdfgen v1.13.
//
// Exposes a Font class to JS (see src/loader.ts). Each glyph is rendered em-normalized
// (1 unit = 1 em) and centered in a size x size cell, matching the Editor pipeline's
//   -emnormalize -scale (size/2) -autoframe -size <size> <size> -pxrange <pxrange>
// so that 1 em -> size/2 px (the shipped fonts use 64px cells == 2 em). Metrics
// (advance/translate/bounds) are reported in em units; @playcanvas/font-tools scales by 32.
//
// The autoframe translate formula below was validated analytically against the engine's
// golden arial.json (e.g. 'A' -> xoffset 21.33, yoffset 20.55).

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <string>
#include <vector>

#include "msdfgen.h"
#include "msdfgen-ext.h"

using namespace emscripten;

namespace {
msdfgen::FreetypeHandle *g_ft = nullptr;
msdfgen::FreetypeHandle *freetype() {
    if (!g_ft) g_ft = msdfgen::initializeFreetype();
    return g_ft;
}
} // namespace

class Font {
public:
    // `bytes` is a JS Uint8Array of the TTF/OTF. Kept alive for the font's lifetime
    // because FreeType's FT_New_Memory_Face references (does not copy) the buffer.
    explicit Font(val bytes)
        : data_(convertJSArrayToNumberVector<unsigned char>(bytes)) {
        handle_ = msdfgen::loadFontData(freetype(),
            reinterpret_cast<const msdfgen::byte *>(data_.data()), static_cast<int>(data_.size()));
    }

    ~Font() { if (handle_) msdfgen::destroyFont(handle_); }

    bool ok() const { return handle_ != nullptr; }

    // Returns a JS object (see src/loader.ts) or null if the font lacks the glyph.
    val generateGlyph(int codepoint, int size, double pxrange) {
        if (!handle_) return val::null();

        // Skip codepoints the font doesn't define (don't bake .notdef into the atlas).
        msdfgen::GlyphIndex gindex(0);
        if (!msdfgen::getGlyphIndex(gindex, handle_, static_cast<msdfgen::unicode_t>(codepoint)) ||
            gindex.getIndex() == 0) {
            return val::null();
        }

        msdfgen::Shape shape;
        double advance = 0.0;
        if (!msdfgen::loadGlyph(shape, handle_, gindex, msdfgen::FONT_SCALING_EM_NORMALIZED, &advance)) {
            return val::null();
        }
        shape.normalize();
        msdfgen::edgeColoringSimple(shape, 3.0);

        const msdfgen::Shape::Bounds b = shape.getBounds();
        const bool hasInk = !shape.contours.empty() && b.l <= b.r && b.b <= b.t;

        const double scale = 0.5 * size;       // px per em -> 64px cell == 2 em
        const double cellEm = size / scale;    // == 2
        const double rangeEm = pxrange / scale;

        // Autoframe: center the glyph's em-space bounds within the cell.
        double tx = 0.0, ty = 0.0;
        if (hasInk) {
            tx = 0.5 * (cellEm - (b.l + b.r));
            ty = 0.5 * (cellEm - (b.b + b.t));
        }

        std::vector<unsigned char> rgba(static_cast<size_t>(size) * size * 4, 0);
        if (hasInk) {
            std::vector<float> sdf(static_cast<size_t>(size) * size * 3);
            msdfgen::BitmapRef<float, 3> ref(sdf.data(), size, size);
            msdfgen::generateMSDF(ref, shape,
                msdfgen::Projection(msdfgen::Vector2(scale, scale), msdfgen::Vector2(tx, ty)),
                msdfgen::Range(rangeEm));

            // Pack to top-down RGBA (msdfgen bitmaps are bottom-up by default).
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const float *px = ref(x, size - 1 - y);
                    const size_t o = (static_cast<size_t>(y) * size + x) * 4;
                    rgba[o + 0] = msdfgen::pixelFloatToByte(px[0]);
                    rgba[o + 1] = msdfgen::pixelFloatToByte(px[1]);
                    rgba[o + 2] = msdfgen::pixelFloatToByte(px[2]);
                    rgba[o + 3] = 255;
                }
            }
        }

        // Copy the buffer into a JS Uint8Array (the C++ vector is freed on return).
        val out = val::object();
        out.set("width", size);
        out.set("height", size);
        out.set("rgba", val::global("Uint8Array").new_(typed_memory_view(rgba.size(), rgba.data())));
        out.set("advance", advance);
        out.set("translateX", tx);
        out.set("translateY", ty);
        out.set("boundsL", hasInk ? b.l : 0.0);
        out.set("boundsB", hasInk ? b.b : 0.0);
        out.set("boundsR", hasInk ? b.r : 0.0);
        out.set("boundsT", hasInk ? b.t : 0.0);
        out.set("range", rangeEm);
        return out;
    }

private:
    std::vector<unsigned char> data_;
    msdfgen::FontHandle *handle_ = nullptr;
};

EMSCRIPTEN_BINDINGS(msdfgen_module) {
    class_<Font>("Font")
        .constructor<val>()
        .function("ok", &Font::ok)
        .function("generateGlyph", &Font::generateGlyph);
}
