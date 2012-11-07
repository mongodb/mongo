// Copyright 2005 Google Inc. All Rights Reserved.

#include "s2.h"

#include "base/integral_types.h"
#include "base/logging.h"
#include "util/math/matrix3x3-inl.h"
#include "util/math/vector2-inl.h"

// Define storage for header file constants (the values are not needed
// here for integral constants).

int const S2::kSwapMask = 0x01;
int const S2::kInvertMask = 0x02;
int const S2::kMaxCellLevel = 30;
double const S2::kMaxDetError = 0.8e-15;  // 14 * (2**-54)
// Enable debugging checks in s2 code?
bool const S2::debug = DEBUG_MODE;

COMPILE_ASSERT(S2::kSwapMask == 0x01 && S2::kInvertMask == 0x02,
               masks_changed);

static const uint32 MIX32 = 0x12b9b0a1UL;


HASH_NAMESPACE_START
// The hash function due to Bob Jenkins (see
// http://burtleburtle.net/bob/hash/index.html).
static inline void mix(uint32& a, uint32& b, uint32& c) {     // 32bit version
  a -= b; a -= c; a ^= (c>>13);
  b -= c; b -= a; b ^= (a<<8);
  c -= a; c -= b; c ^= (b>>13);
  a -= b; a -= c; a ^= (c>>12);
  b -= c; b -= a; b ^= (a<<16);
  c -= a; c -= b; c ^= (b>>5);
  a -= b; a -= c; a ^= (c>>3);
  b -= c; b -= a; b ^= (a<<10);
  c -= a; c -= b; c ^= (b>>15);
}

inline uint32 CollapseZero(uint32 bits) {
  // IEEE 754 has two representations for zero, positive zero and negative
  // zero.  These two values compare as equal, and therefore we need them to
  // hash to the same value.
  //
  // We handle this by simply clearing the top bit of every 32-bit value,
  // which clears the sign bit on both big-endian and little-endian
  // architectures.  This creates some additional hash collisions between
  // points that differ only in the sign of their components, but this is
  // rarely a problem with real data.
  //
  // The obvious alternative is to explicitly map all occurrences of positive
  // zero to negative zero (or vice versa), but this is more expensive and
  // makes the average case slower.
  //
  // We also mask off the low-bit because we've seen differences in
  // some floating point operations (specifically 'fcos' on i386)
  // between different implementations of the same architecure
  // (e.g. 'Xeon 5345' vs. 'Opteron 270').  It's unknown how many bits
  // of mask are sufficient to cover real world cases, but the intent
  // is to be as conservative as possible in discarding bits.

  return bits & 0x7ffffffe;
}

size_t makeHash(S2Point const& p) {
  // This function is significantly faster than calling HashTo32().
  uint32 const* data = reinterpret_cast<uint32 const*>(p.Data());
  DCHECK_EQ((6 * sizeof(*data)), sizeof(p));

  // We call CollapseZero() on every 32-bit chunk to avoid having endian
  // dependencies.
  uint32 a = CollapseZero(data[0]);
  uint32 b = CollapseZero(data[1]);
  uint32 c = CollapseZero(data[2]) + 0x12b9b0a1UL;  // An arbitrary number
  mix(a, b, c);
  a += CollapseZero(data[3]);
  b += CollapseZero(data[4]);
  c += CollapseZero(data[5]);
  mix(a, b, c);
  return c;
}

size_t hash<S2Point>::operator()(S2Point const& p) const {
  return makeHash(p);
}

HASH_NAMESPACE_END

bool S2::IsUnitLength(S2Point const& p) {
  return fabs(p.Norm2() - 1) <= 1e-15;
}

S2Point S2::Ortho(S2Point const& a) {
#ifdef S2_TEST_DEGENERACIES
  // Vector3::Ortho() always returns a point on the X-Y, Y-Z, or X-Z planes.
  // This leads to many more degenerate cases in polygon operations.
  return a.Ortho();
#else
  int k = a.LargestAbsComponent() - 1;
  if (k < 0) k = 2;
  S2Point temp(0.012, 0.0053, 0.00457);
  temp[k] = 1;
  return a.CrossProd(temp).Normalize();
#endif
}

void S2::GetFrame(S2Point const& z, Matrix3x3_d* m) {
  DCHECK(IsUnitLength(z));
  m->SetCol(2, z);
  m->SetCol(1, Ortho(z));
  m->SetCol(0, m->Col(1).CrossProd(z));  // Already unit-length.
}

S2Point S2::ToFrame(Matrix3x3_d const& m, S2Point const& p) {
  // The inverse of an orthonormal matrix is its transpose.
  return m.Transpose() * p;
}

S2Point S2::FromFrame(Matrix3x3_d const& m, S2Point const& q) {
  return m * q;
}

bool S2::ApproxEquals(S2Point const& a, S2Point const& b, double max_error) {
  return a.Angle(b) <= max_error;
}

S2Point S2::RobustCrossProd(S2Point const& a, S2Point const& b) {
  // The direction of a.CrossProd(b) becomes unstable as (a + b) or (a - b)
  // approaches zero.  This leads to situations where a.CrossProd(b) is not
  // very orthogonal to "a" and/or "b".  We could fix this using Gram-Schmidt,
  // but we also want b.RobustCrossProd(a) == -a.RobustCrossProd(b).
  //
  // The easiest fix is to just compute the cross product of (b+a) and (b-a).
  // Mathematically, this cross product is exactly twice the cross product of
  // "a" and "b", but it has the numerical advantage that (b+a) and (b-a)
  // are always perpendicular (since "a" and "b" are unit length).  This
  // yields a result that is nearly orthogonal to both "a" and "b" even if
  // these two values differ only in the lowest bit of one component.

  DCHECK(IsUnitLength(a));
  DCHECK(IsUnitLength(b));
  S2Point x = (b + a).CrossProd(b - a);
  if (x != S2Point(0, 0, 0)) return x;

  // The only result that makes sense mathematically is to return zero, but
  // we find it more convenient to return an arbitrary orthogonal vector.
  return Ortho(a);
}

bool S2::SimpleCCW(S2Point const& a, S2Point const& b, S2Point const& c) {
  // We compute the signed volume of the parallelepiped ABC.  The usual
  // formula for this is (AxB).C, but we compute it here using (CxA).B
  // in order to ensure that ABC and CBA are not both CCW.  This follows
  // from the following identities (which are true numerically, not just
  // mathematically):
  //
  //     (1) x.CrossProd(y) == -(y.CrossProd(x))
  //     (2) (-x).DotProd(y) == -(x.DotProd(y))

  return c.CrossProd(a).DotProd(b) > 0;
}

int S2::RobustCCW(S2Point const& a, S2Point const& b, S2Point const& c) {
  // We don't need RobustCrossProd() here because RobustCCW() does its own
  // error estimation and calls ExpensiveCCW() if there is any uncertainty
  // about the result.
  return RobustCCW(a, b, c, a.CrossProd(b));
}

// Below we define two versions of ExpensiveCCW().  The first version uses
// arbitrary-precision arithmetic (MPFloat) and the "simulation of simplicity"
// technique.  It is completely robust (i.e., it returns consistent results
// for all possible inputs).  The second version uses normal double-precision
// arithmetic.  It is numerically stable and handles many degeneracies well,
// but it is not perfectly robust.  It exists mainly for testing purposes, so
// that we can verify that certain tests actually require the more advanced
// techniques implemented by the first version.

#undef SIMULATION_OF_SIMPLICITY
#ifdef SIMULATION_OF_SIMPLICITY

// Below we define a floating-point type with enough precision so that it can
// represent the exact determinant of any 3x3 matrix of floating-point
// numbers.  We support two options: MPFloat (which is based on MPFR and is
// therefore subject to an LGPL license) and ExactFloat (which is based on the
// OpenSSL Bignum library and therefore has a permissive BSD-style license).

#ifdef S2_USE_EXACTFLOAT

// ExactFloat only supports exact calculations with floating-point numbers.
#include "util/math/exactfloat/exactfloat.h"

#else  // S2_USE_EXACTFLOAT

// MPFloat requires a "maximum precision" to be specified.
//
// To figure out how much precision we need, first observe that any double
// precision number can be represented as an integer by multiplying it by
// 2**1074.  This is because the minimum exponent is -1022, and denormalized
// numbers have 52 bits after the leading "0".  On the other hand, the largest
// double precision value has the form 1.x * (2**1023), which is a 1024-bit
// integer.  Therefore any double precision value can be represented as a
// (1074 + 1024) = 2098 bit integer.
//
// A 3x3 determinant is computed by adding together 6 values, each of which is
// the product of 3 of the input values.  When an m-bit integer is multiplied
// by an n-bit integer, the result has at most (m+n) bits.  When "k" m-bit
// integers are added together, the result has at most m + ceil(log2(k)) bits.
// Therefore the determinant of any 3x3 matrix can be represented exactly
// using no more than (3*2098)+3 = 6297 bits.
//
// Note that MPFloat only uses as much precision as required to compute the
// exact result, and that typically far fewer bits of precision are used.  The
// worst-case estimate above is only achieved for a matrix where every row
// contains both the maximum and minimum possible double precision values
// (i.e. approximately 1e308 and 1e-323).  For randomly chosen unit-length
// vectors, the average case uses only about 200 bits of precision.

// The maximum precision must be at least (6297 + 1) so that we can assert
// that the result of the determinant calculation is exact (by checking that
// the actual precision of the result is less than the maximum precision
// specified).

#include "util/math/mpfloat/mpfloat.h"
typedef MPFloat<6300> ExactFloat;

#endif  // S2_USE_EXACTFLOAT

typedef Vector3<ExactFloat> Vector3_xf;

// The following function returns the sign of the determinant of three points
// A, B, C under a model where every possible S2Point is slightly perturbed by
// a unique infinitesmal amount such that no three perturbed points are
// collinear and no four points are coplanar.  The perturbations are so small
// that they do not change the sign of any determinant that was non-zero
// before the perturbations, and therefore can be safely ignored unless the
// determinant of three points is exactly zero (using multiple-precision
// arithmetic).
//
// Since the symbolic perturbation of a given point is fixed (i.e., the
// perturbation is the same for all calls to this method and does not depend
// on the other two arguments), the results of this method are always
// self-consistent.  It will never return results that would correspond to an
// "impossible" configuration of non-degenerate points.
//
// Requirements:
//   The 3x3 determinant of A, B, C must be exactly zero.
//   The points must be distinct, with A < B < C in lexicographic order.
//
// Returns:
//   +1 or -1 according to the sign of the determinant after the symbolic
// perturbations are taken into account.
//
// Reference:
//   "Simulation of Simplicity" (Edelsbrunner and Muecke, ACM Transactions on
//   Graphics, 1990).
//
static int SymbolicallyPerturbedCCW(
    Vector3_xf const& a, Vector3_xf const& b,
    Vector3_xf const& c, Vector3_xf const& b_cross_c) {
  // This method requires that the points are sorted in lexicographically
  // increasing order.  This is because every possible S2Point has its own
  // symbolic perturbation such that if A < B then the symbolic perturbation
  // for A is much larger than the perturbation for B.
  //
  // Alternatively, we could sort the points in this method and keep track of
  // the sign of the permutation, but it is more efficient to do this before
  // converting the inputs to the multi-precision representation, and this
  // also lets us re-use the result of the cross product B x C.
  DCHECK(a < b && b < c);

  // Every input coordinate x[i] is assigned a symbolic perturbation dx[i].
  // We then compute the sign of the determinant of the perturbed points,
  // i.e.
  //               | a[0]+da[0]  a[1]+da[1]  a[2]+da[2] |
  //               | b[0]+db[0]  b[1]+db[1]  b[2]+db[2] |
  //               | c[0]+dc[0]  c[1]+dc[1]  c[2]+dc[2] |
  //
  // The perturbations are chosen such that
  //
  //   da[2] > da[1] > da[0] > db[2] > db[1] > db[0] > dc[2] > dc[1] > dc[0]
  //
  // where each perturbation is so much smaller than the previous one that we
  // don't even need to consider it unless the coefficients of all previous
  // perturbations are zero.  In fact, it is so small that we don't need to
  // consider it unless the coefficient of all products of the previous
  // perturbations are zero.  For example, we don't need to consider the
  // coefficient of db[1] unless the coefficient of db[2]*da[0] is zero.
  //
  // The follow code simply enumerates the coefficients of the perturbations
  // (and products of perturbations) that appear in the determinant above, in
  // order of decreasing perturbation magnitude.  The first non-zero
  // coefficient determines the sign of the result.  The easiest way to
  // enumerate the coefficients in the correct order is to pretend that each
  // perturbation is some tiny value "eps" raised to a power of two:
  //
  // eps**    1      2      4      8     16     32     64     128    256
  //        da[2]  da[1]  da[0]  db[2]  db[1]  db[0]  dc[2]  dc[1]  dc[0]
  //
  // Essentially we can then just count in binary and test the corresponding
  // subset of perturbations at each step.  So for example, we must test the
  // coefficient of db[2]*da[0] before db[1] because eps**12 > eps**16.
  //
  // Of course, not all products of these perturbations appear in the
  // determinant above, since the determinant only contains the products of
  // elements in distinct rows and columns.  Thus we don't need to consider
  // da[2]*da[1], db[1]*da[1], etc.  Furthermore, sometimes different pairs of
  // perturbations have the same coefficient in the determinant; for example,
  // da[1]*db[0] and db[1]*da[0] have the same coefficient (c[2]).  Therefore
  // we only need to test this coefficient the first time we encounter it in
  // the binary order above (which will be db[1]*da[0]).
  //
  // The sequence of tests below also appears in Table 4-ii of the paper
  // referenced above, if you just want to look it up, with the following
  // translations: [a,b,c] -> [i,j,k] and [0,1,2] -> [1,2,3].  Also note that
  // some of the signs are different because the opposite cross product is
  // used (e.g., B x C rather than C x B).

  int det_sign = b_cross_c[2].sgn();           // da[2]
  if (det_sign != 0) return det_sign;
  det_sign = b_cross_c[1].sgn();               // da[1]
  if (det_sign != 0) return det_sign;
  det_sign = b_cross_c[0].sgn();               // da[0]
  if (det_sign != 0) return det_sign;

  det_sign = (c[0]*a[1] - c[1]*a[0]).sgn();    // db[2]
  if (det_sign != 0) return det_sign;
  det_sign = c[0].sgn();                       // db[2] * da[1]
  if (det_sign != 0) return det_sign;
  det_sign = -(c[1].sgn());                    // db[2] * da[0]
  if (det_sign != 0) return det_sign;
  det_sign = (c[2]*a[0] - c[0]*a[2]).sgn();    // db[1]
  if (det_sign != 0) return det_sign;
  det_sign = c[2].sgn();                       // db[1] * da[0]
  if (det_sign != 0) return det_sign;
  // The following test is listed in the paper, but it is redundant because
  // the previous tests guarantee that C == (0, 0, 0).
  DCHECK_EQ(0, (c[1]*a[2] - c[2]*a[1]).sgn()); // db[0]

  det_sign = (a[0]*b[1] - a[1]*b[0]).sgn();    // dc[2]
  if (det_sign != 0) return det_sign;
  det_sign = -(b[0].sgn());                    // dc[2] * da[1]
  if (det_sign != 0) return det_sign;
  det_sign = b[1].sgn();                       // dc[2] * da[0]
  if (det_sign != 0) return det_sign;
  det_sign = a[0].sgn();                       // dc[2] * db[1]
  if (det_sign != 0) return det_sign;
  return 1;                                    // dc[2] * db[1] * da[0]
}

int S2::ExpensiveCCW(S2Point const& a, S2Point const& b, S2Point const& c) {
  // Return zero if and only if two points are the same.  This ensures (1).
  if (a == b || b == c || c == a) return 0;

  // Sort the three points in lexicographic order, keeping track of the sign
  // of the permutation.  (Each exchange inverts the sign of the determinant.)
  int perm_sign = 1;
  S2Point pa = a, pb = b, pc = c;
  if (pa > pb) { swap(pa, pb); perm_sign = -perm_sign; }
  if (pb > pc) { swap(pb, pc); perm_sign = -perm_sign; }
  if (pa > pb) { swap(pa, pb); perm_sign = -perm_sign; }
  DCHECK(pa < pb && pb < pc);

  // Construct multiple-precision versions of the sorted points and compute
  // their exact 3x3 determinant.
  Vector3_xf xa = Vector3_xf::Cast(pa);
  Vector3_xf xb = Vector3_xf::Cast(pb);
  Vector3_xf xc = Vector3_xf::Cast(pc);
  Vector3_xf xb_cross_xc = xb.CrossProd(xc);
  ExactFloat det = xa.DotProd(xb_cross_xc);

  // The precision of ExactFloat is high enough that the result should always
  // be exact (no rounding was performed).
  DCHECK(!det.is_nan());
  DCHECK_LT(det.prec(), det.max_prec());

  // If the exact determinant is non-zero, we're done.
  int det_sign = det.sgn();
  if (det_sign == 0) {
    // Otherwise, we need to resort to symbolic perturbations to resolve the
    // sign of the determinant.
    det_sign = SymbolicallyPerturbedCCW(xa, xb, xc, xb_cross_xc);
  }
  DCHECK(det_sign != 0);
  return perm_sign * det_sign;
}

#else  // SIMULATION_OF_SIMPLICITY

static inline int PlanarCCW(Vector2_d const& a, Vector2_d const& b) {
  // Return +1 if the edge AB is CCW around the origin, etc.
  double sab = (a.DotProd(b) > 0) ? -1 : 1;
  Vector2_d vab = a + sab * b;
  double da = a.Norm2();
  double db = b.Norm2();
  double sign;
  if (da < db || (da == db && a < b)) {
    sign = a.CrossProd(vab) * sab;
  } else {
    sign = vab.CrossProd(b);
  }
  if (sign > 0) return 1;
  if (sign < 0) return -1;
  return 0;
}

static inline int PlanarOrderedCCW(Vector2_d const& a, Vector2_d const& b,
                                   Vector2_d const& c) {
  int sum = 0;
  sum += PlanarCCW(a, b);
  sum += PlanarCCW(b, c);
  sum += PlanarCCW(c, a);
  if (sum > 0) return 1;
  if (sum < 0) return -1;
  return 0;
}

int S2::ExpensiveCCW(S2Point const& a, S2Point const& b, S2Point const& c) {
  // Return zero if and only if two points are the same.  This ensures (1).
  if (a == b || b == c || c == a) return 0;

  // Now compute the determinant in a stable way.  Since all three points are
  // unit length and we know that the determinant is very close to zero, this
  // means that points are very nearly collinear.  Furthermore, the most common
  // situation is where two points are nearly identical or nearly antipodal.
  // To get the best accuracy in this situation, it is important to
  // immediately reduce the magnitude of the arguments by computing either
  // A+B or A-B for each pair of points.  Note that even if A and B differ
  // only in their low bits, A-B can be computed very accurately.  On the
  // other hand we can't accurately represent an arbitrary linear combination
  // of two vectors as would be required for Gaussian elimination.  The code
  // below chooses the vertex opposite the longest edge as the "origin" for
  // the calculation, and computes the different vectors to the other two
  // vertices.  This minimizes the sum of the lengths of these vectors.
  //
  // This implementation is very stable numerically, but it still does not
  // return consistent results in all cases.  For example, if three points are
  // spaced far apart from each other along a great circle, the sign of the
  // result will basically be random (although it will still satisfy the
  // conditions documented in the header file).  The only way to return
  // consistent results in all cases is to compute the result using
  // multiple-precision arithmetic.  I considered using the Gnu MP library,
  // but this would be very expensive (up to 2000 bits of precision may be
  // needed to store the intermediate results) and seems like overkill for
  // this problem.  The MP library is apparently also quite particular about
  // compilers and compilation options and would be a pain to maintain.

  // We want to handle the case of nearby points and nearly antipodal points
  // accurately, so determine whether A+B or A-B is smaller in each case.
  double sab = (a.DotProd(b) > 0) ? -1 : 1;
  double sbc = (b.DotProd(c) > 0) ? -1 : 1;
  double sca = (c.DotProd(a) > 0) ? -1 : 1;
  S2Point vab = a + sab * b;
  S2Point vbc = b + sbc * c;
  S2Point vca = c + sca * a;
  double dab = vab.Norm2();
  double dbc = vbc.Norm2();
  double dca = vca.Norm2();

  // Sort the difference vectors to find the longest edge, and use the
  // opposite vertex as the origin.  If two difference vectors are the same
  // length, we break ties deterministically to ensure that the symmetry
  // properties guaranteed in the header file will be true.
  double sign;
  if (dca < dbc || (dca == dbc && a < b)) {
    if (dab < dbc || (dab == dbc && a < c)) {
      // The "sab" factor converts A +/- B into B +/- A.
      sign = vab.CrossProd(vca).DotProd(a) * sab;  // BC is longest edge
    } else {
      sign = vca.CrossProd(vbc).DotProd(c) * sca;  // AB is longest edge
    }
  } else {
    if (dab < dca || (dab == dca && b < c)) {
      sign = vbc.CrossProd(vab).DotProd(b) * sbc;  // CA is longest edge
    } else {
      sign = vca.CrossProd(vbc).DotProd(c) * sca;  // AB is longest edge
    }
  }
  if (sign > 0) return 1;
  if (sign < 0) return -1;

  // The points A, B, and C are numerically indistinguishable from coplanar.
  // This may be due to roundoff error, or the points may in fact be exactly
  // coplanar.  We handle this situation by perturbing all of the points by a
  // vector (eps, eps**2, eps**3) where "eps" is an infinitesmally small
  // positive number (e.g. 1 divided by a googolplex).  The perturbation is
  // done symbolically, i.e. we compute what would happen if the points were
  // perturbed by this amount.  It turns out that this is equivalent to
  // checking whether the points are ordered CCW around the origin first in
  // the Y-Z plane, then in the Z-X plane, and then in the X-Y plane.

  int ccw = PlanarOrderedCCW(Vector2_d(a.y(), a.z()), Vector2_d(b.y(), b.z()),
                             Vector2_d(c.y(), c.z()));
  if (ccw == 0) {
    ccw = PlanarOrderedCCW(Vector2_d(a.z(), a.x()), Vector2_d(b.z(), b.x()),
                           Vector2_d(c.z(), c.x()));
    if (ccw == 0) {
      ccw = PlanarOrderedCCW(Vector2_d(a.x(), a.y()), Vector2_d(b.x(), b.y()),
                             Vector2_d(c.x(), c.y()));
      // There are a few cases where "ccw" may still be zero despite our best
      // efforts.  For example, two input points may be exactly proportional
      // to each other (where both still satisfy IsNormalized()).
    }
  }
  return ccw;
}

#endif  // SIMULATION_OF_SIMPLICITY

double S2::Angle(S2Point const& a, S2Point const& b, S2Point const& c) {
  return RobustCrossProd(a, b).Angle(RobustCrossProd(c, b));
}

double S2::TurnAngle(S2Point const& a, S2Point const& b, S2Point const& c) {
  // This is a bit less efficient because we compute all 3 cross products, but
  // it ensures that TurnAngle(a,b,c) == -TurnAngle(c,b,a) for all a,b,c.
  double angle = RobustCrossProd(b, a).Angle(RobustCrossProd(c, b));
  return (RobustCCW(a, b, c) > 0) ? angle : -angle;
}

double S2::Area(S2Point const& a, S2Point const& b, S2Point const& c) {
  DCHECK(IsUnitLength(a));
  DCHECK(IsUnitLength(b));
  DCHECK(IsUnitLength(c));
  // This method is based on l'Huilier's theorem,
  //
  //   tan(E/4) = sqrt(tan(s/2) tan((s-a)/2) tan((s-b)/2) tan((s-c)/2))
  //
  // where E is the spherical excess of the triangle (i.e. its area),
  //       a, b, c, are the side lengths, and
  //       s is the semiperimeter (a + b + c) / 2 .
  //
  // The only significant source of error using l'Huilier's method is the
  // cancellation error of the terms (s-a), (s-b), (s-c).  This leads to a
  // *relative* error of about 1e-16 * s / min(s-a, s-b, s-c).  This compares
  // to a relative error of about 1e-15 / E using Girard's formula, where E is
  // the true area of the triangle.  Girard's formula can be even worse than
  // this for very small triangles, e.g. a triangle with a true area of 1e-30
  // might evaluate to 1e-5.
  //
  // So, we prefer l'Huilier's formula unless dmin < s * (0.1 * E), where
  // dmin = min(s-a, s-b, s-c).  This basically includes all triangles
  // except for extremely long and skinny ones.
  //
  // Since we don't know E, we would like a conservative upper bound on
  // the triangle area in terms of s and dmin.  It's possible to show that
  // E <= k1 * s * sqrt(s * dmin), where k1 = 2*sqrt(3)/Pi (about 1).
  // Using this, it's easy to show that we should always use l'Huilier's
  // method if dmin >= k2 * s^5, where k2 is about 1e-2.  Furthermore,
  // if dmin < k2 * s^5, the triangle area is at most k3 * s^4, where
  // k3 is about 0.1.  Since the best case error using Girard's formula
  // is about 1e-15, this means that we shouldn't even consider it unless
  // s >= 3e-4 or so.

  // We use volatile doubles to force the compiler to truncate all of these
  // quantities to 64 bits.  Otherwise it may compute a value of dmin > 0
  // simply because it chose to spill one of the intermediate values to
  // memory but not one of the others.
  volatile double sa = b.Angle(c);
  volatile double sb = c.Angle(a);
  volatile double sc = a.Angle(b);
  volatile double s = 0.5 * (sa + sb + sc);
  if (s >= 3e-4) {
    // Consider whether Girard's formula might be more accurate.
    double s2 = s * s;
    double dmin = s - max(sa, max(sb, sc));
    if (dmin < 1e-2 * s * s2 * s2) {
      // This triangle is skinny enough to consider Girard's formula.
      double area = GirardArea(a, b, c);
      if (dmin < s * (0.1 * area)) return area;
    }
  }
  // Use l'Huilier's formula.
  return 4 * atan(sqrt(max(0.0, tan(0.5 * s) * tan(0.5 * (s - sa)) *
                           tan(0.5 * (s - sb)) * tan(0.5 * (s - sc)))));
}

double S2::GirardArea(S2Point const& a, S2Point const& b, S2Point const& c) {
  // This is equivalent to the usual Girard's formula but is slightly
  // more accurate, faster to compute, and handles a == b == c without
  // a special case.  The use of RobustCrossProd() makes it much more
  // accurate when two vertices are nearly identical or antipodal.

  S2Point ab = RobustCrossProd(a, b);
  S2Point bc = RobustCrossProd(b, c);
  S2Point ac = RobustCrossProd(a, c);
  return max(0.0, ab.Angle(ac) - ab.Angle(bc) + bc.Angle(ac));
}

double S2::SignedArea(S2Point const& a, S2Point const& b, S2Point const& c) {
  return Area(a, b, c) * RobustCCW(a, b, c);
}

S2Point S2::PlanarCentroid(S2Point const& a, S2Point const& b,
                           S2Point const& c) {
  return (1./3) * (a + b + c);
}

S2Point S2::TrueCentroid(S2Point const& a, S2Point const& b,
                         S2Point const& c) {
  DCHECK(IsUnitLength(a));
  DCHECK(IsUnitLength(b));
  DCHECK(IsUnitLength(c));

  // I couldn't find any references for computing the true centroid of a
  // spherical triangle...  I have a truly marvellous demonstration of this
  // formula which this margin is too narrow to contain :)

  // Use Angle() in order to get accurate results for small triangles.
  double angle_a = b.Angle(c);
  double angle_b = c.Angle(a);
  double angle_c = a.Angle(b);
  double ra = (angle_a == 0) ? 1 : (angle_a / sin(angle_a));
  double rb = (angle_b == 0) ? 1 : (angle_b / sin(angle_b));
  double rc = (angle_c == 0) ? 1 : (angle_c / sin(angle_c));

  // Now compute a point M such that:
  //
  //  [Ax Ay Az] [Mx]                       [ra]
  //  [Bx By Bz] [My]  = 0.5 * det(A,B,C) * [rb]
  //  [Cx Cy Cz] [Mz]                       [rc]
  //
  // To improve the numerical stability we subtract the first row (A) from the
  // other two rows; this reduces the cancellation error when A, B, and C are
  // very close together.  Then we solve it using Cramer's rule.
  //
  // TODO(user): This code still isn't as numerically stable as it could be.
  // The biggest potential improvement is to compute B-A and C-A more
  // accurately so that (B-A)x(C-A) is always inside triangle ABC.
  S2Point x(a.x(), b.x() - a.x(), c.x() - a.x());
  S2Point y(a.y(), b.y() - a.y(), c.y() - a.y());
  S2Point z(a.z(), b.z() - a.z(), c.z() - a.z());
  S2Point r(ra, rb - ra, rc - ra);
  return 0.5 * S2Point(y.CrossProd(z).DotProd(r),
                       z.CrossProd(x).DotProd(r),
                       x.CrossProd(y).DotProd(r));
}

bool S2::OrderedCCW(S2Point const& a, S2Point const& b, S2Point const& c,
                    S2Point const& o) {
  // The last inequality below is ">" rather than ">=" so that we return true
  // if A == B or B == C, and otherwise false if A == C.  Recall that
  // RobustCCW(x,y,z) == -RobustCCW(z,y,x) for all x,y,z.

  int sum = 0;
  if (RobustCCW(b, o, a) >= 0) ++sum;
  if (RobustCCW(c, o, b) >= 0) ++sum;
  if (RobustCCW(a, o, c) > 0) ++sum;
  return sum >= 2;
}

// kIJtoPos[orientation][ij] -> pos
int const S2::kIJtoPos[4][4] = {
  // (0,0) (0,1) (1,0) (1,1)
  {     0,    1,    3,    2  },  // canonical order
  {     0,    3,    1,    2  },  // axes swapped
  {     2,    3,    1,    0  },  // bits inverted
  {     2,    1,    3,    0  },  // swapped & inverted
};

// kPosToIJ[orientation][pos] -> ij
int const S2::kPosToIJ[4][4] = {
  // 0  1  2  3
  {  0, 1, 3, 2 },    // canonical order:    (0,0), (0,1), (1,1), (1,0)
  {  0, 2, 3, 1 },    // axes swapped:       (0,0), (1,0), (1,1), (0,1)
  {  3, 2, 0, 1 },    // bits inverted:      (1,1), (1,0), (0,0), (0,1)
  {  3, 1, 0, 2 },    // swapped & inverted: (1,1), (0,1), (0,0), (1,0)
};

// kPosToOrientation[pos] -> orientation_modifier
int const S2::kPosToOrientation[4] = {
  kSwapMask,
  0,
  0,
  kInvertMask + kSwapMask,
};

// All of the values below were obtained by a combination of hand analysis and
// Mathematica.  In general, S2_TAN_PROJECTION produces the most uniform
// shapes and sizes of cells, S2_LINEAR_PROJECTION is considerably worse, and
// S2_QUADRATIC_PROJECTION is somewhere in between (but generally closer to
// the tangent projection than the linear one).

S2::LengthMetric const S2::kMinAngleSpan(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 1.0 :                      // 1.000
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI / 2 :                    // 1.571
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 4. / 3 :                // 1.333
    0);

S2::LengthMetric const S2::kMaxAngleSpan(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 2 :                        // 2.000
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI / 2 :                    // 1.571
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 1.704897179199218452 :  // 1.705
    0);

S2::LengthMetric const S2::kAvgAngleSpan(M_PI / 2);                    // 1.571
// This is true for all projections.

S2::LengthMetric const S2::kMinWidth(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? sqrt(2. / 3) :             // 0.816
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI / (2 * sqrt(2)) :        // 1.111
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 2 * sqrt(2) / 3 :       // 0.943
    0);

S2::LengthMetric const S2::kMaxWidth(S2::kMaxAngleSpan.deriv());
// This is true for all projections.

S2::LengthMetric const S2::kAvgWidth(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 1.411459345844456965 :     // 1.411
    S2_PROJECTION == S2_TAN_PROJECTION ? 1.437318638925160885 :        // 1.437
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 1.434523672886099389 :  // 1.435
    0);

S2::LengthMetric const S2::kMinEdge(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 2 * sqrt(2) / 3 :          // 0.943
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI / (2 * sqrt(2)) :        // 1.111
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 2 * sqrt(2) / 3 :       // 0.943
    0);

S2::LengthMetric const S2::kMaxEdge(S2::kMaxAngleSpan.deriv());
// This is true for all projections.

S2::LengthMetric const S2::kAvgEdge(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 1.440034192955603643 :     // 1.440
    S2_PROJECTION == S2_TAN_PROJECTION ? 1.461667032546739266 :        // 1.462
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 1.459213746386106062 :  // 1.459
    0);

S2::LengthMetric const S2::kMinDiag(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 2 * sqrt(2) / 3 :          // 0.943
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI * sqrt(2) / 3 :          // 1.481
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 8 * sqrt(2) / 9 :       // 1.257
    0);

S2::LengthMetric const S2::kMaxDiag(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 2 * sqrt(2) :              // 2.828
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI * sqrt(2. / 3) :         // 2.565
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 2.438654594434021032 :  // 2.439
    0);

S2::LengthMetric const S2::kAvgDiag(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 2.031817866418812674 :     // 2.032
    S2_PROJECTION == S2_TAN_PROJECTION ? 2.063623197195635753 :        // 2.064
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 2.060422738998471683 :  // 2.060
    0);

S2::AreaMetric const S2::kMinArea(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 4 / (3 * sqrt(3)) :        // 0.770
    S2_PROJECTION == S2_TAN_PROJECTION ? (M_PI*M_PI) / (4*sqrt(2)) :   // 1.745
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 8 * sqrt(2) / 9 :       // 1.257
    0);

S2::AreaMetric const S2::kMaxArea(
    S2_PROJECTION == S2_LINEAR_PROJECTION ? 4 :                        // 4.000
    S2_PROJECTION == S2_TAN_PROJECTION ? M_PI * M_PI / 4 :             // 2.467
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 2.635799256963161491 :  // 2.636
    0);

S2::AreaMetric const S2::kAvgArea(4 * M_PI / 6);                       // 2.094
// This is true for all projections.

double const S2::kMaxEdgeAspect = (
    S2_PROJECTION == S2_LINEAR_PROJECTION ? sqrt(2) :                  // 1.414
    S2_PROJECTION == S2_TAN_PROJECTION ?  sqrt(2) :                    // 1.414
    S2_PROJECTION == S2_QUADRATIC_PROJECTION ? 1.442615274452682920 :  // 1.443
    0);

double const S2::kMaxDiagAspect = sqrt(3);                             // 1.732
// This is true for all projections.
