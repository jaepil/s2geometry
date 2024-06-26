// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#include "s2/s2predicates.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ostream>
#include <utility>

#include "absl/log/absl_check.h"
#include "s2/s1chord_angle.h"
#include "s2/s2edge_crossings.h"
#include "s2/s2point.h"
#include "s2/s2predicates_internal.h"
#include "s2/util/math/exactfloat/exactfloat.h"

using std::fabs;
using std::max;
using std::min;
using std::sqrt;

namespace s2pred {

// A predefined S1ChordAngle representing (approximately) 45 degrees.
static const S1ChordAngle k45Degrees = S1ChordAngle::FromLength2(2 - M_SQRT2);

int Sign(const S2Point& a, const S2Point& b, const S2Point& c) {
  // We don't need RobustCrossProd() here because Sign() does its own
  // error estimation and calls ExpensiveSign() if there is any uncertainty
  // about the result.
  return Sign(a, b, c, a.CrossProd(b));
}

// Compute the determinant in a numerically stable way.  Unlike TriageSign(),
// this method can usually compute the correct determinant sign even when all
// three points are as collinear as possible.  For example if three points are
// spaced 1km apart along a random line on the Earth's surface using the
// nearest representable points, there is only a 0.4% chance that this method
// will not be able to find the determinant sign.  The probability of failure
// decreases as the points get closer together; if the collinear points are
// 1 meter apart, the failure rate drops to 0.0004%.
//
// This method could be extended to also handle nearly-antipodal points (and
// in fact an earlier version of this code did exactly that), but antipodal
// points are rare in practice so it seems better to simply fall back to
// exact arithmetic in that case.
int StableSign(const S2Point& a, const S2Point& b, const S2Point& c) {
  Vector3_d ab = b - a;
  Vector3_d bc = c - b;
  Vector3_d ca = a - c;
  double ab2 = ab.Norm2();
  double bc2 = bc.Norm2();
  double ca2 = ca.Norm2();

  // Now compute the determinant ((A-C)x(B-C)).C, where the vertices have been
  // cyclically permuted if necessary so that AB is the longest edge.  (This
  // minimizes the magnitude of cross product.)  At the same time we also
  // compute the maximum error in the determinant.  Using a similar technique
  // to the one used for kMaxDetError, the error is at most
  //
  //   |d| <= (3 + 6/sqrt(3)) * |A-C| * |B-C| * e
  //
  // where e = 0.5 * DBL_EPSILON.  If the determinant magnitude is larger than
  // this value then we know its sign with certainty.
  const double kDetErrorMultiplier = 3.2321 * DBL_EPSILON;  // see above
  double det, max_error;
  if (ab2 >= bc2 && ab2 >= ca2) {
    // AB is the longest edge, so compute (A-C)x(B-C).C.
    det = -(ca.CrossProd(bc).DotProd(c));
    max_error = kDetErrorMultiplier * sqrt(ca2 * bc2);
  } else if (bc2 >= ca2) {
    // BC is the longest edge, so compute (B-A)x(C-A).A.
    det = -(ab.CrossProd(ca).DotProd(a));
    max_error = kDetErrorMultiplier * sqrt(ab2 * ca2);
  } else {
    // CA is the longest edge, so compute (C-B)x(A-B).B.
    det = -(bc.CrossProd(ab).DotProd(b));
    max_error = kDetErrorMultiplier * sqrt(bc2 * ab2);
  }
  // Errors smaller than this value may not be accurate due to underflow.
  const double kMinNoUnderflowError = kDetErrorMultiplier * sqrt(DBL_MIN);
  if (max_error < kMinNoUnderflowError) return 0;

  return (fabs(det) <= max_error) ? 0 : (det > 0) ? 1 : -1;
}

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
int SymbolicallyPerturbedSign(
    const Vector3_xf& a, const Vector3_xf& b,
    const Vector3_xf& c, const Vector3_xf& b_cross_c) {
  // This method requires that the points are sorted in lexicographically
  // increasing order.  This is because every possible S2Point has its own
  // symbolic perturbation such that if A < B then the symbolic perturbation
  // for A is much larger than the perturbation for B.
  //
  // Alternatively, we could sort the points in this method and keep track of
  // the sign of the permutation, but it is more efficient to do this before
  // converting the inputs to the multi-precision representation, and this
  // also lets us re-use the result of the cross product B x C.
  ABSL_DCHECK(a < b && b < c);

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

  int det_sign = b_cross_c[2].sgn();            // da[2]
  if (det_sign != 0) return det_sign;
  det_sign = b_cross_c[1].sgn();                // da[1]
  if (det_sign != 0) return det_sign;
  det_sign = b_cross_c[0].sgn();                // da[0]
  if (det_sign != 0) return det_sign;

  det_sign = (c[0]*a[1] - c[1]*a[0]).sgn();     // db[2]
  if (det_sign != 0) return det_sign;
  det_sign = c[0].sgn();                        // db[2] * da[1]
  if (det_sign != 0) return det_sign;
  det_sign = -(c[1].sgn());                     // db[2] * da[0]
  if (det_sign != 0) return det_sign;
  det_sign = (c[2]*a[0] - c[0]*a[2]).sgn();     // db[1]
  if (det_sign != 0) return det_sign;
  det_sign = c[2].sgn();                        // db[1] * da[0]
  if (det_sign != 0) return det_sign;
  // The following test is listed in the paper, but it is redundant because
  // the previous tests guarantee that C == (0, 0, 0).
  ABSL_DCHECK_EQ(0, (c[1] * a[2] - c[2] * a[1]).sgn());  // db[0]

  det_sign = (a[0]*b[1] - a[1]*b[0]).sgn();     // dc[2]
  if (det_sign != 0) return det_sign;
  det_sign = -(b[0].sgn());                     // dc[2] * da[1]
  if (det_sign != 0) return det_sign;
  det_sign = b[1].sgn();                        // dc[2] * da[0]
  if (det_sign != 0) return det_sign;
  det_sign = a[0].sgn();                        // dc[2] * db[1]
  if (det_sign != 0) return det_sign;
  return 1;                                     // dc[2] * db[1] * da[0]
}

// Compute the determinant using exact arithmetic and/or symbolic
// permutations.  Requires that the three points are distinct.
int ExactSign(const S2Point& a, const S2Point& b, const S2Point& c,
              bool perturb) {
  ABSL_DCHECK(a != b && b != c && c != a);

  // Sort the three points in lexicographic order, keeping track of the sign
  // of the permutation.  (Each exchange inverts the sign of the determinant.)
  int perm_sign = 1;
  const S2Point *pa = &a, *pb = &b, *pc = &c;
  using std::swap;
  if (*pa > *pb) { swap(pa, pb); perm_sign = -perm_sign; }
  if (*pb > *pc) { swap(pb, pc); perm_sign = -perm_sign; }
  if (*pa > *pb) { swap(pa, pb); perm_sign = -perm_sign; }
  ABSL_DCHECK(*pa < *pb && *pb < *pc);

  // Construct multiple-precision versions of the sorted points and compute
  // their exact 3x3 determinant.
  Vector3_xf xa = ToExact(*pa);
  Vector3_xf xb = ToExact(*pb);
  Vector3_xf xc = ToExact(*pc);
  Vector3_xf xb_cross_xc = xb.CrossProd(xc);
  ExactFloat det = xa.DotProd(xb_cross_xc);

  // The precision of ExactFloat is high enough that the result should always
  // be exact (no rounding was performed).
  ABSL_DCHECK(!det.is_nan());
  ABSL_DCHECK_LT(det.prec(), det.max_prec());

  // If the exact determinant is non-zero, we're done.
  int det_sign = det.sgn();
  if (det_sign == 0 && perturb) {
    // Otherwise, we need to resort to symbolic perturbations to resolve the
    // sign of the determinant.
    det_sign = SymbolicallyPerturbedSign(xa, xb, xc, xb_cross_xc);
    ABSL_DCHECK_NE(0, det_sign);
  }
  return perm_sign * det_sign;
}

// ExpensiveSign() uses arbitrary-precision arithmetic and the "simulation of
// simplicity" technique in order to be completely robust (i.e., to return
// consistent results for all possible inputs).
//
// Below we define a floating-point type with enough precision so that it can
// represent the exact determinant of any 3x3 matrix of floating-point
// numbers.  It uses ExactFloat, which is based on the OpenSSL Bignum library
// and therefore has a permissive BSD-style license.  (At one time we also
// supported an option based on MPFR, but that has an LGPL license and is
// therefore not suited for some applications.)

int ExpensiveSign(const S2Point& a, const S2Point& b, const S2Point& c,
                  bool perturb) {
  // Return zero if and only if two points are the same.  This ensures (1).
  if (a == b || b == c || c == a) return 0;

  // Next we try recomputing the determinant still using floating-point
  // arithmetic but in a more precise way.  This is more expensive than the
  // simple calculation done by TriageSign(), but it is still *much* cheaper
  // than using arbitrary-precision arithmetic.  This optimization is able to
  // compute the correct determinant sign in virtually all cases except when
  // the three points are truly collinear (e.g., three points on the equator).
  int det_sign = StableSign(a, b, c);
  if (det_sign != 0) return det_sign;

  // TODO(ericv): Create a templated version of StableSign so that we can
  // retry in "long double" precision before falling back to ExactFloat.

  // TODO(ericv): Optimize ExactFloat so that it stores up to 32 bytes of
  // mantissa inline (without requiring memory allocation).

  // Otherwise fall back to exact arithmetic and symbolic permutations.
  return ExactSign(a, b, c, perturb);
}

bool OrderedCCW(const S2Point& a, const S2Point& b, const S2Point& c,
                const S2Point& o) {
  ABSL_DCHECK(a != o && b != o && c != o);

  // The last inequality below is ">" rather than ">=" so that we return true
  // if A == B or B == C, and otherwise false if A == C.  Recall that
  // Sign(x,y,z) == -Sign(z,y,x) for all x,y,z.

  int sum = 0;
  if (Sign(b, o, a) >= 0) ++sum;
  if (Sign(c, o, b) >= 0) ++sum;
  if (Sign(a, o, c) > 0) ++sum;
  return sum >= 2;
}

// Returns cos(XY), and sets "error" to the maximum error in the result.
// REQUIRES: "x" and "y" satisfy S2::IsNormalized().
inline double GetCosDistance(const S2Point& x, const S2Point& y,
                             double* error) {
  double c = x.DotProd(y);
  *error = 9.5 * DBL_ERR * fabs(c) + 1.5 * DBL_ERR;
  return c;
}

// A high precision "long double" version of the function above.
inline long double GetCosDistance(const Vector3_ld& x, const Vector3_ld& y,
                                  long double* error) {
  // With "long double" precision it is worthwhile to compensate for length
  // errors in "x" and "y", since they are only unit length to within the
  // precision of "double".  (This would also reduce the error constant
  // slightly in the method above but is not worth the additional effort.)
  long double c = x.DotProd(y) / sqrt(x.Norm2() * y.Norm2());
  *error = 7 * LD_ERR * fabs(c) + 1.5 * LD_ERR;
  return c;
}

// Returns sin**2(XY), where XY is the angle between X and Y, and sets "error"
// to the maximum error in the result.
//
// REQUIRES: "x" and "y" satisfy S2::IsNormalized().
inline double GetSin2Distance(const S2Point& x, const S2Point& y,
                              double* error) {
  // The (x-y).CrossProd(x+y) trick eliminates almost all of error due to "x"
  // and "y" being not quite unit length.  This method is extremely accurate
  // for small distances; the *relative* error in the result is O(DBL_ERR) for
  // distances as small as DBL_ERR.
  S2Point n = (x - y).CrossProd(x + y);
  double d2 = 0.25 * n.Norm2();
  *error = ((21 + 4 * sqrt(3)) * DBL_ERR * d2 +
            32 * sqrt(3) * DBL_ERR * DBL_ERR * sqrt(d2) +
            768 * DBL_ERR * DBL_ERR * DBL_ERR * DBL_ERR);
  return d2;
}

// A high precision "long double" version of the function above.
inline long double GetSin2Distance(const Vector3_ld& x, const Vector3_ld& y,
                                   long double* error) {
  // In "long double" precision it is worthwhile to compensate for length
  // errors in "x" and "y", since they are only unit length to within the
  // precision of "double".  Otherwise the "d2" error coefficient below would
  // be (16 * DBL_ERR + (5 + 4 * sqrt(3)) * LD_ERR), which is much larger.
  // (Dividing by the squared norms of "x" and "y" would also reduce the error
  // constant slightly in the double-precision version, but this is not worth
  // the additional effort.)
  Vector3_ld n = (x - y).CrossProd(x + y);
  long double d2 = 0.25 * n.Norm2() / (x.Norm2() * y.Norm2());
  *error = ((13 + 4 * sqrt(3)) * LD_ERR * d2 +
            32 * sqrt(3) * DBL_ERR * LD_ERR * sqrt(d2) +
            768 * DBL_ERR * DBL_ERR * LD_ERR * LD_ERR);
  return d2;
}

template <class T>
int TriageCompareCosDistances(const Vector3<T>& x,
                              const Vector3<T>& a, const Vector3<T>& b) {
  T cos_ax_error, cos_bx_error;
  T cos_ax = GetCosDistance(a, x, &cos_ax_error);
  T cos_bx = GetCosDistance(b, x, &cos_bx_error);
  T diff = cos_ax - cos_bx;
  T error = cos_ax_error + cos_bx_error;
  return (diff > error) ? -1 : (diff < -error) ? 1 : 0;
}

template <class T>
int TriageCompareSin2Distances(const Vector3<T>& x,
                               const Vector3<T>& a, const Vector3<T>& b) {
  T sin2_ax_error, sin2_bx_error;
  T sin2_ax = GetSin2Distance(a, x, &sin2_ax_error);
  T sin2_bx = GetSin2Distance(b, x, &sin2_bx_error);
  T diff = sin2_ax - sin2_bx;
  T error = sin2_ax_error + sin2_bx_error;
  return (diff > error) ? 1 : (diff < -error) ? -1 : 0;
}

int ExactCompareDistances(const Vector3_xf& x,
                          const Vector3_xf& a, const Vector3_xf& b) {
  // This code produces the same result as though all points were reprojected
  // to lie exactly on the surface of the unit sphere.  It is based on testing
  // whether x.DotProd(a.Normalize()) < x.DotProd(b.Normalize()), reformulated
  // so that it can be evaluated using exact arithmetic.
  ExactFloat cos_ax = x.DotProd(a);
  ExactFloat cos_bx = x.DotProd(b);
  // If the two values have different signs, we need to handle that case now
  // before squaring them below.
  int a_sign = cos_ax.sgn(), b_sign = cos_bx.sgn();
  if (a_sign != b_sign) {
    return (a_sign > b_sign) ? -1 : 1;  // If cos(AX) > cos(BX), then AX < BX.
  }
  ExactFloat cmp = cos_bx * cos_bx * a.Norm2() - cos_ax * cos_ax * b.Norm2();
  return a_sign * cmp.sgn();
}

// Given three points such that AX == BX (exactly), returns -1, 0, or +1
// according whether AX < BX, AX == BX, or AX > BX after symbolic
// perturbations are taken into account.
int SymbolicCompareDistances(const S2Point& x,
                             const S2Point& a, const S2Point& b) {
  // Our symbolic perturbation strategy is based on the following model.
  // Similar to "simulation of simplicity", we assign a perturbation to every
  // point such that if A < B, then the symbolic perturbation for A is much,
  // much larger than the symbolic perturbation for B.  We imagine that
  // rather than projecting every point to lie exactly on the unit sphere,
  // instead each point is positioned on its own tiny pedestal that raises it
  // just off the surface of the unit sphere.  This means that the distance AX
  // is actually the true distance AX plus the (symbolic) heights of the
  // pedestals for A and X.  The pedestals are infinitesmally thin, so they do
  // not affect distance measurements except at the two endpoints.  If several
  // points project to exactly the same point on the unit sphere, we imagine
  // that they are placed on separate pedestals placed close together, where
  // the distance between pedestals is much, much less than the height of any
  // pedestal.  (There are a finite number of S2Points, and therefore a finite
  // number of pedestals, so this is possible.)
  //
  // If A < B, then A is on a higher pedestal than B, and therefore AX > BX.
  return (a < b) ? 1 : (a > b) ? -1 : 0;
}

static int CompareSin2Distances(const S2Point& x,
                                const S2Point& a, const S2Point& b) {
  int sign = TriageCompareSin2Distances(x, a, b);
  if (kHasLongDouble && sign == 0) {
    sign = TriageCompareSin2Distances(ToLD(x), ToLD(a), ToLD(b));
  }
  return sign;
}

int CompareDistances(const S2Point& x, const S2Point& a, const S2Point& b) {
  // We start by comparing distances using dot products (i.e., cosine of the
  // angle), because (1) this is the cheapest technique, and (2) it is valid
  // over the entire range of possible angles.  (We can only use the sin^2
  // technique if both angles are less than 90 degrees or both angles are
  // greater than 90 degrees.)
  int sign = TriageCompareCosDistances(x, a, b);
  if (sign != 0) return sign;

  // Optimization for (a == b) to avoid falling back to exact arithmetic.
  if (a == b) return 0;

  // It is much better numerically to compare distances using cos(angle) if
  // the distances are near 90 degrees and sin^2(angle) if the distances are
  // near 0 or 180 degrees.  We only need to check one of the two angles when
  // making this decision because the fact that the test above failed means
  // that angles "a" and "b" are very close together.
  double cos_ax = a.DotProd(x);
  if (cos_ax > M_SQRT1_2) {
    // Angles < 45 degrees.
    sign = CompareSin2Distances(x, a, b);
  } else if (cos_ax < -M_SQRT1_2) {
    // Angles > 135 degrees.  sin^2(angle) is decreasing in this range.
    sign = -CompareSin2Distances(x, a, b);
  } else if (kHasLongDouble) {
    // We've already tried double precision, so continue with "long double".
    sign = TriageCompareCosDistances(ToLD(x), ToLD(a), ToLD(b));
  }
  if (sign != 0) return sign;
  sign = ExactCompareDistances(ToExact(x), ToExact(a), ToExact(b));
  if (sign != 0) return sign;
  return SymbolicCompareDistances(x, a, b);
}

template <class T>
int TriageCompareCosDistance(const Vector3<T>& x, const Vector3<T>& y, T r2) {
  constexpr T T_ERR = rounding_epsilon<T>();
  T cos_xy_error;
  T cos_xy = GetCosDistance(x, y, &cos_xy_error);
  T cos_r = 1 - 0.5 * r2;
  T cos_r_error = 2 * T_ERR * cos_r;
  T diff = cos_xy - cos_r;
  T error = cos_xy_error + cos_r_error;
  return (diff > error) ? -1 : (diff < -error) ? 1 : 0;
}

template <class T>
int TriageCompareSin2Distance(const Vector3<T>& x, const Vector3<T>& y, T r2) {
  ABSL_DCHECK_LT(r2, 2.0);  // Only valid for distance limits < 90 degrees.

  constexpr T T_ERR = rounding_epsilon<T>();
  T sin2_xy_error;
  T sin2_xy = GetSin2Distance(x, y, &sin2_xy_error);
  T sin2_r = r2 * (1 - 0.25 * r2);
  T sin2_r_error = 3 * T_ERR * sin2_r;
  T diff = sin2_xy - sin2_r;
  T error = sin2_xy_error + sin2_r_error;
  return (diff > error) ? 1 : (diff < -error) ? -1 : 0;
}

int ExactCompareDistance(const Vector3_xf& x, const Vector3_xf& y,
                         const ExactFloat& r2) {
  // This code produces the same result as though all points were reprojected
  // to lie exactly on the surface of the unit sphere.  It is based on
  // comparing the cosine of the angle XY (when both points are projected to
  // lie exactly on the sphere) to the given threshold.
  ExactFloat cos_xy = x.DotProd(y);
  ExactFloat cos_r = 1 - 0.5 * r2;
  // If the two values have different signs, we need to handle that case now
  // before squaring them below.
  int xy_sign = cos_xy.sgn(), r_sign = cos_r.sgn();
  if (xy_sign != r_sign) {
    return (xy_sign > r_sign) ? -1 : 1;  // If cos(XY) > cos(r), then XY < r.
  }
  ExactFloat cmp = cos_r * cos_r * x.Norm2() * y.Norm2() - cos_xy * cos_xy;
  return xy_sign * cmp.sgn();
}

int CompareDistance(const S2Point& x, const S2Point& y, S1ChordAngle r) {
  // As with CompareDistances(), we start by comparing dot products because
  // the sin^2 method is only valid when the distance XY and the limit "r" are
  // both less than 90 degrees.
  int sign = TriageCompareCosDistance(x, y, r.length2());
  if (sign != 0) return sign;

  // Optimization for (x == y) to avoid falling back to exact arithmetic.
  if (r.length2() == 0 && x == y) return 0;

  // Unlike with CompareDistances(), it's not worth using the sin^2 method
  // when the distance limit is near 180 degrees because the S1ChordAngle
  // representation itself has has a rounding error of up to 2e-8 radians for
  // distances near 180 degrees.
  if (r < k45Degrees) {
    sign = TriageCompareSin2Distance(x, y, r.length2());
    if (kHasLongDouble && sign == 0) {
      sign = TriageCompareSin2Distance(ToLD(x), ToLD(y), ToLD(r.length2()));
    }
  } else if (kHasLongDouble) {
    sign = TriageCompareCosDistance(ToLD(x), ToLD(y), ToLD(r.length2()));
  }
  if (sign != 0) return sign;
  return ExactCompareDistance(ToExact(x), ToExact(y), r.length2());
}

// Helper function that compares the distance XY against the squared chord
// distance "r2" using the given precision "T".
template <class T>
int TriageCompareDistance(const Vector3<T>& x, const Vector3<T>& y, T r2) {
  // The Sin2 method is much more accurate for small distances, but it is only
  // valid when the actual distance and the distance limit are both less than
  // 90 degrees.  So we always start with the Cos method.
  int sign = TriageCompareCosDistance(x, y, r2);
  if (sign == 0 && r2 < k45Degrees.length2()) {
    sign = TriageCompareSin2Distance(x, y, r2);
  }
  return sign;
}

// Helper function that returns "a0" or "a1", whichever is closer to "x".
// Also returns the squared distance from the returned point to "x" in "ax2".
template <class T>
inline Vector3<T> GetClosestVertex(const Vector3<T>& x, const Vector3<T>& a0,
                                   const Vector3<T>& a1, T* ax2) {
  T a0x2 = (a0 - x).Norm2();
  T a1x2 = (a1 - x).Norm2();
  if (a0x2 < a1x2 || (a0x2 == a1x2 && a0 < a1)) {
    *ax2 = a0x2;
    return a0;
  } else {
    *ax2 = a1x2;
    return a1;
  }
}

// Helper function that returns -1, 0, or +1 according to whether the distance
// from "x" to the great circle through (a0, a1) is less than, equal to, or
// greater than the given squared chord length "r2".  This method computes the
// squared sines of the distances involved, which is more accurate when the
// distances are small (less than 45 degrees).
//
// The remaining parameters are functions of (a0, a1) and are passed in
// because they have already been computed: n = (a0 - a1) x (a0 + a1),
// n1 = n.Norm(), and n2 = n.Norm2().
template <class T>
int TriageCompareLineSin2Distance(const Vector3<T>& x, const Vector3<T>& a0,
                                  const Vector3<T>& a1, T r2,
                                  const Vector3<T>& n, T n1, T n2) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // The minimum distance is to a point on the edge interior.  Since the true
  // distance to the edge is always less than 90 degrees, we can return
  // immediately if the limit is 90 degrees or larger.
  if (r2 >= 2.0) return -1;  // distance < limit

  // Otherwise we compute sin^2(distance to edge) to get the best accuracy
  // when the distance limit is small (e.g., S2::kIntersectionError).
  T n2sin2_r = n2 * r2 * (1 - 0.25 * r2);
  T n2sin2_r_error = 6 * T_ERR * n2sin2_r;
  T ax2, xDn = (x - GetClosestVertex(x, a0, a1, &ax2)).DotProd(n);
  T xDn2 = xDn * xDn;
  const T c1 = (((3.5 + 2 * sqrt(3)) * n1 + 32 * sqrt(3) * DBL_ERR) *
                T_ERR * sqrt(ax2));
  T xDn2_error = 4 * T_ERR * xDn2 + (2 * fabs(xDn) + c1) * c1;

  // If we are using extended precision, then it is worthwhile to recompute
  // the length of X more accurately.  Otherwise we use the fact that X is
  // guaranteed to be unit length to with a tolerance of 4 * DBL_ERR.
  if (T_ERR < DBL_ERR) {
    n2sin2_r *= x.Norm2();
    n2sin2_r_error += 4 * T_ERR * n2sin2_r;
  } else {
    n2sin2_r_error += 8 * DBL_ERR * n2sin2_r;
  }
  T diff = xDn2 - n2sin2_r;
  T error = xDn2_error + n2sin2_r_error;
  return (diff > error) ? 1 : (diff < -error) ? -1 : 0;
}

// Like TriageCompareLineSin2Distance, but this method computes the squared
// cosines of the distances involved.  It is more accurate when the distances
// are large (greater than 45 degrees).
template <class T>
int TriageCompareLineCos2Distance(const Vector3<T>& x, const Vector3<T>& a0,
                                  const Vector3<T>& a1, T r2,
                                  const Vector3<T>& n, T n1, T n2) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // The minimum distance is to a point on the edge interior.  Since the true
  // distance to the edge is always less than 90 degrees, we can return
  // immediately if the limit is 90 degrees or larger.
  if (r2 >= 2.0) return -1;  // distance < limit

  // Otherwise we compute cos^2(distance to edge).
  T cos_r = 1 - 0.5 * r2;
  T n2cos2_r = n2 * cos_r * cos_r;
  T n2cos2_r_error = 7 * T_ERR * n2cos2_r;

  // The length of M = X.CrossProd(N) is the cosine of the distance.
  T m2 = x.CrossProd(n).Norm2();
  T m1 = sqrt(m2);
  T m1_error = ((1 + 8 / sqrt(3)) * n1 + 32 * sqrt(3) * DBL_ERR) * T_ERR;
  T m2_error = 3 * T_ERR * m2 + (2 * m1 + m1_error) * m1_error;

  // If we are using extended precision, then it is worthwhile to recompute
  // the length of X more accurately.  Otherwise we use the fact that X is
  // guaranteed to be unit length to within a tolerance of 4 * DBL_ERR.
  if (T_ERR < DBL_ERR) {
    n2cos2_r *= x.Norm2();
    n2cos2_r_error += 4 * T_ERR * n2cos2_r;
  } else {
    n2cos2_r_error += 8 * DBL_ERR * n2cos2_r;
  }
  T diff = m2 - n2cos2_r;
  T error = m2_error + n2cos2_r_error;
  return (diff > error) ? -1 : (diff < -error) ? 1 : 0;
}

template <class T>
inline int TriageCompareLineDistance(const Vector3<T>& x, const Vector3<T>& a0,
                                     const Vector3<T>& a1, T r2,
                                     const Vector3<T>& n, T n1, T n2) {
  if (r2 < k45Degrees.length2()) {
    return TriageCompareLineSin2Distance(x, a0, a1, r2, n, n1, n2);
  } else {
    return TriageCompareLineCos2Distance(x, a0, a1, r2, n, n1, n2);
  }
}

template <class T>
int TriageCompareEdgeDistance(const Vector3<T>& x, const Vector3<T>& a0,
                              const Vector3<T>& a1, T r2) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // First we need to decide whether the closest point is an edge endpoint or
  // somewhere in the interior.  To determine this we compute a plane
  // perpendicular to (a0, a1) that passes through X.  Letting M be the normal
  // to this plane, the closest point is in the edge interior if and only if
  // a0.M < 0 and a1.M > 0.  Note that we can use "<" rather than "<=" because
  // if a0.M or a1.M is zero exactly then it doesn't matter which code path we
  // follow (since the distance to an endpoint and the distance to the edge
  // interior are exactly the same in this case).
  Vector3<T> n = (a0 - a1).CrossProd(a0 + a1);
  Vector3<T> m = n.CrossProd(x);
  // For better accuracy when the edge (a0,a1) is very short, we subtract "x"
  // before computing the dot products with M.
  Vector3<T> a0_dir = a0 - x;
  Vector3<T> a1_dir = a1 - x;
  T a0_sign = a0_dir.DotProd(m);
  T a1_sign = a1_dir.DotProd(m);
  T n2 = n.Norm2();
  T n1 = sqrt(n2);
  T n1_error = ((3.5 + 8 / sqrt(3)) * n1 + 32 * sqrt(3) * DBL_ERR) * T_ERR;
  T a0_sign_error = n1_error * a0_dir.Norm();
  T a1_sign_error = n1_error * a1_dir.Norm();
  if (a0_sign < a0_sign_error && a1_sign > -a1_sign_error) {
    if (a0_sign > -a0_sign_error || a1_sign < a1_sign_error) {
      // It is uncertain whether minimum distance is to an edge vertex or to
      // the edge interior.  We compute both distances and check whether they
      // yield the same result; otherwise the result is uncertain.
      int vertex_sign = min(TriageCompareDistance(x, a0, r2),
                            TriageCompareDistance(x, a1, r2));
      int line_sign = TriageCompareLineDistance(x, a0, a1, r2, n, n1, n2);
      return (vertex_sign == line_sign) ? line_sign : 0;
    }
    // The minimum distance is to the edge interior.
    return TriageCompareLineDistance(x, a0, a1, r2, n, n1, n2);
  }
  // The minimum distance is to an edge endpoint.
  return min(TriageCompareDistance(x, a0, r2),
             TriageCompareDistance(x, a1, r2));
}

// REQUIRES: the closest point to "x" is in the interior of edge (a0, a1).
int ExactCompareLineDistance(const Vector3_xf& x, const Vector3_xf& a0,
                             const Vector3_xf& a1, const ExactFloat& r2) {
  // Since we are given that the closest point is in the edge interior, the
  // true distance is always less than 90 degrees (which corresponds to a
  // squared chord length of 2.0).
  if (r2 >= 2.0) return -1;  // distance < limit

  // Otherwise compute the edge normal
  Vector3_xf n = a0.CrossProd(a1);
  ExactFloat sin_d = x.DotProd(n);
  ExactFloat sin2_r = r2 * (1 - 0.25 * r2);
  ExactFloat cmp = sin_d * sin_d - sin2_r * x.Norm2() * n.Norm2();
  return cmp.sgn();
}

int ExactCompareEdgeDistance(const S2Point& x, const S2Point& a0,
                             const S2Point& a1, S1ChordAngle r) {
  // Even if previous calculations were uncertain, we might not need to do
  // *all* the calculations in exact arithmetic here.  For example it may be
  // easy to determine whether "x" is closer to an endpoint or the edge
  // interior.  The only calculation where we always use exact arithmetic is
  // when measuring the distance to the extended line (great circle) through
  // "a0" and "a1", since it is virtually certain that the previous floating
  // point calculations failed in that case.
  //
  // CompareEdgeDirections requires that no edge has antipodal endpoints,
  // therefore we need to handle the cases a0 == -x, a1 == -x separately.
  if (a0 != -x && a1 != -x &&
      CompareEdgeDirections(a0, a1, a0, x) > 0 &&
      CompareEdgeDirections(a0, a1, x, a1) > 0) {
    // The closest point to "x" is along the interior of the edge.
    return ExactCompareLineDistance(ToExact(x), ToExact(a0), ToExact(a1),
                                    r.length2());
  } else {
    // The closest point to "x" is one of the edge endpoints.
    return min(CompareDistance(x, a0, r), CompareDistance(x, a1, r));
  }
}

int CompareEdgeDistance(const S2Point& x, const S2Point& a0, const S2Point& a1,
                        S1ChordAngle r) {
  // Check that the edge does not consist of antipodal points.  (This catches
  // the most common case -- the full test is in ExactCompareEdgeDistance.)
  ABSL_DCHECK_NE(a0, -a1);

  int sign = TriageCompareEdgeDistance(x, a0, a1, r.length2());
  if (sign != 0) return sign;

  // Optimization for the case where the edge is degenerate.
  if (a0 == a1) return CompareDistance(x, a0, r);
  if (kHasLongDouble) {
    sign = TriageCompareEdgeDistance(ToLD(x), ToLD(a0), ToLD(a1),
                                     ToLD(r.length2()));
    if (sign != 0) return sign;
  }
  return ExactCompareEdgeDistance(x, a0, a1, r);
}

int CompareEdgePairDistance(const S2Point& a0, const S2Point& a1,
                            const S2Point& b0, const S2Point& b1,
                            S1ChordAngle r) {
  // The following logic mimics S2::UpdateEdgePairMinDistance().

  // If the edges cross or share an endpoint, the minimum distance is zero.
  if (S2::CrossingSign(a0, a1, b0, b1) >= 0) {
    return (r.length2() > 0) ? -1 : (r.length2() < 0) ? 1 : 0;
  }
  // Otherwise, the minimum distance is achieved at an endpoint of at least
  // one of the two edges.
  return min(min(CompareEdgeDistance(a0, b0, b1, r),
                 CompareEdgeDistance(a1, b0, b1, r)),
             min(CompareEdgeDistance(b0, a0, a1, r),
                 CompareEdgeDistance(b1, a0, a1, r)));
}

template <class T>
int TriageIntersectionOrdering(const Vector3<T>& a, const Vector3<T>& b,
                               const Vector3<T>& c, const Vector3<T>& d,
                               const Vector3<T>& m, const Vector3<T>& n) {
  ABSL_DCHECK_NE(a, b);
  ABSL_DCHECK_NE(a, -b);
  ABSL_DCHECK_NE(c, d);
  ABSL_DCHECK_NE(c, -d);
  ABSL_DCHECK_NE(m, n);
  ABSL_DCHECK_NE(m, -n);

  // Given an edge AB, and the normal of a great circle M, the intersection of
  // the edge with the great circle is given by the triple product (A×B)×M.
  //
  // Its distance relative to the reference circle N is then proportional to the
  // dot product with N: d0 = ((A×B)×M)•N
  //
  // Edge CD is similar, we want to compute d1 = ((C×D)×M)•N and compare d0 to
  // d1.  If they're further than some error from each other, we can rely on the
  // comparison, otherwise we fall back to more exact arithmetic.
  //
  // ((A×B)×M)•N is a quadruple product.  We can expand this out using
  // Lagrange's formula for a vector triple product and then distribute the dot
  // product, which eliminates all the cross products:
  //
  // d0 = ((A×B)×M)•N
  // d0 = ((M•A)B - (M•B)A)•N
  // d0 = (M•A)(N•B) - (M•B)(N•A)
  //
  // Similarly:
  //
  // d1 = (M•C)(N•D) - (M•D)(N•C)
  //
  // We can compute this difference with a maximum absolute error of 32ε (see
  // the gappa proof at end of the file).
  //
  // NOTE: If we want to push this error bound down as far as possible, we could
  // use the dot product algorithm created by Ogita et al:
  //
  //   Accurate Sum and Dot Product, Ogita, Rump, Oishi 2005.
  //
  // Along with the 2x2 determinant algorithm by Kahan (which is useful for any
  // bilinear form):
  //
  //   Further Analysis of Kahan's Algorithm for the Accurate Computation of
  //   2x2 Determinants, Jeannerod, Louvet, and Muller, 2013.
  //
  // Both algorithms allow us to have bounded relative error, and since we're
  // only interested in the sign of this operation, as long as the relative
  // error is < 1 we can never get a sign flip, which would make this exact for
  // our purposes.
  constexpr T kMaxError = 32 * std::numeric_limits<T>::epsilon();

  T mdota = m.DotProd(a);
  T mdotb = m.DotProd(b);
  T mdotc = m.DotProd(c);
  T mdotd = m.DotProd(d);

  T ndota = n.DotProd(a);
  T ndotb = n.DotProd(b);
  T ndotc = n.DotProd(c);
  T ndotd = n.DotProd(d);

  T prodab = mdota * ndotb - mdotb * ndota;
  T prodcd = mdotc * ndotd - mdotd * ndotc;

  if (std::fabs(prodab - prodcd) > kMaxError) {
    return prodab < prodcd ? -1 : +1;
  }
  return 0;
}

int ExactIntersectionOrdering(const Vector3_xf& a, const Vector3_xf& b,
                              const Vector3_xf& c, const Vector3_xf& d,
                              const Vector3_xf& m, const Vector3_xf& n) {
  ABSL_DCHECK_NE(a, b);
  ABSL_DCHECK_NE(a, -b);
  ABSL_DCHECK_NE(c, d);
  ABSL_DCHECK_NE(c, -d);
  ABSL_DCHECK_NE(n, m);
  ABSL_DCHECK_NE(n, -m);

  ExactFloat mdota = m.DotProd(a);
  ExactFloat mdotb = m.DotProd(b);
  ExactFloat mdotc = m.DotProd(c);
  ExactFloat mdotd = m.DotProd(d);

  ExactFloat ndota = n.DotProd(a);
  ExactFloat ndotb = n.DotProd(b);
  ExactFloat ndotc = n.DotProd(c);
  ExactFloat ndotd = n.DotProd(d);

  ExactFloat prodab = mdota * ndotb - mdotb * ndota;
  ExactFloat prodcd = mdotc * ndotd - mdotd * ndotc;

  if (prodab < prodcd) return -1;
  if (prodab > prodcd) return +1;
  return 0;
}

int CircleEdgeIntersectionOrdering(const S2Point& a, const S2Point& b,
                                   const S2Point& c, const S2Point& d,
                                   const S2Point& m, const S2Point& n) {
  int ans = TriageIntersectionOrdering(a, b, c, d, m, n);
  if (ans != 0) {
    return ans;
  }

  // We got zero, check for duplicate/reverse duplicate edges before falling
  // back to more precision.
  if ((a == c && b == d) || (a == d && b == c)) {
    return 0;
  }

  if (kHasLongDouble) {
    ans = TriageIntersectionOrdering(ToLD(a), ToLD(b), ToLD(c), ToLD(d),
                                     ToLD(m), ToLD(n));
    if (ans != 0) {
      return ans;
    }
  }

  return ExactIntersectionOrdering(  //
      ToExact(a), ToExact(b), ToExact(c), ToExact(d), ToExact(m), ToExact(n));
}

template <typename T>
int TriageSignDotProd(const Vector3<T>& a, const Vector3<T>& b) {
  ABSL_DCHECK_LE(a.Norm2(), 2);
  ABSL_DCHECK_LE(b.Norm2(), 2);

  // The dot product error can be bound as 1.01nu|a||b| assuming nu < .01,
  // where u is the rounding unit (epsilon/2).  n=3 because we have 3
  // components, and we require that our vectors be <= sqrt(2) in length (so
  // that we can support the un-normalized edge normals for cells).
  //
  // So we have 1.01*3*ε/2*2 = 3.03ε, which we'll round up to 3.046875ε
  // which is exactly representable.
  //
  // Reference:
  //   Error Estimation Of Floating-Point Summation And Dot Product, Rump
  //   2011
  constexpr T kMaxError = 3.046875 * std::numeric_limits<T>::epsilon();

  T na = a.DotProd(b);
  if (std::fabs(na) <= kMaxError) return 0;
  return na > 0 ? +1 : -1;
}

int ExactSignDotProd(const Vector3_xf& a, const Vector3_xf& b) {
  ExactFloat dot = a.DotProd(b);
  if (dot.is_zero()) {
    return 0;
  }
  return dot.sgn() > 0 ? +1 : -1;
}

int SignDotProd(const S2Point& a, const S2Point& b) {
  int sign = TriageSignDotProd(a, b);
  if (sign != 0) {
    return sign;
  }

  if (kHasLongDouble) {
    sign = TriageSignDotProd(ToLD(a), ToLD(b));
    if (sign != 0) {
      return sign;
    }
  }

  return ExactSignDotProd(ToExact(a), ToExact(b));
}

template <class T>
int TriageCircleEdgeIntersectionSign(const Vector3<T>& a, const Vector3<T>& b,
                                     const Vector3<T>& n, const Vector3<T>& x) {
  ABSL_DCHECK_NE(a, b);
  ABSL_DCHECK_NE(a, -b);

  // The normal vector for the edge is A×B.  The vector for the line of
  // intersection between the plane defined by N and A×B is (A×B)×N.  We then
  // want to dot this with the normals of the other great circles X and compute
  // the sign.
  //
  // So the full formula for the test is ((A×B)×N)•X which is a quadruple
  // product.  We can expand this out using Lagrange's formula for a triple
  // product, and then distribute the dot product:
  //
  // = ((A×B)×N)•X
  // = ((N•A)B - (N•B)A)•X
  // = (N•A)(X•B) - (N•B)(X•A)
  //
  // We can compute this with a maximum absolute error of 14ε (see the gappa
  // proof at end of the file).
  constexpr T kMaxError = 14 * std::numeric_limits<T>::epsilon();

  T ndota = n.DotProd(a);
  T ndotb = n.DotProd(b);
  T xdota = x.DotProd(a);
  T xdotb = x.DotProd(b);

  T prod = ndota * xdotb - ndotb * xdota;

  // If we're too close to the plane of X we'll have to use more precision.
  if (std::fabs(prod) <= kMaxError) {
    return 0;
  }
  return prod < 0 ? -1 : +1;
}

int ExactCircleEdgeIntersectionSign(const Vector3_xf& a, const Vector3_xf& b,
                                    const Vector3_xf& n, const Vector3_xf& x) {
  ABSL_DCHECK_NE(a, b);
  ABSL_DCHECK_NE(a, -b);

  ExactFloat ndota = n.DotProd(a);
  ExactFloat ndotb = n.DotProd(b);
  ExactFloat xdota = x.DotProd(a);
  ExactFloat xdotb = x.DotProd(b);

  ExactFloat prod = ndota * xdotb - ndotb * xdota;

  if (prod.is_zero()) {
    return 0;
  }
  return prod.sgn() > 0 ? +1 : -1;
}

int CircleEdgeIntersectionSign(const S2Point& a, const S2Point& b,
                               const S2Point& n, const S2Point& x) {
  int ans = TriageCircleEdgeIntersectionSign(a, b, n, x);
  if (ans != 0) {
    return ans;
  }

  if (kHasLongDouble) {
    ans = TriageCircleEdgeIntersectionSign(ToLD(a), ToLD(b), ToLD(n), ToLD(x));
    if (ans != 0) {
      return ans;
    }
  }

  return ExactCircleEdgeIntersectionSign(  //
      ToExact(a), ToExact(b), ToExact(n), ToExact(x));
}

template <class T>
int TriageCompareEdgeDirections(const Vector3<T>& a0, const Vector3<T>& a1,
                                const Vector3<T>& b0, const Vector3<T>& b1) {
  constexpr T T_ERR = rounding_epsilon<T>();
  Vector3<T> na = (a0 - a1).CrossProd(a0 + a1);
  Vector3<T> nb = (b0 - b1).CrossProd(b0 + b1);
  T na_len = na.Norm(), nb_len = nb.Norm();
  T cos_ab = na.DotProd(nb);
  T cos_ab_error = ((5 + 4 * sqrt(3)) * na_len * nb_len +
                    32 * sqrt(3) * DBL_ERR * (na_len + nb_len)) * T_ERR;
  return (cos_ab > cos_ab_error) ? 1 : (cos_ab < -cos_ab_error) ? -1 : 0;
}

bool ArePointsLinearlyDependent(const Vector3_xf& x, const Vector3_xf& y) {
  return IsZero(x.CrossProd(y));
}

bool ArePointsAntipodal(const Vector3_xf& x, const Vector3_xf& y) {
  return ArePointsLinearlyDependent(x, y) && x.DotProd(y).sgn() < 0;
}

int ExactCompareEdgeDirections(const Vector3_xf& a0, const Vector3_xf& a1,
                               const Vector3_xf& b0, const Vector3_xf& b1) {
  ABSL_DCHECK(!ArePointsAntipodal(a0, a1));
  ABSL_DCHECK(!ArePointsAntipodal(b0, b1));
  return a0.CrossProd(a1).DotProd(b0.CrossProd(b1)).sgn();
}

int CompareEdgeDirections(const S2Point& a0, const S2Point& a1,
                          const S2Point& b0, const S2Point& b1) {
  // Check that no edge consists of antipodal points.  (This catches the most
  // common case -- the full test is in ExactCompareEdgeDirections.)
  ABSL_DCHECK_NE(a0, -a1);
  ABSL_DCHECK_NE(b0, -b1);

  int sign = TriageCompareEdgeDirections(a0, a1, b0, b1);
  if (sign != 0) return sign;

  // Optimization for the case where either edge is degenerate.
  if (a0 == a1 || b0 == b1) return 0;
  if (kHasLongDouble) {
    sign = TriageCompareEdgeDirections(ToLD(a0), ToLD(a1), ToLD(b0), ToLD(b1));
    if (sign != 0) return sign;
  }
  return ExactCompareEdgeDirections(ToExact(a0), ToExact(a1),
                                    ToExact(b0), ToExact(b1));
}

// If triangle ABC has positive sign, returns its circumcenter.  If ABC has
// negative sign, returns the negated circumcenter.
template <class T>
Vector3<T> GetCircumcenter(const Vector3<T>& a, const Vector3<T>& b,
                           const Vector3<T>& c, T* error) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // We compute the circumcenter using the intersection of the perpendicular
  // bisectors of AB and BC.  The formula is essentially
  //
  //    Z = ((A x B) x (A + B)) x ((B x C) x (B + C)),
  //
  // except that we compute the cross product (A x B) as (A - B) x (A + B)
  // (and similarly for B x C) since this is much more stable when the inputs
  // are unit vectors.
  Vector3<T> ab_diff = a - b, ab_sum = a + b;
  Vector3<T> bc_diff = b - c, bc_sum = b + c;
  Vector3<T> nab = ab_diff.CrossProd(ab_sum);
  T nab_len = nab.Norm();
  T ab_len = ab_diff.Norm();
  Vector3<T> nbc = bc_diff.CrossProd(bc_sum);
  T nbc_len = nbc.Norm();
  T bc_len = bc_diff.Norm();
  Vector3<T> mab = nab.CrossProd(ab_sum);
  Vector3<T> mbc = nbc.CrossProd(bc_sum);
  *error = (((16 + 24 * sqrt(3)) * T_ERR +
                8 * DBL_ERR * (ab_len + bc_len)) * nab_len * nbc_len +
               128 * sqrt(3) * DBL_ERR * T_ERR * (nab_len + nbc_len) +
               3 * 4096 * DBL_ERR * DBL_ERR * T_ERR * T_ERR);
  return mab.CrossProd(mbc);
}

template <class T>
int TriageEdgeCircumcenterSign(const Vector3<T>& x0, const Vector3<T>& x1,
                               const Vector3<T>& a, const Vector3<T>& b,
                               const Vector3<T>& c, int abc_sign) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // Compute the circumcenter Z of triangle ABC, and then test which side of
  // edge X it lies on.
  T z_error;
  Vector3<T> z = GetCircumcenter(a, b, c, &z_error);
  Vector3<T> nx = (x0 - x1).CrossProd(x0 + x1);
  // If the sign of triangle ABC is negative, then we have computed -Z and the
  // result should be negated.
  T result = abc_sign * nx.DotProd(z);

  T z_len = z.Norm();
  T nx_len = nx.Norm();
  T nx_error = ((1 + 2 * sqrt(3)) * nx_len + 32 * sqrt(3) * DBL_ERR) * T_ERR;
  T result_error = ((3 * T_ERR * nx_len + nx_error) * z_len + z_error * nx_len);
  return (result > result_error) ? 1 : (result < -result_error) ? -1 : 0;
}

int ExactEdgeCircumcenterSign(const Vector3_xf& x0, const Vector3_xf& x1,
                              const Vector3_xf& a, const Vector3_xf& b,
                              const Vector3_xf& c, int abc_sign) {
  // Return zero if the edge X is degenerate.  (Also see the comments in
  // SymbolicEdgeCircumcenterSign.)
  if (ArePointsLinearlyDependent(x0, x1)) {
    ABSL_DCHECK_GT(x0.DotProd(x1), 0);  // Antipodal edges not allowed.
    return 0;
  }
  // The simplest predicate for testing whether the sign is positive is
  //
  // (1)  (X0 x X1) . (|C|(A x B) + |A|(B x C) + |B|(C x A)) > 0
  //
  // where |A| denotes A.Norm() and the expression after the "." represents
  // the circumcenter of triangle ABC.  (This predicate is terrible from a
  // numerical accuracy point of view, but that doesn't matter since we are
  // going to use exact arithmetic.)  This predicate also assumes that
  // triangle ABC is CCW (positive sign); we correct for that below.
  //
  // The only problem with evaluating this inequality is that computing |A|,
  // |B| and |C| requires square roots.  To avoid this problem we use the
  // standard technique of rearranging the inequality to isolate at least one
  // square root and then squaring both sides.  We need to repeat this process
  // twice in order to eliminate all the square roots, which leads to a
  // polynomial predicate of degree 20 in the input arguments.
  //
  // Rearranging (1) we get
  //
  //      (X0 x X1) . (|C|(A x B) + |A|(B x C)) > |B|(X0 x X1) . (A x C)
  //
  // Before squaring we need to check the sign of each side.  If the signs are
  // different then we know the result without squaring, and if the signs are
  // both negative then after squaring both sides we need to invert the
  // result.  Define
  //
  //      dAB = (X0 x X1) . (A x B)
  //      dBC = (X0 x X1) . (B x C)
  //      dCA = (X0 x X1) . (C x A)
  //
  // Then we can now write the inequality above as
  //
  // (2)  |C| dAB + |A| dBC > -|B| dCA
  //
  // The RHS of (2) is positive if dCA < 0, and the LHS of (2) is positive if
  // (|C| dAB + |A| dBC) > 0.  Since the LHS has square roots, we need to
  // eliminate them using the same process.  Rewriting the LHS as
  //
  // (3)  |C| dAB > -|A| dBC
  //
  // we again need to check the signs of both sides.  Let's start with that.
  // We also precompute the following values because they are used repeatedly
  // when squaring various expressions below:
  //
  //     abc2 = |A|^2 dBC^2
  //     bca2 = |B|^2 dCA^2
  //     cab2 = |C|^2 dAB^2
  Vector3_xf nx = x0.CrossProd(x1);
  ExactFloat dab = nx.DotProd(a.CrossProd(b));
  ExactFloat dbc = nx.DotProd(b.CrossProd(c));
  ExactFloat dca = nx.DotProd(c.CrossProd(a));
  ExactFloat abc2 = a.Norm2() * (dbc * dbc);
  ExactFloat bca2 = b.Norm2() * (dca * dca);
  ExactFloat cab2 = c.Norm2() * (dab * dab);

  // If the two sides of (3) have different signs (including the case where
  // one side is zero) then we know the result.  Also, if both sides are zero
  // then we know the result.  The following logic encodes this.
  int lhs3_sgn = dab.sgn(), rhs3_sgn = -dbc.sgn();
  int lhs2_sgn = max(-1, min(1, lhs3_sgn - rhs3_sgn));
  if (lhs2_sgn == 0 && lhs3_sgn != 0) {
    // Both sides of (3) have the same non-zero sign, so square both sides.
    // If both sides were negative then invert the result.
    lhs2_sgn = (cab2 - abc2).sgn() * lhs3_sgn;
  }
  // Now if the two sides of (2) have different signs then we know the result
  // of this entire function.
  int rhs2_sgn = -dca.sgn();
  int result = max(-1, min(1, lhs2_sgn - rhs2_sgn));
  if (result == 0 && lhs2_sgn != 0) {
    // Both sides of (2) have the same non-zero sign, so square both sides.
    // (If both sides were negative then we invert the result below.)
    // This gives
    //
    //        |C|^2 dAB^2 + |A|^2 dBC^2 + 2 |A| |C| dAB dBC > |B|^2 dCA^2
    //
    // This expression still has square roots (|A| and |C|), so we rewrite as
    //
    // (4)    2 |A| |C| dAB dBC > |B|^2 dCA^2 - |C|^2 dAB^2 - |A|^2 dBC^2 .
    //
    // Again, if the two sides have different signs then we know the result.
    int lhs4_sgn = dab.sgn() * dbc.sgn();
    ExactFloat rhs4 = bca2 - cab2 - abc2;
    result = max(-1, min(1, lhs4_sgn - rhs4.sgn()));
    if (result == 0 && lhs4_sgn != 0) {
      // Both sides of (4) have the same non-zero sign, so square both sides.
      // If both sides were negative then invert the result.
      result = (4 * abc2 * cab2 - rhs4 * rhs4).sgn() * lhs4_sgn;
    }
    // Correct the sign if both sides of (2) were negative.
    result *= lhs2_sgn;
  }
  // If the sign of triangle ABC is negative, then we have computed -Z and the
  // result should be negated.
  return abc_sign * result;
}

// Given arguments such that ExactEdgeCircumcenterSign(x0, x1, a, b, c) == 0,
// returns the value of Sign(X0, X1, Z) (where Z is the circumcenter of
// triangle ABC) after symbolic perturbations are taken into account.  The
// result is zero only if X0 == X1, A == B, B == C, or C == A.  (It is nonzero
// if these pairs are exactly proportional to each other but not equal.)
int SymbolicEdgeCircumcenterSign(
    const S2Point& x0, const S2Point& x1,
    const S2Point& a_arg, const S2Point& b_arg, const S2Point& c_arg) {
  // We use the same perturbation strategy as SymbolicCompareDistances. Note
  // that pedestal perturbations of X0 and X1 do not affect the result,
  // because Sign(X0, X1, Z) does not change when its arguments are scaled
  // by a positive factor.  Therefore we only need to consider A, B, C.
  // Suppose that A is the smallest lexicographically and therefore has the
  // largest perturbation.  This has the effect of perturbing the circumcenter
  // of ABC slightly towards A, and since the circumcenter Z was previously
  // exactly collinear with edge X, this implies that after the perturbation
  // Sign(X0, X1, Z) == UnperturbedSign(X0, X1, A).  (We want the result
  // to be zero if X0, X1, and A are linearly dependent, rather than using
  // symbolic perturbations, because these perturbations are defined to be
  // much, much smaller than the pedestal perturbation of B and C that are
  // considered below.)
  //
  // If A is also exactly collinear with edge X, then we move on to the next
  // smallest point lexicographically out of {B, C}.  It is easy to see that
  // as long as A, B, C are all distinct, one of these three Sign calls
  // will be nonzero, because if A, B, C are all distinct and collinear with
  // edge X then their circumcenter Z coincides with the normal of X, and
  // therefore Sign(X0, X1, Z) is nonzero.
  //
  // This function could be extended to handle the case where X0 and X1 are
  // linearly dependent as follows.  First, suppose that every point has both
  // a pedestal peturbation as described above, and also the three
  // axis-aligned perturbations described in the "Simulation of Simplicity"
  // paper, where all pedestal perturbations are defined to be much, much
  // larger than any axis-aligned perturbation.  Note that since pedestal
  // perturbations have no effect on Sign, we can use this model for *all*
  // the S2 predicates, which ensures that all the various predicates are
  // fully consistent with each other.
  //
  // With this model, the strategy described above yields the correct result
  // unless X0 and X1 are exactly linearly dependent.  When that happens, then
  // no perturbation (pedestal or axis-aligned) of A,B,C affects the result,
  // and no pedestal perturbation of X0 or X1 affects the result, therefore we
  // need to consider the smallest axis-aligned perturbation of X0 or X1.  The
  // first perturbation that makes X0 and X1 linearly independent yields the
  // result.  Supposing that X0 < X1, this is the perturbation of X0[2] unless
  // both points are multiples of [0, 0, 1], in which case it is the
  // perturbation of X0[1].  The sign test can be implemented by computing the
  // perturbed cross product of X0 and X1 and taking the dot product with the
  // exact value of Z.  For example if X0[2] is perturbed, the perturbed cross
  // product is proportional to (0, 0, 1) x X1 = (-X1[1], x1[0], 0).  Note
  // that if the dot product with Z is exactly zero, then it is still
  // necessary to fall back to pedestal perturbations of A, B, C, but one of
  // these perturbations is now guaranteed to succeed.

  // If any two triangle vertices are equal, the result is zero.
  if (a_arg == b_arg || b_arg == c_arg || c_arg == a_arg) return 0;

  // Sort A, B, C in lexicographic order.
  const S2Point *a = &a_arg, *b = &b_arg, *c = &c_arg;
  if (*b < *a) std::swap(a, b);
  if (*c < *b) std::swap(b, c);
  if (*b < *a) std::swap(a, b);

  // Now consider the perturbations in decreasing order of size.
  int sign = UnperturbedSign(x0, x1, *a);
  if (sign != 0) return sign;
  sign = UnperturbedSign(x0, x1, *b);
  if (sign != 0) return sign;
  return UnperturbedSign(x0, x1, *c);
}

int EdgeCircumcenterSign(const S2Point& x0, const S2Point& x1,
                         const S2Point& a, const S2Point& b,
                         const S2Point& c) {
  // Check that the edge does not consist of antipodal points.  (This catches
  // the most common case -- the full test is in ExactEdgeCircumcenterSign.)
  ABSL_DCHECK_NE(x0, -x1);

  int abc_sign = Sign(a, b, c);
  int sign = TriageEdgeCircumcenterSign(x0, x1, a, b, c, abc_sign);
  if (sign != 0) return sign;

  // Optimization for the cases that are going to return zero anyway, in order
  // to avoid falling back to exact arithmetic.
  if (x0 == x1 || a == b || b == c || c == a) return 0;
  if (kHasLongDouble) {
    sign = TriageEdgeCircumcenterSign(
        ToLD(x0), ToLD(x1), ToLD(a), ToLD(b), ToLD(c), abc_sign);
    if (sign != 0) return sign;
  }
  sign = ExactEdgeCircumcenterSign(
      ToExact(x0), ToExact(x1), ToExact(a), ToExact(b), ToExact(c), abc_sign);
  if (sign != 0) return sign;

  // Unlike the other methods, SymbolicEdgeCircumcenterSign does not depend
  // on the sign of triangle ABC.
  return SymbolicEdgeCircumcenterSign(x0, x1, a, b, c);
}

template <class T>
Excluded TriageVoronoiSiteExclusion(const Vector3<T>& a, const Vector3<T>& b,
                                    const Vector3<T>& x0, const Vector3<T>& x1,
                                    T r2) {
  constexpr T T_ERR = rounding_epsilon<T>();

  // Define the "coverage disc" of a site S to be the disc centered at S with
  // radius r (i.e., squared chord angle length r2).  Similarly, define the
  // "coverage interval" of S along an edge X to be the intersection of X with
  // the coverage disc of S.  The coverage interval can be represented as the
  // point at the center of the interval and an angle that measures the
  // semi-width or "radius" of the interval.
  //
  // To test whether site A excludes site B along the input edge X, we test
  // whether the coverage interval of A contains the coverage interval of B.
  // Let "ra" and "rb" be the radii (semi-widths) of the two intervals, and
  // let "d" be the angle between their center points.  Then "a" properly
  // contains "b" if (ra - rb > d), and "b" contains "a" if (rb - ra > d).
  // Note that only one of these conditions can be true.  Therefore we can
  // determine whether one site excludes the other by checking whether
  //
  // (1)   |rb - ra| > d
  //
  // and use the sign of (rb - ra) to determine which site is excluded.
  //
  // The actual code is based on the following.  Let A1 and B1 be the unit
  // vectors A and B scaled by cos(r) (these points are inside the sphere).
  // The planes perpendicular to OA1 and OA2 cut off two discs of radius r
  // around A and B.  Now consider the two lines (inside the sphere) where
  // these planes intersect the plane containing the input edge X, and let A2
  // and B2 be the points on these lines that are closest to A and B. The
  // coverage intervals of A and B can be represented as an interval along
  // each of these lines, centered at A2 and B2.  Let P1 and P2 be the
  // endpoints of the coverage interval for A, and let Q1 and Q2 be the
  // endpoints of the coverage interval for B.  We can view each coverage
  // interval as either a chord through the sphere's interior, or as a segment
  // of the original edge X (by projecting the chord onto the sphere's
  // surface).
  //
  // To check whether B's interval is contained by A's interval, we test
  // whether both endpoints of B's interval (Q1 and Q2) are contained by A's
  // interval.  E.g., we could test whether Qi.DotProd(A2) > A2.Norm2().
  //
  // However rather than constructing the actual points A1, A2, and so on, it
  // turns out to be more efficient to compute the sines and cosines
  // ("components") of the various angles and then use trigonometric
  // identities.  Predicate (1) can be expressed as
  //
  //      |sin(rb - ra)| > sin(d)
  //
  // provided that |d| <= Pi/2 (which must be checked), and then expanded to
  //
  // (2)  |sin(rb) cos(ra) - sin(ra) cos(rb)| > sin(d) .
  //
  // The components of the various angles can be expressed using dot and cross
  // products based on the construction above:
  //
  //   sin(ra) = sqrt(sin^2(r) |a|^2 |n|^2 - |a.n|^2) / |aXn|
  //   cos(ra) = cos(r) |a| |n| / |aXn|
  //   sin(rb) = sqrt(sin^2(r) |b|^2 |n|^2 - |b.n|^2) / |bXn|
  //   cos(rb) = cos(r) |b| |n| / |bXn|
  //   sin(d)  = (aXb).n |n| / (|aXn| |bXn|)
  //   cos(d)  = (aXn).(bXn) / (|aXn| |bXn|)
  //
  // Also, the squared chord length r2 is equal to 4 * sin^2(r / 2), which
  // yields the following relationships:
  //
  //   sin(r)  = sqrt(r2 (1 - r2 / 4))
  //   cos(r)  = 1 - r2 / 2
  //
  // We then scale both sides of (2) by |aXn| |bXn| / |n| (in order to
  // minimize the number of calculations and to avoid divisions), which gives:
  //
  //    cos(r) ||a| sqrt(sin^2(r) |b|^2 |n|^2 - |b.n|^2) -
  //            |b| sqrt(sin^2(r) |a|^2 |n|^2 - |a.n|^2)| > (aXb).n
  //
  // Furthermore we can substitute |a| = |b| = 1 (as long as this is taken
  // into account in the error bounds), yielding
  //
  // (3)   cos(r) |sqrt(sin^2(r) |n|^2 - |b.n|^2) -
  //               sqrt(sin^2(r) |n|^2 - |a.n|^2)| > (aXb).n
  //
  // The code below is more complicated than this because many expressions
  // have been modified for better numerical stability.  For example, dot
  // products between unit vectors are computed using (x - y).DotProd(x + y),
  // and the dot product between a point P and the normal N of an edge X is
  // measured using (P - Xi).DotProd(N) where Xi is the endpoint of X that is
  // closer to P.

  Vector3<T> n = (x0 - x1).CrossProd(x0 + x1);  // 2 * x0.CrossProd(x1)
  T n2 = n.Norm2();
  T n1 = sqrt(n2);
  // This factor is used in the error terms of dot products with "n" below.
  T Dn_error = ((3.5 + 2 * sqrt(3)) * n1 + 32 * sqrt(3) * DBL_ERR) * T_ERR;

  T cos_r = 1 - 0.5 * r2;
  T sin2_r = r2 * (1 - 0.25 * r2);
  T n2sin2_r = n2 * sin2_r;

  // "ra" and "rb" denote sin(ra) and sin(rb) after the scaling above.
  T ax2, aDn = (a - GetClosestVertex(a, x0, x1, &ax2)).DotProd(n);
  T aDn2 = aDn * aDn;
  T aDn_error = Dn_error * sqrt(ax2);
  T ra2 = n2sin2_r - aDn2;
  T ra2_error = (8 * DBL_ERR + 4 * T_ERR) * aDn2 +
                (2 * fabs(aDn) + aDn_error) * aDn_error + 6 * T_ERR * n2sin2_r;
  // This is the minimum possible value of ra2, which is used to bound the
  // derivative of sqrt(ra2) in computing ra_error below.
  T min_ra2 = ra2 - ra2_error;
  if (min_ra2 < 0) return Excluded::UNCERTAIN;
  T ra = sqrt(ra2);
  // Includes the ra2 subtraction error above.
  T ra_error = 1.5 * T_ERR * ra + 0.5 * ra2_error / sqrt(min_ra2);

  T bx2, bDn = (b - GetClosestVertex(b, x0, x1, &bx2)).DotProd(n);
  T bDn2 = bDn * bDn;
  T bDn_error = Dn_error * sqrt(bx2);
  T rb2 = n2sin2_r - bDn2;
  T rb2_error = (8 * DBL_ERR + 4 * T_ERR) * bDn2 +
                (2 * fabs(bDn) + bDn_error) * bDn_error + 6 * T_ERR * n2sin2_r;
  T min_rb2 = rb2 - rb2_error;
  if (min_rb2 < 0) return Excluded::UNCERTAIN;
  T rb = sqrt(rb2);
  // Includes the rb2 subtraction error above.
  T rb_error = 1.5 * T_ERR * rb + 0.5 * rb2_error / sqrt(min_rb2);

  // The sign of LHS(3) before taking the absolute value determines which site
  // may be excluded by the other.  If it is positive then A may be excluded,
  // and if it is negative then B may be excluded.
  T lhs3 = cos_r * (rb - ra);
  T abs_lhs3 = fabs(lhs3);
  T lhs3_error = cos_r * (ra_error + rb_error) + 3 * T_ERR * abs_lhs3;

  // Now we evaluate the RHS of (3), which is proportional to sin(d).
  Vector3<T> aXb = (a - b).CrossProd(a + b);  // 2 * a.CrossProd(b)
  T aXb1 = aXb.Norm();
  T sin_d = 0.5 * aXb.DotProd(n);
  T sin_d_error = (4 * DBL_ERR + (2.5 + 2 * sqrt(3)) * T_ERR) * aXb1 * n1 +
                  16 * sqrt(3) * DBL_ERR * T_ERR * (aXb1 + n1);

  // If LHS(3) is definitely less than RHS(3), neither site excludes the other.
  T result = abs_lhs3 - sin_d;
  T result_error = lhs3_error + sin_d_error;
  if (result < -result_error) return Excluded::NEITHER;

  // d < 0 means that when AB is projected onto the great circle through X0X1,
  // it proceeds in the opposite direction as X0X1.  Normally we have d > 0
  // since GetVoronoiSiteExclusion requires d(A,X0) < d(B,X0) (corresponding to
  // the fact that sites are processed in order of increasing distance from X0).
  //
  // However when edge X is long and/or the snap radius "r" is large, there
  // are two cases where where d < 0 can occur:
  //
  // 1. d(A,X0) > Pi/2 and d(B,X1) < Pi/2 or the symmetric case (swap < and >).
  //    This can only happen when d(X0,X1) + r > Pi/2.  Note that {A,B} may
  //    project to the interior of edge X or beyond its endpoints.  In this
  //    case A is kept iff d(A,X0) < Pi/2, and B is kept otherwise.  Note that
  //    the sign of (rb - ra) is not sufficient to determine which point is
  //    kept in the situation where d(X0,X1) + r > Pi.
  //
  // 2. A is beyond endpoint X0, B is beyond endpoint X1, and AB wraps around
  //    the sphere in the opposite direction from edge X.  This case can only
  //    happen when d(X0,X1) + r > Pi.  Here each site is closest to one
  //    endpoint of X and neither excludes the other.
  //
  // The algorithm that handles both cases is to keep A if d(A,X0) < Pi/2 and
  // to keep B if d(B,X1) < Pi/2.  (One of these conditions is always true.)
  if (sin_d < -sin_d_error) {
    // Site A is kept if ca < 0 and excluded if ca > 0.
    T r90 = S1ChordAngle::Right().length2();
    int ca = TriageCompareCosDistance(a, x0, r90);
    int cb = TriageCompareCosDistance(b, x1, r90);
    if (ca < 0 && cb < 0) return Excluded::NEITHER;
    if (ca <= 0 && cb <= 0) return Excluded::UNCERTAIN;  // One or both kept?
    // Since either ca or cb is 1, we know the result even if the distance
    // comparison for the other site was uncertain.
    ABSL_DCHECK(ca <= 0 || cb <= 0);
    return (ca > 0) ? Excluded::FIRST : Excluded::SECOND;
  }
  if (sin_d <= sin_d_error) return Excluded::UNCERTAIN;

  // Otherwise, before proceeding further we need to check that |d| <= Pi/2.
  // In fact, |d| < Pi/2 is enough because of the requirement that r < Pi/2.
  // The following expression represents cos(d) after scaling; it is
  // equivalent to (aXn).(bXn) but has about 30% less error.
  T cos_d = a.DotProd(b) * n2 - aDn * bDn;
  T cos_d_error =
      ((8 * DBL_ERR + 5 * T_ERR) * fabs(aDn) + aDn_error) * fabs(bDn) +
      (fabs(aDn) + aDn_error) * bDn_error + (8 * DBL_ERR + 8 * T_ERR) * n2;
  if (cos_d <= -cos_d_error) return Excluded::NEITHER;

  // Potential optimization: if the sign of cos(d) is uncertain, then instead
  // we could check whether cos(d) >= cos(r).  Unfortunately this is fairly
  // expensive since it requires computing denominator |aXn||bXn| of cos(d)
  // and the associated error bounds.  In any case this case is relatively
  // rare so it seems better to punt.
  if (cos_d < cos_d_error) return Excluded::UNCERTAIN;

  // Now we can finish checking the results of predicate (3).
  if (result <= result_error) return Excluded::UNCERTAIN;
  ABSL_DCHECK_GT(abs_lhs3, lhs3_error);
  return (lhs3 > 0) ? Excluded::FIRST : Excluded::SECOND;
}

Excluded ExactVoronoiSiteExclusion(const Vector3_xf& a, const Vector3_xf& b,
                                   const Vector3_xf& x0, const Vector3_xf& x1,
                                   const ExactFloat& r2) {
  ABSL_DCHECK(!ArePointsAntipodal(x0, x1));

  // Recall that one site excludes the other if
  //
  // (1)  |sin(rb - ra)| > sin(d)
  //
  // and that the sign of (rb - ra) determines which site is excluded (see the
  // comments in TriageVoronoiSiteExclusion).  To evaluate this using exact
  // arithmetic, we expand this to the same predicate as before:
  //
  // (2)    cos(r) ||a| sqrt(sin^2(r) |b|^2 |n|^2 - |b.n|^2) -
  //                |b| sqrt(sin^2(r) |a|^2 |n|^2 - |a.n|^2)| > (aXb).n
  //
  // We also need to verify that d <= Pi/2, which is implemented by checking
  // that sin(d) >= 0 and cos(d) >= 0.
  //
  // To eliminate the square roots we use the standard technique of
  // rearranging the inequality to isolate at least one square root and then
  // squaring both sides.  We need to repeat this process twice in order to
  // eliminate all the square roots, which leads to a polynomial predicate of
  // degree 20 in the input arguments (i.e., degree 4 in each of "a", "b",
  // "x0", "x1", and "r2").

  // Before squaring we need to check the sign of each side.  If the RHS of
  // (2) is negative (corresponding to sin(d) < 0), then we need to apply the
  // logic in TriageVoronoiSiteExclusion.
  Vector3_xf n = x0.CrossProd(x1);
  ExactFloat rhs2 = a.CrossProd(b).DotProd(n);
  int rhs2_sgn = rhs2.sgn();
  if (rhs2_sgn < 0) {
    // This is the d < 0 case.  See comments in TriageVoronoiSiteExclusion.
    ExactFloat r90 = S1ChordAngle::Right().length2();
    int ca = ExactCompareDistance(a, x0, r90);
    int cb = ExactCompareDistance(b, x1, r90);
    if (ca < 0 && cb < 0) return Excluded::NEITHER;
    ABSL_DCHECK(ca != 0 && cb != 0);  // This is guaranteed since d < 0.
    ABSL_DCHECK(ca < 0 || cb < 0);    // At least one site must be kept.
    return (ca > 0) ? Excluded::FIRST : Excluded::SECOND;
  }

  // We also check that cos(d) >= 0.  Given what else we need to compute, it
  // is cheaper use the identity (aXn).(bXn) = (a.b) |n|^2 - (a.n)(b.n) .
  ExactFloat n2 = n.Norm2();
  ExactFloat aDn = a.DotProd(n);
  ExactFloat bDn = b.DotProd(n);
  ExactFloat cos_d = a.DotProd(b) * n2 - aDn * bDn;
  if (cos_d.sgn() < 0) return Excluded::NEITHER;

  // Otherwise we continue evaluating the LHS of (2), defining
  //    sa = |b| sqrt(sin^2(r) |a|^2 |n|^2 - |a.n|^2)
  //    sb = |a| sqrt(sin^2(r) |b|^2 |n|^2 - |b.n|^2) .
  // The sign of the LHS of (2) (before taking the absolute value) determines
  // which coverage interval is larger and therefore which site is potentially
  // being excluded.
  ExactFloat a2 = a.Norm2();
  ExactFloat b2 = b.Norm2();
  ExactFloat n2sin2_r = r2 * (1 - 0.25 * r2) * n2;
  ExactFloat sa2 = b2 * (n2sin2_r * a2 - aDn * aDn);
  ExactFloat sb2 = a2 * (n2sin2_r * b2 - bDn * bDn);
  int lhs2_sgn = (sb2 - sa2).sgn();

  if (lhs2_sgn == 0) {
    // If the RHS of (2) is zero as well (i.e., d == 0) then both sites are
    // equidistant from every point on edge X.  This case requires symbolic
    // perturbations, but it should already have been handled in
    // GetVoronoiSiteExclusion() (see the call to CompareDistances).
    ABSL_DCHECK_GT(rhs2_sgn, 0);
    return Excluded::NEITHER;
  }
  // Next we square both sides of (2), yielding
  //
  //      cos^2(r) (sb^2 + sa^2 - 2 sa sb) > (aXb.n)^2
  //
  // which can be rearranged to give
  //
  // (3)  cos^2(r) (sb^2 + sa^2) - (aXb.n)^2 > 2 cos^2(r) sa sb .
  //
  // The RHS of (3) is always non-negative, but we still need to check the
  // sign of the LHS.
  ExactFloat cos_r = 1 - 0.5 * r2;
  ExactFloat cos2_r = cos_r * cos_r;
  ExactFloat lhs3 = cos2_r * (sa2 + sb2) - rhs2 * rhs2;
  if (lhs3.sgn() < 0) return Excluded::NEITHER;

  // Otherwise we square both sides of (3) to obtain:
  //
  // (4)  LHS(3)^2  >  4 cos^4(r) sa^2 sb^2
  ExactFloat lhs4 = lhs3 * lhs3;
  ExactFloat rhs4 = 4 * cos2_r * cos2_r * sa2 * sb2;
  int result = (lhs4 - rhs4).sgn();
  if (result < 0) return Excluded::NEITHER;
  if (result == 0) {
    // We have |rb - ra| = d and d > 0.  This implies that one coverage
    // interval contains the other, but not properly: the two intervals share
    // a common endpoint.  The distance from each site to that point is
    // exactly "r", therefore we need to use symbolic perturbations.  Recall
    // that site A is considered closer to an equidistant point if and only if
    // A > B.  Therefore if (rb > ra && A > B) or (ra > rb && B > A) then each
    // site is closer to at least one point and neither site is excluded.
    //
    // Ideally this logic would be in a separate SymbolicVoronoiSiteExclusion
    // method for better testing, but this is not convenient because it needs
    // lhs_sgn (which requires exact arithmetic to compute).
    if ((lhs2_sgn > 0) == (a > b)) return Excluded::NEITHER;
  }
  // At this point we know that one of the two sites is excluded.  The sign of
  // the LHS of (2) (before the absolute value) determines which one.
  return (lhs2_sgn > 0) ? Excluded::FIRST : Excluded::SECOND;
}

Excluded GetVoronoiSiteExclusion(const S2Point& a, const S2Point& b,
                                 const S2Point& x0, const S2Point& x1,
                                 S1ChordAngle r) {
  ABSL_DCHECK_LT(r, S1ChordAngle::Right());
  ABSL_DCHECK_LT(s2pred::CompareDistances(x0, a, b), 0);  // (implies a != b)
  ABSL_DCHECK_LE(s2pred::CompareEdgeDistance(a, x0, x1, r), 0);
  ABSL_DCHECK_LE(s2pred::CompareEdgeDistance(b, x0, x1, r), 0);
  // Check that the edge does not consist of antipodal points.  (This catches
  // the most common case -- the full test is in ExactVoronoiSiteExclusion.)
  ABSL_DCHECK_NE(x0, -x1);

  // If one site is closer than the other to both endpoints of X, then it is
  // closer to every point on X.  Note that this also handles the case where A
  // and B are equidistant from every point on X (i.e., X is the perpendicular
  // bisector of AB), because CompareDistances uses symbolic perturbations to
  // ensure that either A or B is considered closer (in a consistent way).
  // This also ensures that the choice of A or B does not depend on the
  // direction of X.
  if (s2pred::CompareDistances(x1, a, b) < 0) {
    return Excluded::SECOND;  // Site A is closer to every point on X.
  }

  Excluded result = TriageVoronoiSiteExclusion(a, b, x0, x1, r.length2());
  if (result != Excluded::UNCERTAIN) return result;
  if (kHasLongDouble) {
    result = TriageVoronoiSiteExclusion(ToLD(a), ToLD(b), ToLD(x0), ToLD(x1),
                                        ToLD(r.length2()));
    if (result != Excluded::UNCERTAIN) return result;
  }
  return ExactVoronoiSiteExclusion(ToExact(a), ToExact(b), ToExact(x0),
                                   ToExact(x1), r.length2());
}

std::ostream& operator<<(std::ostream& os, Excluded excluded) {
  switch (excluded) {
    case Excluded::FIRST: return os << "FIRST";
    case Excluded::SECOND: return os << "SECOND";
    case Excluded::NEITHER: return os << "NEITHER";
    case Excluded::UNCERTAIN: return os << "UNCERTAIN";
    default: return os << "Unknown enum value";
  }
}

// Explicitly instantiate all of the template functions above so that the
// tests can use them without putting all the definitions in a header file.

template int TriageCompareCosDistances<double>(
    const Vector3_d&, const Vector3_d&, const Vector3_d&);

template int TriageCompareCosDistances<long double>(
    const Vector3_ld&, const Vector3_ld&, const Vector3_ld&);

template int TriageCompareSin2Distances<double>(
    const Vector3_d&, const Vector3_d&, const Vector3_d&);

template int TriageCompareSin2Distances<long double>(
    const Vector3_ld&, const Vector3_ld&, const Vector3_ld&);

template int TriageCompareCosDistance<double>(
    const Vector3_d&, const Vector3_d&, double r2);

template int TriageCompareCosDistance<long double>(
    const Vector3_ld&, const Vector3_ld&, long double r2);

template int TriageCompareSin2Distance<double>(
    const Vector3_d&, const Vector3_d&, double r2);

template int TriageCompareSin2Distance<long double>(
    const Vector3_ld&, const Vector3_ld&, long double r2);

template int TriageSignDotProd<double>(const Vector3_d& a, const Vector3_d& b);

template int TriageSignDotProd<long double>(const Vector3_ld& a,
                                            const Vector3_ld& b);

template int TriageIntersectionOrdering<double>(
    const Vector3_d& a, const Vector3_d& b, const Vector3_d& c,
    const Vector3_d& d, const Vector3_d& m, const Vector3_d& n);

template int TriageIntersectionOrdering<long double>(
    const Vector3_ld& a, const Vector3_ld& b, const Vector3_ld& c,
    const Vector3_ld& d, const Vector3_ld& m, const Vector3_ld& n);

template int TriageCircleEdgeIntersectionSign<double>(const Vector3_d& a,
                                                      const Vector3_d& b,
                                                      const Vector3_d& n,
                                                      const Vector3_d& x);

template int TriageCircleEdgeIntersectionSign<long double>(const Vector3_ld& a,
                                                           const Vector3_ld& b,
                                                           const Vector3_ld& n,
                                                           const Vector3_ld& x);

template int TriageCompareEdgeDistance<double>(
    const Vector3_d& x, const Vector3_d& a0, const Vector3_d& a1, double r2);

template int TriageCompareEdgeDistance<long double>(
    const Vector3_ld& x, const Vector3_ld& a0, const Vector3_ld& a1,
    long double r2);

template int TriageCompareEdgeDirections<double>(
    const Vector3_d& a0, const Vector3_d& a1,
    const Vector3_d& b0, const Vector3_d& b1);

template int TriageCompareEdgeDirections<long double>(
    const Vector3_ld& a0, const Vector3_ld& a1,
    const Vector3_ld& b0, const Vector3_ld& b1);

template int TriageEdgeCircumcenterSign<double>(
    const Vector3_d& x0, const Vector3_d& x1, const Vector3_d& a,
    const Vector3_d& b, const Vector3_d& c, int abc_sign);

template int TriageEdgeCircumcenterSign<long double>(
    const Vector3_ld& x0, const Vector3_ld& x1, const Vector3_ld& a,
    const Vector3_ld& b, const Vector3_ld& c, int abc_sign);

template Excluded TriageVoronoiSiteExclusion(
    const Vector3_d& a, const Vector3_d& b,
    const Vector3_d& x0, const Vector3_d& x1, double r2);

template Excluded TriageVoronoiSiteExclusion(
    const Vector3_ld& a, const Vector3_ld& b,
    const Vector3_ld& x0, const Vector3_ld& x1, long double r2);

}  // namespace s2pred

// Gappa proof for TriageIntersectionOrdering
//
// # Use IEEE754 double precision, round-to-nearest by default.
// @rnd = float<ieee_64, ne>;
//
// # Five vectors, two forming edges AB, CD and two normals N,M for great
// # circles.
// a0 = rnd(a0_ex);
// a1 = rnd(a1_ex);
// a2 = rnd(a2_ex);
// b0 = rnd(b0_ex);
// b1 = rnd(b1_ex);
// b2 = rnd(b2_ex);
// c0 = rnd(c0_ex);
// c1 = rnd(c1_ex);
// c2 = rnd(c2_ex);
// d0 = rnd(d0_ex);
// d1 = rnd(d1_ex);
// d2 = rnd(d2_ex);
// n0 = rnd(n0_ex);
// n1 = rnd(n1_ex);
// n2 = rnd(n2_ex);
// m0 = rnd(m0_ex);
// m1 = rnd(m1_ex);
// m2 = rnd(m2_ex);
//
// # (AxB)xN     =  (N*A)B - (N*B)*A         -- Lagrange's formula
// # ((AxB)xN)*M = ((N*A)B - (N*B)*A)*M
// #             = (N*A)(M*B) - (X*B)(M*A)
//
// ndota_ rnd = n0*a0 + n1*a1 + n2*a2;
// ndotb_ rnd = n0*b0 + n1*b1 + n2*b2;
// mdota_ rnd = m0*a0 + m1*a1 + m2*a2;
// mdotb_ rnd = m0*b0 + m1*b1 + m2*b2;
// prod0_ rnd = ndota_*mdotb_ - ndotb_*mdota_;
//
// ndotc_ rnd = n0*c0 + n1*c1 + n2*c2;
// ndotd_ rnd = n0*d0 + n1*d1 + n2*d2;
// mdotc_ rnd = m0*c0 + m1*c1 + m2*c2;
// mdotd_ rnd = m0*d0 + m1*d1 + m2*d2;
// prod1_ rnd = ndotc_*mdotd_ - ndotd_*mdotc_;
//
// diff_ rnd = prod1_ - prod0_;
//
// # Compute it all again in exact arithmetic.
// ndota = n0*a0 + n1*a1 + n2*a2;
// ndotb = n0*b0 + n1*b1 + n2*b2;
// mdota = m0*a0 + m1*a1 + m2*a2;
// mdotb = m0*b0 + m1*b1 + m2*b2;
// prod0 = ndota*mdotb - ndotb*mdota;
//
// ndotc = n0*c0 + n1*c1 + n2*c2;
// ndotd = n0*d0 + n1*d1 + n2*d2;
// mdotc = m0*c0 + m1*c1 + m2*c2;
// mdotd = m0*d0 + m1*d1 + m2*d2;
// prod1 = ndotc*mdotd - ndotd*mdotc;
//
// diff = prod1 - prod0;
//
// {
//   # A,B,C, and D are meant to be normalized S2Point values, so their
//   # magnitude will be at most 1.  M and N are allowed to be unnormalized cell
//   # edge normals, so their magnitude can be up to sqrt(2).  In each case the
//   # components will be at most one.
//      a0 in [-1, 1]
//   /\ a1 in [-1, 1]
//   /\ a2 in [-1, 1]
//   /\ b0 in [-1, 1]
//   /\ b1 in [-1, 1]
//   /\ b2 in [-1, 1]
//   /\ c0 in [-1, 1]
//   /\ c1 in [-1, 1]
//   /\ c2 in [-1, 1]
//   /\ d0 in [-1, 1]
//   /\ d1 in [-1, 1]
//   /\ d2 in [-1, 1]
//
//   /\ n0 in [-1, 1]
//   /\ n1 in [-1, 1]
//   /\ n2 in [-1, 1]
//   /\ m0 in [-1, 1]
//   /\ m1 in [-1, 1]
//   /\ m2 in [-1, 1]
//
//   # We always dot an unnormalized normal against a normalized point so the
//   # magnitude of the dot product in each case is bounded by sqrt(2).
//   /\ |ndota_| in [0, 1.4142135623730954]
//   /\ |ndotb_| in [0, 1.4142135623730954]
//   /\ |ndotc_| in [0, 1.4142135623730954]
//   /\ |ndotd_| in [0, 1.4142135623730954]
//
//   /\ |mdota_| in [0, 1.4142135623730954]
//   /\ |mdotb_| in [0, 1.4142135623730954]
//   /\ |mdotc_| in [0, 1.4142135623730954]
//   /\ |mdotd_| in [0, 1.4142135623730954]
//
//   ->
//   |diff_ - diff| in ?
// }
//
// > gappa proof.gappa
// Results:
//|  diff_ - diff| in [0, 1145679351550454559b-107 {7.06079e-15, 2^(-47.0091)}]
//
// >>> 1145679351550454559*2**-107/2**-52
// 31.79898987322334

// Gappa proof for TriageCircleEdgeIntersectionSign
//
// # Use IEEE754 double precision, round-to-nearest by default.
// @rnd = float<ieee_64, ne>;
//
// # Four vectors, two forming an edge AB and two normals (X,Y) for great
// circles. a0 = rnd(a0_ex); a1 = rnd(a1_ex); a2 = rnd(a2_ex); b0 =
// rnd(b0_ex); b1 = rnd(b1_ex); b2 = rnd(b2_ex); n0 = rnd(n0_ex); n1 =
// rnd(n1_ex); n2 = rnd(n2_ex); x0 = rnd(x0_ex); x1 = rnd(x1_ex); x2 =
// rnd(x2_ex);
//
// # (AxB)xX     =  (X*A)B - (X*B)*A         -- Lagrange's formula
// # ((AxB)xX)*Y = ((X*A)B - (X*B)*A)*Y
// #             = (X*A)(Y*B) - (X*B)(Y*A)
//
// ndota_ rnd = n0*a0 + n1*a1 + n2*a2;
// ndotb_ rnd = n0*b0 + n1*b1 + n2*b2;
// xdota_ rnd = x0*a0 + x1*a1 + x2*a2;
// xdotb_ rnd = x0*b0 + x1*b1 + x2*b2;
// diff_  rnd = ndota_*xdotb_ - ndotb_*xdota_;
//
// # Compute it all again in exact arithmetic.
// ndota = n0*a0 + n1*a1 + n2*a2;
// ndotb = n0*b0 + n1*b1 + n2*b2;
// xdota = x0*a0 + x1*a1 + x2*a2;
// xdotb = x0*b0 + x1*b1 + x2*b2;
// diff  = ndota*xdotb - ndotb*xdota;
//
// {
//   # A and B are meant to be normalized S2Point values, so their magnitude
//   will # be at most 1.  X and Y are allowed to be unnormalized cell edge
//   normals, so # their magnitude can be up to sqrt(2).  In each case the
//   components will be # at most one.
//      a0 in [-1, 1]
//   /\ a1 in [-1, 1]
//   /\ a2 in [-1, 1]
//   /\ b0 in [-1, 1]
//   /\ b1 in [-1, 1]
//   /\ b2 in [-1, 1]
//   /\ n0 in [-1, 1]
//   /\ n1 in [-1, 1]
//   /\ n2 in [-1, 1]
//   /\ x0 in [-1, 1]
//   /\ x1 in [-1, 1]
//   /\ x2 in [-1, 1]
//
//   # We always dot an unnormalized normal against a normalized point so the
//   # magnitude of the dot product in each case is bounded by sqrt(2).
//   /\ |ndota_| in [0, 1.4142135623730954]
//   /\ |ndotb_| in [0, 1.4142135623730954]
//   /\ |xdota_| in [0, 1.4142135623730954]
//   /\ |xdotb_| in [0, 1.4142135623730954]
//
//   ->
//   |diff_ - diff| in ?
// } 6ms
//
// > gappa proof.gappa
// Results:
//   |diff_ - diff| in [0, 1001564163474598623b-108 {3.08631e-15,
//   2^(-48.2031)}]
//
// >>> 1001564163474598623*2**-108/2**-52
// 13.89949493661167
