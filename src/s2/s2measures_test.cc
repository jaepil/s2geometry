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

#include "s2/s2measures.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>
#include "absl/log/absl_log.h"
#include "absl/log/log_streamer.h"
#include "absl/random/random.h"
#include "s2/s2latlng.h"
#include "s2/s2point.h"
#include "s2/s2random.h"
#include "s2/s2testing.h"

using std::fabs;

TEST(S2, AngleMethods) {
  S2Point pz(0, 0, 1);
  S2Point p000(1, 0, 0);
  S2Point p045 = S2Point(1, 1, 0).Normalize();
  S2Point p180(-1, 0, 0);

  EXPECT_DOUBLE_EQ(S2::Angle(p000, pz, p045), M_PI_4);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p000, pz, p045), -3 * M_PI_4);

  EXPECT_DOUBLE_EQ(S2::Angle(p045, pz, p180), 3 * M_PI_4);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p045, pz, p180), -M_PI_4);

  EXPECT_DOUBLE_EQ(S2::Angle(p000, pz, p180), M_PI);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(p000, pz, p180), 0);

  EXPECT_DOUBLE_EQ(S2::Angle(pz, p000, p045), M_PI_2);
  EXPECT_DOUBLE_EQ(S2::TurnAngle(pz, p000, p045), M_PI_2);

  EXPECT_DOUBLE_EQ(S2::Angle(pz, p000, pz), 0);
  EXPECT_DOUBLE_EQ(fabs(S2::TurnAngle(pz, p000, pz)), M_PI);
}

TEST(S2, AreaMethods) {
  S2Point pz(0, 0, 1);
  S2Point p000(1, 0, 0);
  S2Point p045 = S2Point(1, 1, 0).Normalize();
  S2Point p090(0, 1, 0);
  S2Point p180(-1, 0, 0);

  EXPECT_DOUBLE_EQ(S2::Area(p000, p090, pz), M_PI_2);
  EXPECT_DOUBLE_EQ(S2::Area(p045, pz, p180), 3 * M_PI_4);

  // Make sure that Area() has good *relative* accuracy even for
  // very small areas.
  static const double eps = 1e-10;
  S2Point pepsx = S2Point(eps, 0, 1).Normalize();
  S2Point pepsy = S2Point(0, eps, 1).Normalize();
  double expected1 = 0.5 * eps * eps;
  EXPECT_NEAR(S2::Area(pepsx, pepsy, pz), expected1, 1e-14 * expected1);

  // Make sure that it can handle degenerate triangles.
  S2Point pr = S2Point(0.257, -0.5723, 0.112).Normalize();
  S2Point pq = S2Point(-0.747, 0.401, 0.2235).Normalize();
  EXPECT_EQ(S2::Area(pr, pr, pr), 0);
  // The following test is not exact due to rounding error.
  EXPECT_NEAR(S2::Area(pr, pq, pr), 0, 1e-15);
  EXPECT_EQ(S2::Area(p000, p045, p090), 0);

  // TODO(user,b/338315414): Split up this test case.
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "AREA_METHODS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));

  double max_girard = 0;
  for (int i = 0; i < 10000; ++i) {
    S2Point p0 = s2random::Point(bitgen);
    S2Point d1 = s2random::Point(bitgen);
    S2Point d2 = s2random::Point(bitgen);
    S2Point p1 = (p0 + 1e-15 * d1).Normalize();
    S2Point p2 = (p0 + 1e-15 * d2).Normalize();
    // The actual displacement can be as much as 1.2e-15 due to roundoff.
    // This yields a maximum triangle area of about 0.7e-30.
    EXPECT_LE(S2::Area(p0, p1, p2), 0.7e-30);
    max_girard = std::max(max_girard, S2::GirardArea(p0, p1, p2));
  }
  // This check only passes if GirardArea() uses RobustCrossProd().
  ABSL_LOG(INFO) << "Worst case Girard for triangle area 1e-30: " << max_girard;
  EXPECT_LE(max_girard, 1e-14);

  // Try a very long and skinny triangle.
  S2Point p045eps = S2Point(1, 1, eps).Normalize();
  double expected2 = 5.8578643762690495119753e-11;  // Mathematica.
  EXPECT_NEAR(S2::Area(p000, p045eps, p090), expected2, 1e-9 * expected2);

  // Triangles with near-180 degree edges that sum to a quarter-sphere.
  static const double eps2 = 1e-14;
  S2Point p000eps2 = S2Point(1, 0.1*eps2, eps2).Normalize();
  double quarter_area1 = S2::Area(p000eps2, p000, p045) +
                         S2::Area(p000eps2, p045, p180) +
                         S2::Area(p000eps2, p180, pz) +
                         S2::Area(p000eps2, pz, p000);
  EXPECT_DOUBLE_EQ(quarter_area1, M_PI);

  // Four other triangles that sum to a quarter-sphere.
  S2Point p045eps2 = S2Point(1, 1, eps2).Normalize();
  double quarter_area2 = S2::Area(p045eps2, p000, p045) +
                         S2::Area(p045eps2, p045, p180) +
                         S2::Area(p045eps2, p180, pz) +
                         S2::Area(p045eps2, pz, p000);
  EXPECT_DOUBLE_EQ(quarter_area2, M_PI);

  // Compute the area of a hemisphere using four triangles with one near-180
  // degree edge and one near-degenerate edge.
  for (int i = 0; i < 100; ++i) {
    double lng = absl::Uniform(bitgen, 0.0, 2 * M_PI);
    S2Point p0 = S2LatLng::FromRadians(1e-20, lng).Normalized().ToPoint();
    S2Point p1 = S2LatLng::FromRadians(0, lng).Normalized().ToPoint();
    double p2_lng = lng + absl::Uniform(bitgen, 0.0, 1.0);
    S2Point p2 = S2LatLng::FromRadians(0, p2_lng).Normalized().ToPoint();
    S2Point p3 = S2LatLng::FromRadians(0, lng + M_PI).Normalized().ToPoint();
    S2Point p4 = S2LatLng::FromRadians(0, lng + 5.0).Normalized().ToPoint();
    double area = (S2::Area(p0, p1, p2) + S2::Area(p0, p2, p3) +
                   S2::Area(p0, p3, p4) + S2::Area(p0, p4, p1));
    EXPECT_NEAR(area, 2 * M_PI, 2e-15);
  }

  // This tests a case where the triangle has zero area, but S2::Area()
  // computes (dmin > 0) due to rounding errors.
  EXPECT_EQ(0.0, S2::Area(S2LatLng::FromDegrees(-45, -170).ToPoint(),
                          S2LatLng::FromDegrees(45, -170).ToPoint(),
                          S2LatLng::FromDegrees(0, -170).ToPoint()));
}

// Previously these three points shows catastrophic error in their cross product
// which prevented Area() from falling back to the Girard method properly. They
// returned an area on the order of 1e-14 and the real area is ~1e-21, 7 orders
// of magnitude relative error. Check that they return zero now.
TEST(S2, GetAreaRegression_B229644268) {
  const S2Point a(-1.705424004316021258e-01, -8.242696197922716461e-01,
                  5.399026611737816062e-01);
  const S2Point b(-1.706078905422188652e-01, -8.246067119418969416e-01,
                  5.393669607095969987e-01);
  const S2Point c(-1.705800600596222294e-01, -8.244634596153025408e-01,
                  5.395947061167500891e-01);
  EXPECT_EQ(S2::Area(a, b, c), 0);
}
