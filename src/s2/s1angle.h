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

#ifndef S2_S1ANGLE_H_
#define S2_S1ANGLE_H_

#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
#include <type_traits>

#include "absl/log/absl_check.h"
#include "s2/util/coding/coder.h"
#include "s2/_fp_contract_off.h"  // IWYU pragma: keep
#include "s2/s2coder.h"
#include "s2/s2error.h"
#include "s2/s2point.h"
#include "s2/util/math/mathutil.h"

class S2LatLng;

// This class represents a one-dimensional angle (as opposed to a
// two-dimensional solid angle).  It has methods for converting angles to
// or from radians, degrees, and the E5/E6/E7 representations (i.e. degrees
// multiplied by 1e5/1e6/1e7 and rounded to the nearest integer).
//
// The internal representation is a double-precision value in radians, so
// conversion to and from radians is exact.  Conversions between E5, E6, E7,
// and Degrees are not always exact; for example, Degrees(3.1) is different
// from E6(3100000) or E7(310000000).  However, the following properties are
// guaranteed for any integer "n", provided that "n" is in the input range of
// both functions:
//
//     Degrees(n) == E6(1000000 * n)
//     Degrees(n) == E7(10000000 * n)
//          E6(n) == E7(10 * n)
//
// The corresponding properties are *not* true for E5, so if you use E5 then
// don't test for exact equality when comparing to other formats such as
// Degrees or E7.
//
// The following conversions between degrees and radians are exact:
//
//          Degrees(180) == Radians(M_PI)
//       Degrees(45 * k) == Radians(k * M_PI / 4)  for k == 0..8
//
// These identities also hold when the arguments are scaled up or down by any
// power of 2.  Some similar identities are also true, for example,
// Degrees(60) == Radians(M_PI / 3), but be aware that this type of identity
// does not hold in general.  For example, Degrees(3) != Radians(M_PI / 60).
//
// Similarly, the conversion to radians means that Angle::Degrees(x).degrees()
// does not always equal "x".  For example,
//
//         S1Angle::Degrees(45 * k).degrees() == 45 * k      for k == 0..8
//   but       S1Angle::Degrees(60).degrees() != 60.
//
// This means that when testing for equality, you should allow for numerical
// errors (EXPECT_DOUBLE_EQ) or convert to discrete E5/E6/E7 values first.
//
// CAVEAT: All of the above properties depend on "double" being the usual
// 64-bit IEEE 754 type (which is true on almost all modern platforms).
//
// This class is intended to be copied by value as desired.  It uses
// the default copy constructor and assignment operator.
class S1Angle {
 public:
  // These methods construct S1Angle objects from their measure in radians
  // or degrees.
  static constexpr S1Angle Radians(double radians);
  static constexpr S1Angle Degrees(double degrees);
  static constexpr S1Angle E5(int32_t e5);
  static constexpr S1Angle E6(int32_t e6);
  static constexpr S1Angle E7(int32_t e7);

  // Convenience functions -- to use when args have been fixed32s in protos.
  //
  // The arguments are static_cast into int32_t, so very large unsigned values
  // are treated as negative numbers.
  static constexpr S1Angle UnsignedE6(uint32_t e6);
  static constexpr S1Angle UnsignedE7(uint32_t e7);

  // The default constructor yields a zero angle.  This is useful for STL
  // containers and class methods with output arguments.
  constexpr S1Angle() = default;

  // Return an angle larger than any finite angle.
  static constexpr S1Angle Infinity();

  // A explicit shorthand for the default constructor.
  static constexpr S1Angle Zero();

  // Return the angle between two points, which is also equal to the distance
  // between these points on the unit sphere.  The points do not need to be
  // normalized.  This function has a maximum error of 3.25 * DBL_EPSILON (or
  // 2.5 * DBL_EPSILON for angles up to 1 radian). If either point is
  // zero-length (e.g. an uninitialized S2Point), or almost zero-length, the
  // resulting angle will be zero.
  S1Angle(const S2Point& x, const S2Point& y);

  // Like the constructor above, but return the angle (i.e., distance) between
  // two S2LatLng points.  This function has about 15 digits of accuracy for
  // small distances but only about 8 digits of accuracy as the distance
  // approaches 180 degrees (i.e., nearly-antipodal points).
  S1Angle(const S2LatLng& x, const S2LatLng& y);

  constexpr double radians() const;
  constexpr double degrees() const;

  // Returns the E5/E6/E7 representation (i.e. degrees multiplied by
  // 1e5/1e6/1e7 and rounded to the nearest integer).  The angle in degrees
  // must be in the interval (-180, 180].  (`Normalize` and `Normalized`
  // will ensure this.)
  int32_t e5() const;
  int32_t e6() const;
  int32_t e7() const;

  // Return the absolute value of an angle.
  S1Angle abs() const;
  friend S1Angle abs(S1Angle a);

  // Comparison operators.
  friend constexpr bool operator==(S1Angle x, S1Angle y);
  friend constexpr bool operator!=(S1Angle x, S1Angle y);
  friend constexpr bool operator<(S1Angle x, S1Angle y);
  friend constexpr bool operator>(S1Angle x, S1Angle y);
  friend constexpr bool operator<=(S1Angle x, S1Angle y);
  friend constexpr bool operator>=(S1Angle x, S1Angle y);

  // Simple arithmetic operators for manipulating S1Angles.
  friend constexpr S1Angle operator-(S1Angle a);
  friend constexpr S1Angle operator+(S1Angle a, S1Angle b);
  friend constexpr S1Angle operator-(S1Angle a, S1Angle b);
  friend constexpr S1Angle operator*(double m, S1Angle a);
  friend constexpr S1Angle operator*(S1Angle a, double m);
  friend constexpr S1Angle operator/(S1Angle a, double m);
  friend constexpr double operator/(S1Angle a, S1Angle b);
  S1Angle& operator+=(S1Angle a);
  S1Angle& operator-=(S1Angle a);
  S1Angle& operator*=(double m);
  S1Angle& operator/=(double m);

  // Trigonometric functions (not necessary but slightly more convenient).
  friend double sin(S1Angle a);
  friend double cos(S1Angle a);
  friend double tan(S1Angle a);

  struct SinCosPair {
    double sin;
    double cos;
  };
  // Return the sine and cosine of the angle. This may be more efficient than
  // calling sin() and cos() separately.
  SinCosPair SinCos() const;

  // Return the angle normalized to the range (-180, 180] degrees.
  S1Angle Normalized() const;

  // Normalize this angle to the range (-180, 180] degrees.
  void Normalize();

  // Class for encoding and decoding the object when used in Flume pipelines.
  class Coder : public s2coding::S2Coder<S1Angle> {
   public:
    void Encode(Encoder& encoder, const S1Angle& angle) const override;
    bool Decode(Decoder& decoder, S1Angle& angle,
                S2Error& error) const override;
  };

  // When S1Angle is used as a key in one of the absl::btree container types,
  // indicate that linear rather than binary search should be used.  This is
  // much faster when the comparison function is cheap.
  typedef std::true_type absl_btree_prefer_linear_node_search;

 private:
  explicit constexpr S1Angle(double radians) : radians_(radians) {}
  double radians_ = 0.0;
};


//////////////////   Implementation details follow   ////////////////////


inline constexpr S1Angle S1Angle::Infinity() {
  return S1Angle(std::numeric_limits<double>::infinity());
}

inline constexpr S1Angle S1Angle::Zero() {
  return S1Angle(0);
}

inline constexpr double S1Angle::radians() const {
  return radians_;
}

inline constexpr double S1Angle::degrees() const {
  return (180 / M_PI) * radians_;
}

// Note that the E5, E6, and E7 conversion involve two multiplications rather
// than one.  This is mainly for backwards compatibility (changing this would
// break many tests), but it does have the nice side effect that conversions
// between Degrees, E6, and E7 are exact when the arguments are integers.

inline int32_t S1Angle::e5() const {
  // TODO(user,b/298298095): Tighten this to [-180, 180].
  ABSL_DCHECK_LE(std::numeric_limits<int32_t>::min() / 1e5, degrees());
  ABSL_DCHECK_LE(degrees(), std::numeric_limits<int32_t>::max() / 1e5);
  return MathUtil::Round<int32_t>(1e5 * degrees());
}

inline int32_t S1Angle::e6() const {
  ABSL_DCHECK_LE(std::numeric_limits<int32_t>::min() / 1e6, degrees());
  ABSL_DCHECK_LE(degrees(), std::numeric_limits<int32_t>::max() / 1e6);
  return MathUtil::Round<int32_t>(1e6 * degrees());
}

inline int32_t S1Angle::e7() const {
  ABSL_DCHECK_LE(std::numeric_limits<int32_t>::min() / 1e7, degrees());
  ABSL_DCHECK_LE(degrees(), std::numeric_limits<int32_t>::max() / 1e7);
  return MathUtil::Round<int32_t>(1e7 * degrees());
}

inline S1Angle S1Angle::abs() const {
  return S1Angle(std::fabs(radians_));
}

inline S1Angle abs(S1Angle a) {
  return S1Angle(std::fabs(a.radians_));
}

inline constexpr bool operator==(S1Angle x, S1Angle y) {
  return x.radians() == y.radians();
}

inline constexpr bool operator!=(S1Angle x, S1Angle y) {
  return x.radians() != y.radians();
}

inline constexpr bool operator<(S1Angle x, S1Angle y) {
  return x.radians() < y.radians();
}

inline constexpr bool operator>(S1Angle x, S1Angle y) {
  return x.radians() > y.radians();
}

inline constexpr bool operator<=(S1Angle x, S1Angle y) {
  return x.radians() <= y.radians();
}

inline constexpr bool operator>=(S1Angle x, S1Angle y) {
  return x.radians() >= y.radians();
}

inline constexpr S1Angle operator-(S1Angle a) {
  return S1Angle::Radians(-a.radians());
}

inline constexpr S1Angle operator+(S1Angle a, S1Angle b) {
  return S1Angle::Radians(a.radians() + b.radians());
}

inline constexpr S1Angle operator-(S1Angle a, S1Angle b) {
  return S1Angle::Radians(a.radians() - b.radians());
}

inline constexpr S1Angle operator*(double m, S1Angle a) {
  return S1Angle::Radians(m * a.radians());
}

inline constexpr S1Angle operator*(S1Angle a, double m) {
  return S1Angle::Radians(m * a.radians());
}

inline constexpr S1Angle operator/(S1Angle a, double m) {
  return S1Angle::Radians(a.radians() / m);
}

inline constexpr double operator/(S1Angle a, S1Angle b) {
  return a.radians() / b.radians();
}

inline S1Angle& S1Angle::operator+=(S1Angle a) {
  radians_ += a.radians();
  return *this;
}

inline S1Angle& S1Angle::operator-=(S1Angle a) {
  radians_ -= a.radians();
  return *this;
}

inline S1Angle& S1Angle::operator*=(double m) {
  radians_ *= m;
  return *this;
}

inline S1Angle& S1Angle::operator/=(double m) {
  radians_ /= m;
  return *this;
}

inline double sin(S1Angle a) {
  return sin(a.radians());
}

inline double cos(S1Angle a) {
  return cos(a.radians());
}

inline double tan(S1Angle a) {
  return tan(a.radians());
}

inline S1Angle::SinCosPair S1Angle::SinCos() const {
// TODO(b/370513151): Remove once Clang can optimize this.
// NB: __sincos() provided by __APPLE__ is not bit-identical to sin(), cos()
// under `--config=darwin_arm64`.  Windows does not support sincos().
// `sincos` gives different results on Ubuntu 22.  Disable for glibc until
// this can be narrowed down further.
// https://github.com/google/s2geometry/issues/413
#if defined(__ANDROID__)
  double sin_angle, cos_angle;
  sincos(radians(), &sin_angle, &cos_angle);
  return {sin_angle, cos_angle};
#else
  return {sin(radians()), cos(radians())};
#endif
}

inline constexpr S1Angle S1Angle::Radians(double radians) {
  return S1Angle(radians);
}

inline constexpr S1Angle S1Angle::Degrees(double degrees) {
  return S1Angle((M_PI / 180) * degrees);
}

inline constexpr S1Angle S1Angle::E5(int32_t e5) {
  return Degrees(1e-5 * e5);
}

inline constexpr S1Angle S1Angle::E6(int32_t e6) {
  return Degrees(1e-6 * e6);
}

inline constexpr S1Angle S1Angle::E7(int32_t e7) {
  return Degrees(1e-7 * e7);
}

inline constexpr S1Angle S1Angle::UnsignedE6(uint32_t e6) {
  return E6(static_cast<int32_t>(e6));
}

inline constexpr S1Angle S1Angle::UnsignedE7(uint32_t e7) {
  return E7(static_cast<int32_t>(e7));
}

// Writes the angle in degrees with 7 digits of precision after the
// decimal point, e.g. "17.3745904".
std::ostream& operator<<(std::ostream& os, S1Angle a);

#endif  // S2_S1ANGLE_H_
