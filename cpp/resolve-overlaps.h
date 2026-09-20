// Overlap resolution for msdfgen shapes without Skia.
//
// msdfgen computes each texel's distance to the nearest edge of the shape and takes its sign
// from that edge's orientation. That is only correct when every edge borders the outside:
// a self-intersecting contour (or two overlapping contours) has edge pieces that run through
// the filled region, and texels near them get a negative distance -> pinholes at stroke
// junctions. Upstream fixes this with Skia's path ops (resolveShapeGeometry), which we do not
// build. This is a small, dependency-free equivalent for the non-zero winding rule.

#pragma once

#include "msdfgen.h"

namespace pcmsdf {

enum ResolveResult {
    RESOLVE_UNCHANGED = 0, // no overlapping geometry found; the shape was left untouched
    RESOLVE_RESOLVED = 1,  // overlaps found and removed; the shape's contours were rewritten
    RESOLVE_FAILED = -1    // overlaps found but the boundary could not be re-chained; shape untouched
};

// Rewrites `shape` so that its contours trace only the boundary of the region filled under
// the non-zero winding rule: contours are split where they cross (each other or themselves),
// pieces that lie inside the filled region are discarded, and the remaining pieces are chained
// back into closed contours oriented with the fill on their right (msdfgen's convention).
// Call before Shape::normalize() and edge coloring. Shapes without overlaps are not modified.
ResolveResult resolveOverlaps(msdfgen::Shape &shape);

} // namespace pcmsdf
