// See resolve-overlaps.h. Outline of the algorithm:
//
//   1. Find every point where two edges cross (curve/curve, via flattening + Newton refinement)
//      and every vertex that lies on the interior of another edge (T-junctions, shared edges).
//   2. Split the edges at those parameters into pieces. Untouched edges are cloned verbatim.
//   3. Classify each piece by probing the winding number a hair to its left and right. A piece
//      with fill on exactly one side is boundary (reversed if the fill is on its left, so the
//      result matches msdfgen's fill-on-the-right convention); anything else is interior and
//      is dropped. Contours that touch nothing are decided by a single probe; if such a contour
//      runs the wrong way (msdfgen would render it inverted) it is reversed as a whole.
//   4. Chain the boundary pieces end-to-start into closed contours, snapping the joins exactly.
//
// Everything is in em units (glyphs are loaded em-normalized), so one font unit of a 2048 upem
// font is ~4.9e-4 and one texel of the 64 px cell is 1/32.

#include "resolve-overlaps.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <vector>

#include "core/arithmetics.hpp"

// Build with -DPCMSDF_TRACE to log the per-piece decisions to stdout.
#ifdef PCMSDF_TRACE
#include <cstdio>
#define TRACE(...) std::printf(__VA_ARGS__)
#else
#define TRACE(...) ((void) 0)
#endif

namespace pcmsdf {

using msdfgen::Contour;
using msdfgen::CubicSegment;
using msdfgen::EdgeHolder;
using msdfgen::EdgeSegment;
using msdfgen::LinearSegment;
using msdfgen::Point2;
using msdfgen::QuadraticSegment;
using msdfgen::Scanline;
using msdfgen::Shape;
using msdfgen::SignedDistance;
using msdfgen::Vector2;
using msdfgen::crossProduct;
using msdfgen::dotProduct;
using msdfgen::mix;

namespace {

// Points closer than this are the same vertex. Fonts sit on an integer grid (>= 4.9e-4 em apart
// at 2048 upem) but curves routinely pass a few 1e-5 em from a vertex they were meant to hit;
// treating those as touching keeps the boundary chain closed. 2.5e-4 em is 0.008 px in a 64 px cell.
constexpr double POINT_EPS = 2.5e-4;
constexpr double MIN_PIECE_LEN = POINT_EPS; // shorter split pieces are slivers; the join is snapped shut
constexpr double MAX_PROBE = 1e-4;          // max offset of the winding probes on either side of a piece
constexpr double MAX_ORPHAN_LEN = 2e-3;     // dangling chains shorter than this (0.06 px) are discarded
constexpr double MIN_PROBE = 1e-9;      // below this the piece is too pinched to classify
constexpr double MIN_CONTOUR_AREA = 1e-9;
constexpr int FLATTEN_STEPS_QUADRATIC = 8;
constexpr int FLATTEN_STEPS_CUBIC = 16;

struct Box {
    double l = DBL_MAX, b = DBL_MAX, r = -DBL_MAX, t = -DBL_MAX;
    bool overlaps(const Box &o) const { return l <= o.r && o.l <= r && b <= o.t && o.b <= t; }
    bool contains(Point2 p) const { return p.x >= l && p.x <= r && p.y >= b && p.y <= t; }
    double distance(Point2 p) const {
        double dx = std::max(std::max(l - p.x, p.x - r), 0.0);
        double dy = std::max(std::max(b - p.y, p.y - t), 0.0);
        return std::sqrt(dx * dx + dy * dy);
    }
};

Box boxOf(const EdgeSegment *e) {
    Box bx;
    e->bound(bx.l, bx.b, bx.r, bx.t);
    bx.l -= POINT_EPS; bx.b -= POINT_EPS; bx.r += POINT_EPS; bx.t += POINT_EPS;
    return bx;
}

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

bool samePoint(Point2 a, Point2 b) { return (a - b).squaredLength() <= POINT_EPS * POINT_EPS; }

int flattenSteps(const EdgeSegment *e) {
    switch (e->type()) {
        case QuadraticSegment::EDGE_TYPE: return FLATTEN_STEPS_QUADRATIC;
        case CubicSegment::EDGE_TYPE: return FLATTEN_STEPS_CUBIC;
        default: return 1;
    }
}

// --- sub-curve extraction (polar forms / blossoms) ------------------------------------------

Point2 blossom2(const Point2 *p, double u, double v) {
    return mix(mix(p[0], p[1], u), mix(p[1], p[2], u), v);
}

Point2 blossom3(const Point2 *p, double u, double v, double w) {
    Point2 a = mix(p[0], p[1], u), b = mix(p[1], p[2], u), c = mix(p[2], p[3], u);
    return mix(mix(a, b, v), mix(b, c, v), w);
}

// The part of `e` between parameters t0 < t1 as a new segment of the same type.
EdgeSegment *subSegment(const EdgeSegment *e, double t0, double t1) {
    const Point2 *p = e->controlPoints();
    switch (e->type()) {
        case LinearSegment::EDGE_TYPE:
            return new LinearSegment(mix(p[0], p[1], t0), mix(p[0], p[1], t1), e->color);
        case QuadraticSegment::EDGE_TYPE:
            return new QuadraticSegment(blossom2(p, t0, t0), blossom2(p, t0, t1), blossom2(p, t1, t1), e->color);
        case CubicSegment::EDGE_TYPE:
            return new CubicSegment(blossom3(p, t0, t0, t0), blossom3(p, t0, t0, t1),
                                    blossom3(p, t0, t1, t1), blossom3(p, t1, t1, t1), e->color);
    }
    return e->clone();
}

// Move an endpoint without msdfgen's tangent-preserving adjustment (which divides by the
// control polygon's cross product and misbehaves on nearly straight curves). Moves are <= POINT_EPS.
void setStartPoint(EdgeSegment *e, Point2 to) {
    switch (e->type()) {
        case LinearSegment::EDGE_TYPE: static_cast<LinearSegment *>(e)->p[0] = to; break;
        case QuadraticSegment::EDGE_TYPE: static_cast<QuadraticSegment *>(e)->p[0] = to; break;
        case CubicSegment::EDGE_TYPE: static_cast<CubicSegment *>(e)->p[0] = to; break;
    }
}

void setEndPoint(EdgeSegment *e, Point2 to) {
    switch (e->type()) {
        case LinearSegment::EDGE_TYPE: static_cast<LinearSegment *>(e)->p[1] = to; break;
        case QuadraticSegment::EDGE_TYPE: static_cast<QuadraticSegment *>(e)->p[2] = to; break;
        case CubicSegment::EDGE_TYPE: static_cast<CubicSegment *>(e)->p[3] = to; break;
    }
}

// Exact derivative. (EdgeSegment::direction() is only proportional to it: msdfgen drops the
// factor 2 / 3 of the Bezier derivative, which would make Newton steps overshoot.)
Vector2 derivative(const EdgeSegment *e, double t) {
    const Point2 *p = e->controlPoints();
    switch (e->type()) {
        case LinearSegment::EDGE_TYPE:
            return p[1] - p[0];
        case QuadraticSegment::EDGE_TYPE:
            return 2 * mix(p[1] - p[0], p[2] - p[1], t);
        case CubicSegment::EDGE_TYPE:
            return 3 * mix(mix(p[1] - p[0], p[2] - p[1], t), mix(p[2] - p[1], p[3] - p[2], t), t);
    }
    return e->direction(t);
}

double polylineLength(const EdgeSegment *e, int steps = 8) {
    double len = 0;
    Point2 prev = e->point(0);
    for (int i = 1; i <= steps; ++i) {
        Point2 cur = e->point(double(i) / steps);
        len += (cur - prev).length();
        prev = cur;
    }
    return len;
}

// --- intersection search ---------------------------------------------------------------------

// Records a split parameter on e; returns false if it lands on a vertex or an existing split.
bool addSplit(const EdgeSegment *e, std::vector<double> &splits, double t) {
    Point2 p = e->point(t);
    if (samePoint(p, e->point(0)) || samePoint(p, e->point(1))) return false; // at a vertex: nothing to split
    for (double s : splits)
        if (samePoint(e->point(s), p)) return false;
    splits.push_back(t);
    return true;
}

// Newton-refine an approximate crossing a(ta) = b(tb). Returns true if it converged onto a
// common point (which may sit at a parameter endpoint: a T-junction).
bool refineCrossing(const EdgeSegment *a, const EdgeSegment *b, double &ta, double &tb) {
    for (int i = 0; i < 16; ++i) {
        Vector2 f = a->point(ta) - b->point(tb);
        if (f.squaredLength() < 1e-26) break;
        Vector2 da = derivative(a, ta), db = derivative(b, tb);
        double det = crossProduct(da, db);
        if (std::fabs(det) < 1e-18) break; // (near-)tangential: no unique crossing
        // Solve da*dta - db*dtb = -f.
        double dta = crossProduct(db, f) / det;
        double dtb = crossProduct(da, f) / det;
        double nta = clamp01(ta + dta), ntb = clamp01(tb + dtb);
        if (nta == ta && ntb == tb) break;
        ta = nta; tb = ntb;
    }
    return (a->point(ta) - b->point(tb)).squaredLength() < 1e-18;
}

// Flatten `e` into `steps` line segments (points and their curve parameters).
void flatten(const EdgeSegment *e, int steps, std::vector<Point2> &pts, std::vector<double> &ts) {
    pts.resize(steps + 1); ts.resize(steps + 1);
    for (int i = 0; i <= steps; ++i) {
        ts[i] = double(i) / steps;
        pts[i] = e->point(ts[i]);
    }
}

// Every crossing between a and b, appended to their split lists. Sets `incident` if the curves
// meet anywhere (including at a shared vertex) and `split` if a new split was recorded.
void findCrossings(const EdgeSegment *a, const EdgeSegment *b, std::vector<double> &splitsA,
                   std::vector<double> &splitsB, bool &incident, bool &split) {
    static std::vector<Point2> pa, pb; // scratch buffers (the WASM module is single-threaded)
    static std::vector<double> ta, tb;
    flatten(a, flattenSteps(a), pa, ta);
    flatten(b, flattenSteps(b), pb, tb);
    const double tol = 0.05; // generous: Newton corrects the flattened estimate
    for (size_t i = 0; i + 1 < pa.size(); ++i) {
        Vector2 r = pa[i + 1] - pa[i];
        for (size_t j = 0; j + 1 < pb.size(); ++j) {
            Vector2 s = pb[j + 1] - pb[j];
            double denom = crossProduct(r, s);
            if (std::fabs(denom) < 1e-18) continue; // parallel; shared-line cases are caught by the vertex pass
            Vector2 d = pb[j] - pa[i];
            double t = crossProduct(d, s) / denom, u = crossProduct(d, r) / denom;
            if (t < -tol || t > 1 + tol || u < -tol || u > 1 + tol) continue;
            double pta = mix(ta[i], ta[i + 1], clamp01(t)), ptb = mix(tb[j], tb[j + 1], clamp01(u));
            if (!refineCrossing(a, b, pta, ptb)) continue;
            incident = true;
            split = addSplit(a, splitsA, pta) || split;
            split = addSplit(b, splitsB, ptb) || split;
        }
    }
}

// If vertex v lies on the interior of e, returns true and its parameter.
bool vertexOnEdge(const EdgeSegment *e, Point2 v, double &t) {
    const Point2 *p = e->controlPoints();
    Point2 end = e->point(1);
    if (samePoint(v, p[0]) || samePoint(v, end)) return false;
    SignedDistance sd = e->signedDistance(v, t);
    if (std::fabs(sd.distance) > 2 * POINT_EPS) return false;
    // Polish the closest-point parameter (cubic search is approximate).
    for (int i = 0; i < 4; ++i) {
        Vector2 d = derivative(e, t);
        double dd = dotProduct(d, d);
        if (dd == 0) break;
        t = clamp01(t + dotProduct(v - e->point(t), d) / dd);
    }
    Point2 q = e->point(t);
    if (samePoint(q, p[0]) || samePoint(q, end)) return false;
    return samePoint(q, v);
}

// --- pieces ----------------------------------------------------------------------------------

struct Piece {
    std::unique_ptr<EdgeSegment> seg;
    int parent = -1;   // flat edge index
    int contour = -1;
    bool split = false; // produced by splitting (vs a verbatim clone of a whole edge)
    Point2 start, end, mid;
    Vector2 startDir, endDir;
    bool keep = false;

    void refresh() {
        start = seg->point(0); end = seg->point(1); mid = seg->point(.5);
        startDir = seg->direction(0).normalize(); endDir = seg->direction(1).normalize();
    }
};

int windingAt(const Shape &shape, Point2 p) {
    Scanline line;
    shape.scanline(line, p.y);
    return line.sumIntersections(p.x);
}

enum Side { SIDE_NONE, SIDE_BOUNDARY, SIDE_INTERIOR, SIDE_DEGENERATE };

// Probe the winding number either side of the piece. `clearance` is the distance from its
// midpoint to the nearest other edge, bounding how far the probes may stray.
Side classify(const Shape &shape, Piece &piece, double len, double clearance) {
    double probe = std::min(MAX_PROBE, .5 * std::min(clearance, .5 * len));
    if (probe < MIN_PROBE) return SIDE_DEGENERATE;
    Vector2 dir = piece.seg->direction(.5).normalize();
    Vector2 right = dir.getOrthonormal(false);
    int wRight = windingAt(shape, piece.mid + probe * right);
    int wLeft = windingAt(shape, piece.mid - probe * right);
    if ((wRight != 0) == (wLeft != 0)) return SIDE_INTERIOR; // fill on both sides (or neither)
    if (wLeft != 0) { // fill on the left: flip to msdfgen's fill-on-the-right convention
        piece.seg->reverse();
        piece.refresh();
    }
    return SIDE_BOUNDARY;
}

double contourArea(const Contour &c) {
    double total = 0;
    Point2 prev = c.edges.back()->point(1);
    for (const EdgeHolder &e : c.edges) {
        for (int i = 1; i <= 4; ++i) {
            Point2 cur = e->point(i / 4.);
            total += (cur.x - prev.x) * (cur.y + prev.y); // shoelace
            prev = cur;
        }
    }
    return .5 * total;
}

} // namespace

ResolveResult resolveOverlaps(Shape &shape) {
    // Flat edge list.
    std::vector<const EdgeSegment *> edges;
    std::vector<int> edgeContour, edgeIndexInContour, contourEdgeCount;
    for (int c = 0; c < (int) shape.contours.size(); ++c) {
        const Contour &contour = shape.contours[c];
        contourEdgeCount.push_back((int) contour.edges.size());
        for (int i = 0; i < (int) contour.edges.size(); ++i) {
            edges.push_back(contour.edges[i]);
            edgeContour.push_back(c);
            edgeIndexInContour.push_back(i);
        }
    }
    const int n = (int) edges.size();
    if (n < 2) return RESOLVE_UNCHANGED;

    std::vector<Box> boxes(n);
    for (int i = 0; i < n; ++i) boxes[i] = boxOf(edges[i]);

    auto adjacent = [&](int i, int j) {
        if (edgeContour[i] != edgeContour[j]) return false;
        int cnt = contourEdgeCount[edgeContour[i]];
        int a = edgeIndexInContour[i], b = edgeIndexInContour[j];
        return (a + 1) % cnt == b || (b + 1) % cnt == a;
    };

    // 1. Crossings and T-junctions.
    std::vector<std::vector<double>> splits(n);
    std::vector<bool> contourTouched(shape.contours.size(), false);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (!boxes[i].overlaps(boxes[j])) continue;
            bool incident = false, split = false;
            findCrossings(edges[i], edges[j], splits[i], splits[j], incident, split);
            // Neighbouring edges always meet at their shared vertex; that alone is not contact.
            if (split || (incident && !adjacent(i, j))) {
                contourTouched[edgeContour[i]] = true;
                contourTouched[edgeContour[j]] = true;
            }
        }
    }
    for (int k = 0; k < n; ++k) {
        Point2 v = edges[k]->point(0);
        for (int m = 0; m < n; ++m) {
            if (m == k || adjacent(k, m) || !boxes[m].contains(v)) continue;
            double t;
            if (vertexOnEdge(edges[m], v, t)) {
                addSplit(edges[m], splits[m], t);
                contourTouched[edgeContour[k]] = true;
                contourTouched[edgeContour[m]] = true;
            }
        }
    }

    // (No early-out for shapes that touch nothing: every contour still gets one winding probe
    // below, which is what catches fonts whose outlines all run the wrong way, e.g. Roboto Mono.)

    // 2. Pieces.
    std::vector<Piece> pieces;
    for (int k = 0; k < n; ++k) {
        std::vector<double> &s = splits[k];
        std::sort(s.begin(), s.end());
        auto push = [&](EdgeSegment *seg, bool split) {
            Piece p;
            p.seg.reset(seg); p.parent = k; p.contour = edgeContour[k]; p.split = split;
            p.refresh();
            pieces.push_back(std::move(p));
        };
        if (s.empty()) {
            push(edges[k]->clone(), false);
        } else {
            double t0 = 0;
            for (double t : s) { push(subSegment(edges[k], t0, t), true); t0 = t; }
            push(subSegment(edges[k], t0, 1), true);
        }
    }

    // 3. Classification. Contours that never meet other geometry are entirely inside or
    // entirely outside it, so one probe decides the whole contour.
    auto clearanceOf = [&](const Piece &piece) {
        double best = DBL_MAX;
        Vector2 dir = derivative(piece.seg.get(), .5).normalize();
        for (int m = 0; m < n; ++m) {
            if (m == piece.parent || boxes[m].distance(piece.mid) >= best) continue;
            double t;
            double d = std::fabs(edges[m]->signedDistance(piece.mid, t).distance);
            if (d < POINT_EPS) {
                // An edge running along this piece (two contours sharing a stretch of outline)
                // does not get between the piece and its probes; skip it. A crossing edge would,
                // but crossings have already been split off, so this only happens for tangencies.
                Vector2 other = derivative(edges[m], clamp01(t)).normalize();
                if (std::fabs(crossProduct(dir, other)) < 1e-3) continue;
            }
            best = std::min(best, d);
        }
        return best;
    };
    bool changed = false;

    // Pieces that retrace each other in opposite directions (the outline doubling back on itself,
    // or two fills abutting along a shared stretch) enclose nothing and cancel out. Decide them
    // here, before probing: their probes would land inside a zero-width sliver.
    std::vector<bool> cancelled(pieces.size(), false);
    for (size_t i = 0; i < pieces.size(); ++i) {
        for (size_t j = i + 1; j < pieces.size(); ++j) {
            if (cancelled[i] || cancelled[j]) continue;
            if (samePoint(pieces[i].start, pieces[j].end) && samePoint(pieces[i].end, pieces[j].start) &&
                samePoint(pieces[i].mid, pieces[j].mid)) {
                cancelled[i] = cancelled[j] = true;
                changed = true;
            }
        }
    }

    std::vector<int> contourVerdict(shape.contours.size(), 0); // 0 unknown, 1 keep, 2 keep reversed, -1 drop
    for (size_t idx = 0; idx < pieces.size(); ++idx) {
        Piece &piece = pieces[idx];
        if (cancelled[idx]) { piece.keep = false; continue; }
        if (!contourTouched[piece.contour]) {
            int &verdict = contourVerdict[piece.contour];
            if (verdict == 0) {
                // Probe the contour's longest edge, the most robust place to sample.
                int best = -1; double bestLen = -1;
                for (int k = 0; k < n; ++k) {
                    if (edgeContour[k] != piece.contour) continue;
                    double len = polylineLength(edges[k]);
                    if (len > bestLen) { bestLen = len; best = k; }
                }
                Piece probe;
                probe.seg.reset(edges[best]->clone()); probe.parent = best; probe.contour = piece.contour;
                probe.refresh();
                Side side = classify(shape, probe, bestLen, clearanceOf(probe));
                if (side == SIDE_INTERIOR) {
                    verdict = -1;
                } else if (side == SIDE_BOUNDARY && probe.start != edges[best]->point(0)) {
                    // classify() flipped the probe: the whole contour runs with the fill on its left.
                    // msdfgen would render it inverted (e.g. Raleway's '6'), so reverse it.
                    verdict = 2;
                } else {
                    verdict = 1; // kept verbatim, so shapes without overlaps come out bit-identical
                }
                if (verdict != 1) changed = true;
            }
            piece.keep = verdict > 0;
            if (verdict == 2) { piece.seg->reverse(); piece.refresh(); }
            continue;
        }
        double len = polylineLength(piece.seg.get());
        if (piece.split && len < MIN_PIECE_LEN) { piece.keep = false; continue; } // sliver from a split; the join is snapped shut
        double clearance = clearanceOf(piece);
        Side side = classify(shape, piece, len, clearance);
        TRACE("piece edge=%d contour=%d split=%d len=%.3g clearance=%.3g (%.6f,%.6f)->(%.6f,%.6f) side=%d\n",
              piece.parent, piece.contour, (int) piece.split, len, clearance,
              piece.start.x, piece.start.y, piece.end.x, piece.end.y, (int) side);
        if (side == SIDE_BOUNDARY) {
            piece.keep = true;
        } else {
            piece.keep = false;
            changed = true;
        }
    }
    if (!changed) return RESOLVE_UNCHANGED;

    // Coincident boundary pieces (two contours sharing an edge with the same orientation):
    // keep one.
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (!pieces[i].keep) continue;
        for (size_t j = i + 1; j < pieces.size(); ++j) {
            if (!pieces[j].keep) continue;
            if (samePoint(pieces[i].start, pieces[j].start) && samePoint(pieces[i].end, pieces[j].end) &&
                samePoint(pieces[i].mid, pieces[j].mid))
                pieces[j].keep = false;
        }
    }

    // 4. Chain boundary pieces into closed contours.
    std::vector<Contour> contours;
    std::vector<bool> used(pieces.size(), false);
    for (size_t s = 0; s < pieces.size(); ++s) {
        if (!pieces[s].keep || used[s]) continue;
        std::vector<size_t> chain{ s };
        used[s] = true;
        size_t cur = s;
        bool closed = false;
        while (chain.size() <= pieces.size()) {
            Point2 endPt = pieces[cur].end;
            // Candidates: unused boundary pieces starting here, plus closing the loop onto the
            // first piece. Several only meet at pinch points; prefer the sharpest right turn
            // (it hugs the fill, keeping loops that merely touch separate).
            auto turnTo = [&](const Piece &to) {
                return std::atan2(crossProduct(pieces[cur].endDir, to.startDir), dotProduct(pieces[cur].endDir, to.startDir));
            };
            int next = -1;
            double bestTurn = DBL_MAX;
            if (samePoint(endPt, pieces[s].start)) bestTurn = turnTo(pieces[s]); // next stays -1: close
            for (size_t c = 0; c < pieces.size(); ++c) {
                if (!pieces[c].keep || used[c] || !samePoint(pieces[c].start, endPt)) continue;
                double turn = turnTo(pieces[c]);
                if (turn < bestTurn) { bestTurn = turn; next = (int) c; }
            }
            if (next < 0) {
                if (bestTurn < DBL_MAX) { closed = true; break; }
                // Dangling boundary piece. A sub-texel curl or sliver left over from a near-miss
                // self-intersection is harmless to discard; anything larger means the geometry
                // is inconsistent and we give up on this shape.
                double chainLen = 0;
                for (size_t idx : chain) chainLen += polylineLength(pieces[idx].seg.get());
                TRACE("dangling piece edge=%d end=(%.6f,%.6f) chain=%d len=%.3g\n", pieces[cur].parent, endPt.x, endPt.y, (int) chain.size(), chainLen);
                if (chainLen < MAX_ORPHAN_LEN) { chain.clear(); break; }
                return RESOLVE_FAILED;
            }
            // Snap the join exactly.
            setStartPoint(pieces[next].seg.get(), endPt);
            pieces[next].refresh();
            used[next] = true;
            chain.push_back(next);
            cur = next;
        }
        if (chain.empty()) continue; // discarded orphan
        if (!closed) return RESOLVE_FAILED;
        setEndPoint(pieces[cur].seg.get(), pieces[s].start);
        Contour contour;
        for (size_t idx : chain) contour.addEdge(EdgeHolder(pieces[idx].seg.release()));
        if (std::fabs(contourArea(contour)) >= MIN_CONTOUR_AREA)
            contours.push_back(std::move(contour));
    }

    shape.contours = std::move(contours);
    return RESOLVE_RESOLVED;
}

} // namespace pcmsdf
