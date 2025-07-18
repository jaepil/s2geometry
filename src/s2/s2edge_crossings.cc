// Copyright 2005 Google Inc. All Rights Reserved.
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

#include "s2/s2edge_crossings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "absl/base/casts.h"
#include "absl/base/optimization.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/string_view.h"

#include "s2/s1angle.h"
#include "s2/s2edge_crosser.h"
#include "s2/s2edge_crossings_internal.h"
#include "s2/s2point.h"
#include "s2/s2pointutil.h"
#include "s2/s2predicates.h"
#include "s2/s2predicates_internal.h"
#include "s2/util/math/exactfloat/exactfloat.h"

namespace S2 {

using absl::string_view;
using internal::GetIntersectionExact;
using internal::intersection_method_tally_;
using internal::IntersectionMethod;
using S2::internal::GetStableCrossProd;
using s2pred::DBL_ERR;
using s2pred::kSqrt3;
using s2pred::rounding_epsilon;
using s2pred::ToExact;
using s2pred::ToLD;
using std::fabs;
using std::max;
using std::sqrt;

using Vector3_ld = s2pred::Vector3_ld;
using Vector3_xf = s2pred::Vector3_xf;

// kRobustCrossProdError can be set somewhat arbitrarily because the algorithm
// uses more precision as needed in order to achieve the specified error.  The
// only strict requirement is that kRobustCrossProdError >= DBL_ERR, since
// this is the minimum error even when using exact arithmetic.  We set the
// error somewhat larger than this so that virtually all cases can be handled
// using ordinary double-precision arithmetic.
static_assert(kRobustCrossProdError.radians() == 6 * DBL_ERR, "update comment");

// kIntersectionError can also be set somewhat arbitrarily (see above) except
// that in this case the error using exact arithmetic is up to 2 * DBL_ERR,
// and the error limit is set to 8 * DBL_ERR so that virtually all cases can
// be handled using ordinary double-precision arithmetic.
static_assert(kIntersectionError.radians() == 8 * DBL_ERR, "update comment");

namespace internal {

const S1Angle kExactCrossProdError = S1Angle::Radians(DBL_ERR);
const S1Angle kIntersectionExactError = S1Angle::Radians(2 * DBL_ERR);

int* intersection_method_tally_ = nullptr;

string_view GetIntersectionMethodName(IntersectionMethod method) {
  switch (method) {
    case IntersectionMethod::SIMPLE:    return "Simple";
    case IntersectionMethod::SIMPLE_LD: return "Simple_ld";
    case IntersectionMethod::STABLE:    return "Stable";
    case IntersectionMethod::STABLE_LD: return "Stable_ld";
    case IntersectionMethod::EXACT:     return "Exact";
    default:                            return "Unknown";
  }
}

// Evaluates the cross product of unit-length vectors "a" and "b" in a
// numerically stable way, returning true if the error in the result is
// guaranteed to be at most kRobustCrossProdError.
template <class T>
inline bool GetStableCrossProd(const Vector3<T>& a, const Vector3<T>& b,
                               Vector3<T>* result) {
  // We compute the cross product (a - b) x (a + b).  Mathematically this is
  // exactly twice the cross product of "a" and "b", but it has the numerical
  // advantage that (a - b) and (a + b) are nearly perpendicular (since "a" and
  // "b" are unit length).  This yields a result that is nearly orthogonal to
  // both "a" and "b" even if these two values differ only very slightly.
  //
  // The maximum directional error in radians when this calculation is done in
  // precision T (where T is a floating-point type) is:
  //
  //   (1 + 2 * sqrt(3) + 32 * sqrt(3) * DBL_ERR / ||N||) * T_ERR
  //
  // where ||N|| is the norm of the result.  To keep this error to at most
  // kRobustCrossProdError, assuming this value is much less than 1, we need
  //
  //   (1 + 2 * sqrt(3) + 32 * sqrt(3) * DBL_ERR / ||N||) * T_ERR <= kErr
  //
  //   ||N|| >= 32 * sqrt(3) * DBL_ERR / (kErr / T_ERR - (1 + 2 * sqrt(3)))
  //
  // From this you can see that in order for this calculation to ever succeed in
  // double precision, we must have kErr > (1 + 2 * sqrt(3)) * DBL_ERR, which is
  // about 4.46 * DBL_ERR.  We actually set kRobustCrossProdError == 6 * DBL_ERR
  // (== 3 * DBL_EPSILON) in order to minimize the number of cases where higher
  // precision is needed; in particular, higher precision is only necessary when
  // "a" and "b" are closer than about 18 * DBL_ERR == 9 * DBL_EPSILON.
  // (80-bit precision can handle inputs as close as 2.5 * LDBL_EPSILON.)
  constexpr T T_ERR = rounding_epsilon<T>();
  // `kMinNorm` should be `constexpr`, but we make it only `const` to work
  // around a gcc ppc64el bug with `long double`s.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107745
  // https://github.com/google/s2geometry/issues/279
  const T kMinNorm =
      (32 * kSqrt3 * DBL_ERR) /
      (kRobustCrossProdError.radians() / T_ERR - (1 + 2 * kSqrt3));

  *result = (a - b).CrossProd(a + b);
  return result->Norm2() >= kMinNorm * kMinNorm;
}

// Explicitly instantiate this function so that we can use it in tests without
// putting its definition in a header file.
template bool GetStableCrossProd<double>(
  const Vector3_d&, const Vector3_d&, Vector3_d*);
template bool GetStableCrossProd<long double>(
    const Vector3_ld&, const Vector3_ld&, Vector3_ld*);

}  // namespace internal

S2Point RobustCrossProd(const S2Point& a, const S2Point& b) {
  ABSL_DCHECK(IsUnitLength(a));
  ABSL_DCHECK(IsUnitLength(b));

  // The direction of a.CrossProd(b) becomes unstable as (a + b) or (a - b)
  // approaches zero.  This leads to situations where a.CrossProd(b) is not
  // very orthogonal to "a" and/or "b".  To solve this problem robustly requires
  // falling back to extended precision, arbitrary precision, and even symbolic
  // perturbations to handle the case when "a" and "b" are exactly
  // proportional, e.g. a == -b (see s2predicates.cc for details).
  Vector3_d result;
  if (GetStableCrossProd(a, b, &result)) {
    return result;
  }
  // Handle the (a == b) case now, before doing expensive arithmetic.  The only
  // result that makes sense mathematically is to return zero, but it turns out
  // to reduce the number of special cases in client code if we instead return
  // an arbitrary orthogonal vector.
  if (a == b) {
    return Ortho(a);
  }
  constexpr bool kUseLongDoubleInRobustCrossProd = s2pred::kHasLongDouble;
  // Next we try using "long double" precision (if available).
  Vector3_ld result_ld;
  if (kUseLongDoubleInRobustCrossProd &&
      GetStableCrossProd(ToLD(a), ToLD(b), &result_ld)) {
    return Vector3_d::Cast(result_ld);
  }
  // Otherwise we fall back to exact arithmetic, then symbolic perturbations.
  return internal::ExactCrossProd(a, b);
}

// Returns the cross product of "a" and "b" after symbolic perturbations.
// (These perturbations only affect the result if "a" and "b" are exactly
// collinear, e.g. if a == -b or a == (1+eps) * b.)  The result may not be
// normalizable (i.e., EnsureNormalizable() should be called on the result).
static Vector3_d SymbolicCrossProdSorted(const S2Point& a, const S2Point& b) {
  ABSL_DCHECK(a < b);
  ABSL_DCHECK(s2pred::IsZero(ToExact(a).CrossProd(ToExact(b))));

  // The following code uses the same symbolic perturbation model as S2::Sign.
  // The particular sequence of tests below was obtained using Mathematica
  // (although it would be easy to do it by hand for this simple case).
  //
  // Just like the function SymbolicallyPerturbedSign() in s2predicates.cc,
  // every input coordinate x[i] is assigned a symbolic perturbation dx[i].  We
  // then compute the cross product
  //
  //     (a + da).CrossProd(b + db) .
  //
  // The result is a polynomial in the perturbation symbols.  For example if we
  // did this in one dimension, the result would be
  //
  //     a * b + b * da + a * db + da * db
  //
  // where "a" and "b" have numerical values and "da" and "db" are symbols.
  // In 3 dimensions the result is similar except that the coefficients are
  // 3-vectors rather than scalars.
  //
  // Every possible S2Point has its own symbolic perturbation in each coordinate
  // (i.e., there are about 3 * 2**192 symbols).  The magnitudes of the
  // perturbations are chosen such that if x < y lexicographically, the
  // perturbations for "y" are much smaller than the perturbations for "x".
  // Similarly, the perturbations for the coordinates of a given point x are
  // chosen such that dx[0] is much smaller than dx[1] which is much smaller
  // than dx[2].  Putting this together with fact the inputs to this function
  // have been sorted so that a < b lexicographically, this tells us that
  //
  //     da[2] > da[1] > da[0] > db[2] > db[1] > db[0]
  //
  // where each perturbation is so much smaller than the previous one that we
  // don't even need to consider it unless the coefficients of all previous
  // perturbations are zero.  In fact, each succeeding perturbation is so small
  // that we don't need to consider it unless the coefficient of all products of
  // the previous perturbations are zero.  For example, we don't need to
  // consider the coefficient of db[1] unless the coefficient of db[2]*da[0] is
  // zero.
  //
  // The follow code simply enumerates the coefficients of the perturbations
  // (and products of perturbations) that appear in the cross product above, in
  // order of decreasing perturbation magnitude.  The first non-zero
  // coefficient determines the result.  The easiest way to enumerate the
  // coefficients in the correct order is to pretend that each perturbation is
  // some tiny value "eps" raised to a power of two:
  //
  // eps**    1      2      4      8     16     32
  //        da[2]  da[1]  da[0]  db[2]  db[1]  db[0]
  //
  // Essentially we can then just count in binary and test the corresponding
  // subset of perturbations at each step.  So for example, we must test the
  // coefficient of db[2]*da[0] before db[1] because eps**12 > eps**16.

  if (b[0] != 0 || b[1] != 0) {           // da[2]
    return Vector3_d(-b[1], b[0], 0);
  }
  if (b[2] != 0) {                        // da[1]
    return Vector3_d(b[2], 0, 0);         // Note that b[0] == 0.
  }

  // None of the remaining cases can occur in practice, because we can only get
  // to this point if b = (0, 0, 0).  Nevertheless, even (0, 0, 0) has a
  // well-defined direction under the symbolic perturbation model.
  ABSL_DCHECK(b[1] == 0 && b[2] == 0);  // da[0] coefficients (always zero)

  if (a[0] != 0 || a[1] != 0) {          // db[2]
    return Vector3_d(a[1], -a[0], 0);
  }

  // The following coefficient is always non-zero, so we can stop here.
  //
  // It may seem strange that we are returning (1, 0, 0) as the cross product
  // without even looking at the sign of a[2].  (Wouldn't you expect
  // (0, 0, -1) x (0, 0, 0) and (0, 0, 1) x (0, 0, 0) to point in opposite
  // directions?)  It's worth pointing out that in this function there is *no
  // relationship whatsoever* between the vectors "a" and "-a", because the
  // perturbations applied to these vectors may be entirely different.  This is
  // why the identity "RobustCrossProd(-a, b) == -RobustCrossProd(a, b)" does
  // not hold whenever "a" and "b" are linearly dependent (i.e., proportional).
  // [As it happens the two cross products above actually do point in opposite
  // directions, but for example (1, 1, 1) x (2, 2, 2) = (-2, 2, 0) and
  // (-1, -1, -1) x (2, 2, 2) = (-2, 2, 0) do not.]
  return Vector3_d(1, 0, 0);                   // db[2] * da[1]
}

// Returns true if the given vector's magnitude is large enough such that the
// angle to another vector of the same magnitude can be measured using Angle()
// without loss of precision due to floating-point underflow.  (This requirement
// is also sufficient to ensure that Normalize() can be called without risk of
// precision loss.)
inline static bool IsNormalizable(const Vector3_d& p) {
  // Let ab = RobustCrossProd(a, b) and cd = RobustCrossProd(cd).  In order for
  // ab.Angle(cd) to not lose precision, the squared magnitudes of ab and cd
  // must each be at least 2**-484.  This ensures that the sum of the squared
  // magnitudes of ab.CrossProd(cd) and ab.DotProd(cd) is at least 2**-968,
  // which ensures that any denormalized terms in these two calculations do
  // not affect the accuracy of the result (since all denormalized numbers are
  // smaller than 2**-1022, which is less than DBL_ERR * 2**-968).
  //
  // The fastest way to ensure this is to test whether the largest component of
  // the result has a magnitude of at least 2**-242.
  return max(fabs(p[0]), max(fabs(p[1]), fabs(p[2]))) >= ldexp(1, -242);
}

// Scales a 3-vector as necessary to ensure that the result can be normalized
// without loss of precision due to floating-point underflow.
//
// REQUIRES: p != (0, 0, 0)
inline static Vector3_d EnsureNormalizable(const Vector3_d& p) {
  ABSL_DCHECK_NE(p, Vector3_d(0, 0, 0));
  if (!IsNormalizable(p)) {
    // We can't just scale by a fixed factor because the smallest representable
    // double is 2**-1074, so if we multiplied by 2**(1074 - 242) then the
    // result might be so large that we couldn't square it without overflow.
    //
    // Note that we must scale by a power of two to avoid rounding errors,
    // and that the calculation of "pmax" is free because IsNormalizable()
    // is inline.  The code below scales "p" such that the largest component is
    // in the range [1, 2).
    double p_max = max(fabs(p[0]), max(fabs(p[1]), fabs(p[2])));

    // The expression below avoids signed overflow for any value of ilogb().
    return ldexp(2, -1 - ilogb(p_max)) * p;
  }
  return p;
}

// Converts an ExactFloat vector to a double-precision vector, scaling the
// result as necessary to ensure that the result can be normalized without loss
// of precision due to floating-point underflow.  (This method doesn't actually
// call Normalize() since that would create additional error in situations
// where normalization is not necessary.)
static Vector3_d NormalizableFromExact(const Vector3_xf& xf) {
  Vector3_d x(xf[0].ToDouble(), xf[1].ToDouble(), xf[2].ToDouble());
  if (IsNormalizable(x)) {
    return x;
  }
  // Scale so that the largest component magnitude is in the range [0.5, 1).
  // Note that the exponents involved could be much smaller than those
  // representable by an IEEE double precision float.
  int exp = ExactFloat::kMinExp - 1;
  for (int i = 0; i < 3; ++i) {
    if (xf[i].is_normal()) exp = std::max(exp, xf[i].exp());
  }
  if (exp < ExactFloat::kMinExp) {
    return Vector3_d(0, 0, 0);  // The exact result is (0, 0, 0).
  }
  return Vector3_d(ldexp(xf[0], -exp).ToDouble(),
                   ldexp(xf[1], -exp).ToDouble(),
                   ldexp(xf[2], -exp).ToDouble());
}

namespace internal {

Vector3_d SymbolicCrossProd(const S2Point& a, const S2Point& b) {
  ABSL_DCHECK_NE(a, b);
  // SymbolicCrossProdSorted() requires that a < b.
  if (a < b) {
    return EnsureNormalizable(SymbolicCrossProdSorted(a, b));
  } else {
    return -EnsureNormalizable(SymbolicCrossProdSorted(b, a));
  }
}
Vector3_d ExactCrossProd(const S2Point& a, const S2Point& b) {
  ABSL_DCHECK_NE(a, b);
  Vector3_xf result_xf = ToExact(a).CrossProd(ToExact(b));
  if (!s2pred::IsZero(result_xf)) {
    return NormalizableFromExact(result_xf);
  }
  // SymbolicCrossProd() requires that a < b.
  if (a < b) {
    return EnsureNormalizable(SymbolicCrossProd(a, b));
  } else {
    return -EnsureNormalizable(SymbolicCrossProd(b, a));
  }
}

}  // namespace internal

int CrossingSign(const S2Point& a, const S2Point& b,
                 const S2Point& c, const S2Point& d) {
  S2EdgeCrosser crosser(&a, &b, &c);
  return crosser.CrossingSign(&d);
}

bool VertexCrossing(const S2Point& a, const S2Point& b,
                    const S2Point& c, const S2Point& d) {
  // If A == B or C == D there is no intersection.  We need to check this
  // case first in case 3 or more input points are identical.
  if (a == b || c == d) return false;

  // If any other pair of vertices is equal, there is a crossing if and only
  // if OrderedCCW() indicates that the edge AB is further CCW around the
  // shared vertex O (either A or B) than the edge CD, starting from an
  // arbitrary fixed reference point.
  //
  // Optimization: if AB=CD or AB=DC, we can avoid most of the calculations.
  if (a == c) return (b == d) || s2pred::OrderedCCW(S2::RefDir(a), d, b, a);
  if (b == d) return s2pred::OrderedCCW(S2::RefDir(b), c, a, b);

  if (a == d) return (b == c) || s2pred::OrderedCCW(S2::RefDir(a), c, b, a);
  if (b == c) return s2pred::OrderedCCW(S2::RefDir(b), d, a, b);

  ABSL_LOG(ERROR) << "VertexCrossing called with 4 distinct vertices";
  return false;
}

int SignedVertexCrossing(const S2Point& a, const S2Point& b,
                         const S2Point& c, const S2Point& d) {
  if (a == b || c == d) return 0;

  // See VertexCrossing.  The sign of the crossing is +1 if both edges are
  // outgoing or both edges are incoming with respect to the common vertex
  // and -1 otherwise.
  if (a == c) {
    return ((b == d) || s2pred::OrderedCCW(S2::RefDir(a), d, b, a)) ? 1 : 0;
  }
  if (b == d) return s2pred::OrderedCCW(S2::RefDir(b), c, a, b) ? 1 : 0;

  if (a == d) {
    return ((b == c) || s2pred::OrderedCCW(S2::RefDir(a), c, b, a)) ? -1 : 0;
  }
  if (b == c) return s2pred::OrderedCCW(S2::RefDir(b), d, a, b) ? -1 : 0;

  ABSL_LOG(ERROR) << "SignedVertexCrossing called with 4 distinct vertices";
  return 0;
}

bool EdgeOrVertexCrossing(const S2Point& a, const S2Point& b,
                          const S2Point& c, const S2Point& d) {
  int crossing = CrossingSign(a, b, c, d);
  if (crossing < 0) return false;
  if (crossing > 0) return true;
  return VertexCrossing(a, b, c, d);
}

// Computes the cross product of "x" and "y", normalizes it to be unit length,
// and stores the result in "result".  Also returns the length of the cross
// product before normalization, which is useful for estimating the amount of
// error in the result.  For numerical stability, "x" and "y" should both be
// approximately unit length.
template <class T>
static T RobustNormalWithLength(const Vector3<T>& x, const Vector3<T>& y,
                                Vector3<T>* result) {
  // This computes 2 * (x.CrossProd(y)), but has much better numerical
  // stability when "x" and "y" are unit length.
  Vector3<T> tmp = (x - y).CrossProd(x + y);
  T length = tmp.Norm();
  if (length != 0) {
    *result = (1 / length) * tmp;
  }
  return 0.5 * length;  // Since tmp == 2 * (x.CrossProd(y))
}

// If the intersection point of the edges (a0,a1) and (b0,b1) can be computed
// to within an error of at most kIntersectionError by this function, then set
// "result" to the intersection point and return true.
template <class T>
static bool GetIntersectionSimple(const Vector3<T>& a0, const Vector3<T>& a1,
                                  const Vector3<T>& b0, const Vector3<T>& b1,
                                  Vector3<T>* result) {
  // The code below computes the intersection point as
  //
  //    (a0.CrossProd(a1)).CrossProd(b0.CrossProd(b1))
  //
  // except that it has better numerical stability and also computes a
  // guaranteed error bound.
  //
  // Each cross product is computed as (X-Y).CrossProd(X+Y) using unit-length
  // input vectors, which eliminates most of the cancellation error.  However
  // the error in the direction of the cross product can still become large if
  // the two points are extremely close together.  We can show that as long as
  // the length of the cross product is at least (16 * sqrt(3) + 24) * DBL_ERR
  // (about 6e-15), then the directional error is at most 5 * T_ERR (about
  // 3e-19 when T == "long double").  (DBL_ERR appears in the first formula
  // because the inputs are assumed to be normalized in double precision
  // rather than in the given type T.)
  //
  // The third cross product is different because its inputs already have some
  // error.  Letting "result_len" be the length of the cross product, it can
  // be shown that the error is at most
  //
  //   (2 + 2 * sqrt(3) + 12 / result_len) * T_ERR
  //
  // We want this error to be at most kIntersectionError, which is true as
  // long as "result_len" is at least kMinResultLen defined below.

  constexpr T T_ERR = rounding_epsilon<T>();
  constexpr T kMinNormalLength = (16 * kSqrt3 + 24) * DBL_ERR;
  constexpr T kMinResultLen =
      12 / (kIntersectionError.radians() / T_ERR - (2 + 2 * kSqrt3));

  // On some platforms "long double" is the same as "double", and on these
  // platforms this method always returns false (e.g. ARM32, Win32).  Rather
  // than testing this directly, instead we look at kMinResultLen since this
  // is a direct measure of whether "long double" has sufficient accuracy to
  // be useful.  If kMinResultLen >= 0.5, it means that this method will fail
  // even for edges that meet at an angle of 30 degrees.  (On Intel platforms
  // kMinResultLen corresponds to an intersection angle of about 0.04
  // degrees.)
  if (kMinResultLen >= 0.5) return false;

  Vector3<T> a_norm, b_norm;
  if (RobustNormalWithLength(a0, a1, &a_norm) >= kMinNormalLength &&
      RobustNormalWithLength(b0, b1, &b_norm) >= kMinNormalLength &&
      RobustNormalWithLength(a_norm, b_norm, result) >= kMinResultLen) {
    // Make sure that we return the intersection point rather than its antipode.
    *result *= (a_norm.DotProd(b1 - b0) < 0) ? -1 : 1;
    return true;
  }
  return false;
}

static bool GetIntersectionSimpleLD(const S2Point& a0, const S2Point& a1,
                                    const S2Point& b0, const S2Point& b1,
                                    S2Point* result) {
  Vector3_ld result_ld;
  if (GetIntersectionSimple(ToLD(a0), ToLD(a1), ToLD(b0), ToLD(b1),
                            &result_ld)) {
    *result = S2Point::Cast(result_ld);
    return true;
  }
  return false;
}

// Given a point X and a vector "a_norm" (not necessarily unit length),
// compute x.DotProd(a_norm) and return a bound on the error in the result.
// The remaining parameters allow this dot product to be computed more
// accurately and efficiently.  They include the length of "a_norm"
// ("a_norm_len") and the edge endpoints "a0" and "a1".
template <class T>
static T GetProjection(const Vector3<T>& x,
                       const Vector3<T>& a_norm, T a_norm_len,
                       const Vector3<T>& a0, const Vector3<T>& a1,
                       T* error) {
  // The error in the dot product is proportional to the lengths of the input
  // vectors, so rather than using "x" itself (a unit-length vector) we use
  // the vectors from "x" to the closer of the two edge endpoints.  This
  // typically reduces the error by a huge factor.
  Vector3<T> x0 = x - a0;
  Vector3<T> x1 = x - a1;
  T x0_dist2 = x0.Norm2();
  T x1_dist2 = x1.Norm2();

  // If both distances are the same, we need to be careful to choose one
  // endpoint deterministically so that the result does not change if the
  // order of the endpoints is reversed.
  T dist, result;
  if (x0_dist2 < x1_dist2 || (x0_dist2 == x1_dist2 && x0 < x1)) {
    dist = sqrt(x0_dist2);
    result = x0.DotProd(a_norm);
  } else {
    dist = sqrt(x1_dist2);
    result = x1.DotProd(a_norm);
  }
  // This calculation bounds the error from all sources: the computation of
  // the normal, the subtraction of one endpoint, and the dot product itself.
  // (DBL_ERR appears because the input points are assumed to be normalized in
  // double precision rather than in the given type T.)
  //
  // For reference, the bounds that went into this calculation are:
  // ||N'-N|| <= ((1 + 2 * sqrt(3))||N|| + 32 * sqrt(3) * DBL_ERR) * T_ERR
  // |(A.B)'-(A.B)| <= (1.5 * (A.B) + 1.5 * ||A|| * ||B||) * T_ERR
  // ||(X-Y)'-(X-Y)|| <= ||X-Y|| * T_ERR
  constexpr T T_ERR = rounding_epsilon<T>();
  *error = (((3.5 + 2 * kSqrt3) * a_norm_len + 32 * kSqrt3 * DBL_ERR)
            * dist + 1.5 * fabs(result)) * T_ERR;
  return result;
}

// Helper function for GetIntersectionStable().  It expects that the edges
// (a0,a1) and (b0,b1) have been sorted so that the first edge is longer.
template <class T>
static bool GetIntersectionStableSorted(
    const Vector3<T>& a0, const Vector3<T>& a1,
    const Vector3<T>& b0, const Vector3<T>& b1, Vector3<T>* result) {
  ABSL_DCHECK_GE((a1 - a0).Norm2(), (b1 - b0).Norm2());

  // Compute the normal of the plane through (a0, a1) in a stable way.
  Vector3<T> a_norm = (a0 - a1).CrossProd(a0 + a1);
  T a_norm_len = a_norm.Norm();
  T b_len = (b1 - b0).Norm();

  // Compute the projection (i.e., signed distance) of b0 and b1 onto the
  // plane through (a0, a1).  Distances are scaled by the length of a_norm.
  T b0_error, b1_error;
  T b0_dist = GetProjection(b0, a_norm, a_norm_len, a0, a1, &b0_error);
  T b1_dist = GetProjection(b1, a_norm, a_norm_len, a0, a1, &b1_error);

  // The total distance from b0 to b1 measured perpendicularly to (a0,a1) is
  // |b0_dist - b1_dist|.  Note that b0_dist and b1_dist generally have
  // opposite signs because b0 and b1 are on opposite sides of (a0, a1).  The
  // code below finds the intersection point by interpolating along the edge
  // (b0, b1) to a fractional distance of b0_dist / (b0_dist - b1_dist).
  //
  // It can be shown that the maximum error in the interpolation fraction is
  //
  //     (b0_dist * b1_error - b1_dist * b0_error) /
  //        (dist_sum * (dist_sum - error_sum))
  //
  // We save ourselves some work by scaling the result and the error bound by
  // "dist_sum", since the result is normalized to be unit length anyway.
  //
  // Make sure that we return the intersection point rather than its antipode.
  // It is sufficient to ensure that (b0_dist - b1_dist) is non-negative.
  if (b0_dist < b1_dist) {
    b0_dist = -b0_dist;
    b1_dist = -b1_dist;
  }
  T dist_sum = b0_dist - b1_dist;
  T error_sum = b0_error + b1_error;
  if (dist_sum <= error_sum) {
    return false;  // Error is unbounded in this case.
  }
  Vector3<T> x = b0_dist * b1 - b1_dist * b0;
  constexpr T T_ERR = rounding_epsilon<T>();
  T error = b_len * fabs(b0_dist * b1_error - b1_dist * b0_error) /
      (dist_sum - error_sum) + 2 * T_ERR * dist_sum;

  // Finally we normalize the result, compute the corresponding error, and
  // check whether the total error is acceptable.
  T x_len2 = x.Norm2();
  if (x_len2 < std::numeric_limits<T>::min()) {
    // If x.Norm2() is less than the minimum normalized value of T, x_len might
    // lose precision and the result might fail to satisfy S2::IsUnitLength().
    // TODO(ericv): Implement S2::RobustNormalize().
    return false;
  }
  T x_len = sqrt(x_len2);
  const T kMaxError = kIntersectionError.radians();
  if (error > (kMaxError - T_ERR) * x_len) {
    return false;
  }
  *result = (1 / x_len) * x;
  return true;
}

// If the intersection point of the edges (a0,a1) and (b0,b1) can be computed
// to within an error of at most kIntersectionError by this function, then set
// "result" to the intersection point and return true.
template <class T>
static bool GetIntersectionStable(const Vector3<T>& a0, const Vector3<T>& a1,
                                  const Vector3<T>& b0, const Vector3<T>& b1,
                                  Vector3<T>* result) {
  // Sort the two edges so that (a0,a1) is longer, breaking ties in a
  // deterministic way that does not depend on the ordering of the endpoints.
  // This is desirable for two reasons:
  //  - So that the result doesn't change when edges are swapped or reversed.
  //  - It reduces error, since the first edge is used to compute the edge
  //    normal (where a longer edge means less error), and the second edge
  //    is used for interpolation (where a shorter edge means less error).
  T a_len2 = (a1 - a0).Norm2();
  T b_len2 = (b1 - b0).Norm2();
  if (a_len2 < b_len2 ||
      (a_len2 == b_len2 && internal::CompareEdges(a0, a1, b0, b1))) {
    return GetIntersectionStableSorted(b0, b1, a0, a1, result);
  } else {
    return GetIntersectionStableSorted(a0, a1, b0, b1, result);
  }
}

inline static S2Point ToS2Point(const Vector3_xf& xf) {
  return NormalizableFromExact(xf).Normalize();
}

namespace internal {

bool GetIntersectionStableLD(const S2Point& a0, const S2Point& a1,
                             const S2Point& b0, const S2Point& b1,
                             S2Point* result) {
  Vector3_ld result_ld;
  if (GetIntersectionStable(ToLD(a0), ToLD(a1), ToLD(b0), ToLD(b1),
                            &result_ld)) {
    *result = S2Point::Cast(result_ld);
    return true;
  }
  return false;
}

// Compute the intersection point of (a0, a1) and (b0, b1) using exact
// arithmetic.  Note that the result is not exact because it is rounded to
// double precision.
S2Point GetIntersectionExact(const S2Point& a0, const S2Point& a1,
                             const S2Point& b0, const S2Point& b1) {
  // Since we are using exact arithmetic, we don't need to worry about
  // numerical stability.
  Vector3_xf a_norm_xf = ToExact(a0).CrossProd(ToExact(a1));
  Vector3_xf b_norm_xf = ToExact(b0).CrossProd(ToExact(b1));
  Vector3_xf x_xf = a_norm_xf.CrossProd(b_norm_xf);

  // The final Normalize() call is done in double precision, which creates a
  // directional error of up to 2 * DBL_ERR.  (NormalizableFromExact() and
  // Normalize() each contribute up to DBL_ERR of directional error.)
  if (!s2pred::IsZero(x_xf)) {
    // Make sure that we return the intersection point rather than its antipode.
    return s2pred::Sign(a0, a1, b1) * ToS2Point(x_xf);
  }

  // The two edges are exactly collinear, but we still consider them to be
  // "crossing" because of simulation of simplicity.  The most principled way to
  // handle this situation is to use symbolic perturbations, similar to what
  // S2::RobustCrossProd and s2pred::Sign do.  This is certainly possible, but
  // it turns out that there are approximately 18 cases to consider (compared to
  // the 4 cases for RobustCrossProd and 13 for s2pred::Sign).
  //
  // For now we use a heuristic that simply chooses a plausible intersection
  // point.  Out of the four endpoints, exactly two lie in the interior of the
  // other edge.  Of those two we return the one that is lexicographically
  // smallest.
  S2Point a_norm = ToS2Point(a_norm_xf);
  S2Point b_norm = ToS2Point(b_norm_xf);
  if (a_norm == S2Point(0, 0, 0)) a_norm = SymbolicCrossProd(a0, a1);
  if (b_norm == S2Point(0, 0, 0)) b_norm = SymbolicCrossProd(b0, b1);

  S2Point x(10, 10, 10);  // Greater than any valid S2Point.
  if (s2pred::OrderedCCW(b0, a0, b1, b_norm) && a0 < x) x = a0;
  if (s2pred::OrderedCCW(b0, a1, b1, b_norm) && a1 < x) x = a1;
  if (s2pred::OrderedCCW(a0, b0, a1, a_norm) && b0 < x) x = b0;
  if (s2pred::OrderedCCW(a0, b1, a1, a_norm) && b1 < x) x = b1;

  ABSL_DCHECK(S2::IsUnitLength(x));
  return x;
}

}  // namespace internal

// Given three points "a", "x", "b", returns true if these three points occur
// in the given order along the edge (a,b) to within the given tolerance.
// More precisely, either "x" must be within "tolerance" of "a" or "b", or
// when "x" is projected onto the great circle through "a" and "b" it must lie
// along the edge (a,b) (i.e., the shortest path from "a" to "b").
static bool ApproximatelyOrdered(const S2Point& a, const S2Point& x,
                                 const S2Point& b, double tolerance) {
  if ((x - a).Norm2() <= tolerance * tolerance) return true;
  if ((x - b).Norm2() <= tolerance * tolerance) return true;
  return s2pred::OrderedCCW(a, x, b, S2::RobustCrossProd(a, b).Normalize());
}

S2Point GetIntersection(const S2Point& a0, const S2Point& a1,
                        const S2Point& b0, const S2Point& b1) {
  ABSL_DCHECK_GT(CrossingSign(a0, a1, b0, b1), 0);

  // It is difficult to compute the intersection point of two edges accurately
  // when the angle between the edges is very small.  Previously we handled
  // this by only guaranteeing that the returned intersection point is within
  // kIntersectionError of each edge.  However, this means that when the edges
  // cross at a very small angle, the computed result may be very far from the
  // true intersection point.
  //
  // Instead this function now guarantees that the result is always within
  // kIntersectionError of the true intersection.  This requires using more
  // sophisticated techniques and in some cases extended precision.
  //
  // Three different techniques are implemented, but only two are used:
  //
  //  - GetIntersectionSimple() computes the intersection point using
  //    numerically stable cross products in "long double" precision.
  //
  //  - GetIntersectionStable() computes the intersection point using
  //    projection and interpolation, taking care to minimize cancellation
  //    error.  This method exists in "double" and "long double" versions.
  //
  //  - GetIntersectionExact() computes the intersection point using exact
  //    arithmetic and converts the final result back to an S2Point.
  //
  // We don't actually use the first method (GetIntersectionSimple) because it
  // turns out that GetIntersectionStable() is twice as fast and also much
  // more accurate (even in double precision). The "long double" version
  // (only available on some platforms) uses 80-bit precision on x64 and
  // 128-bit precision on ARM64, and is ~7x slower on x86. The exact arithmetic
  // version is about 140x slower than "double" on x86.
  //
  // So our strategy is to first call GetIntersectionStable() in double
  // precision; if that doesn't work then we fall back to exact arithmetic.
  // Because "long double" version gives different results depending on the
  // platform, it is not used in this function.

  // TODO(b/365080041): consider moving the unused implementations to the test
  // so they can be benchmarked and compared for accuracy.
  constexpr bool kUseSimpleMethod = false;
  // Long double version produces different results on x86-64 and ARM64
  // platforms, and is not used in this function.
  constexpr bool kUseLongDoubleInIntersection = s2pred::kHasLongDouble && false;
  S2Point result;
  IntersectionMethod method;
  if (kUseSimpleMethod && GetIntersectionSimple(a0, a1, b0, b1, &result)) {
    method = IntersectionMethod::SIMPLE;
  } else if (kUseSimpleMethod && kUseLongDoubleInIntersection &&
             GetIntersectionSimpleLD(a0, a1, b0, b1, &result)) {
    method = IntersectionMethod::SIMPLE_LD;
  } else if (GetIntersectionStable(a0, a1, b0, b1, &result)) {
    method = IntersectionMethod::STABLE;
  } else if (kUseLongDoubleInIntersection &&
             internal::GetIntersectionStableLD(a0, a1, b0, b1, &result)) {
    method = IntersectionMethod::STABLE_LD;
  } else {
    result = GetIntersectionExact(a0, a1, b0, b1);
    method = IntersectionMethod::EXACT;
  }
  if (intersection_method_tally_) {
    ++intersection_method_tally_[static_cast<int>(method)];
  }

  // Make sure that the intersection point lies on both edges.
  ABSL_DCHECK(
      ApproximatelyOrdered(a0, result, a1, kIntersectionError.radians()));
  ABSL_DCHECK(
      ApproximatelyOrdered(b0, result, b1, kIntersectionError.radians()));

  return result;
}

}  // namespace S2
