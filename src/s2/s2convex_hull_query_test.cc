// Copyright 2015 Google Inc. All Rights Reserved.
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

#include "s2/s2convex_hull_query.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "absl/log/absl_check.h"
#include "absl/log/log_streamer.h"
#include "absl/random/random.h"
#include "s2/s1angle.h"
#include "s2/s2cap.h"
#include "s2/s2cell_id.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"
#include "s2/s2polyline.h"
#include "s2/s2random.h"
#include "s2/s2testing.h"
#include "s2/s2text_format.h"

using s2textformat::MakePointOrDie;
using std::fabs;
using std::unique_ptr;
using std::vector;

namespace {

TEST(S2ConvexHullQuery, NoPoints) {
  S2ConvexHullQuery query;
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_TRUE(result->is_empty());
}

static bool LoopHasVertex(const S2Loop& loop, const S2Point& p) {
  for (int i = 0; i < loop.num_vertices(); ++i) {
    if (loop.vertex(i) == p) return true;
  }
  return false;
}

TEST(S2ConvexHullQuery, OnePoint) {
  S2ConvexHullQuery query;
  S2Point p(0, 0, 1);
  query.AddPoint(p);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_EQ(3, result->num_vertices());
  EXPECT_TRUE(result->IsNormalized());
  EXPECT_TRUE(LoopHasVertex(*result, p));
  // Add some duplicate points and check that the result is the same.
  query.AddPoint(p);
  query.AddPoint(p);
  unique_ptr<S2Loop> result2(query.GetConvexHull());
  EXPECT_TRUE(result2->Equals(*result.get()));
}

TEST(S2ConvexHullQuery, TwoPoints) {
  S2ConvexHullQuery query;
  S2Point p(0, 0, 1);
  S2Point q(0, 1, 0);
  query.AddPoint(p);
  query.AddPoint(q);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_EQ(3, result->num_vertices());
  EXPECT_TRUE(result->IsNormalized());
  EXPECT_TRUE(LoopHasVertex(*result, p));
  EXPECT_TRUE(LoopHasVertex(*result, q));
  // Add some duplicate points and check that the result is the same.
  query.AddPoint(q);
  query.AddPoint(p);
  query.AddPoint(p);
  unique_ptr<S2Loop> result2(query.GetConvexHull());
  EXPECT_TRUE(result2->Equals(*result.get()));
}

TEST(S2ConvexHullQuery, TwoAntipodalPoints) {
  S2ConvexHullQuery query;
  query.AddPoint(S2Point(0, 0, 1));
  query.AddPoint(S2Point(0, 0, -1));
  unique_ptr<S2Loop> result = query.GetConvexHull();
  EXPECT_TRUE(result->is_full());
}

TEST(S2ConvexHullQuery, EmptyLoop) {
  S2ConvexHullQuery query;
  S2Loop empty(S2Loop::kEmpty());
  query.AddLoop(empty);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_TRUE(result->is_empty());
}

TEST(S2ConvexHullQuery, FullLoop) {
  S2ConvexHullQuery query;
  S2Loop full(S2Loop::kFull());
  query.AddLoop(full);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_TRUE(result->is_full());
}

TEST(S2ConvexHullQuery, EmptyPolygon) {
  S2ConvexHullQuery query;
  vector<unique_ptr<S2Loop>> loops;
  S2Polygon empty(std::move(loops));
  query.AddPolygon(empty);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_TRUE(result->is_empty());
}

TEST(S2ConvexHullQuery, NonConvexPoints) {
  // Generate a point set such that the only convex region containing them is
  // the entire sphere.  In other words, you can generate any point on the
  // sphere by repeatedly linearly interpolating between the points.  (The
  // four points of a tetrahedron would also work, but this is easier.)
  S2ConvexHullQuery query;
  for (int face = 0; face < 6; ++face) {
    query.AddPoint(S2CellId::FromFace(face).ToPoint());
  }
  unique_ptr<S2Loop> result(query.GetConvexHull());
  EXPECT_TRUE(result->is_full());
}

TEST(S2ConvexHullQuery, SimplePolyline) {
  // A polyline is handling identically to a point set, so there is no need
  // for special testing other than code coverage.
  unique_ptr<S2Polyline> polyline(s2textformat::MakePolylineOrDie(
      "0:1, 0:9, 1:6, 2:6, 3:10, 4:10, 5:5, 4:0, 3:0, 2:5, 1:5"));
  S2ConvexHullQuery query;
  query.AddPolyline(*polyline);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  unique_ptr<S2Loop> expected_result(
      s2textformat::MakeLoopOrDie("0:1, 0:9, 3:10, 4:10, 5:5, 4:0, 3:0"));
  EXPECT_TRUE(result->BoundaryEquals(*expected_result.get()));
}

TEST(S2ConvexHullQuery, CapBoundExpandedToHemisphere) {
  // The following 3 points yield an S2Cap bound that is slightly smaller than
  // a hemisphere.  Here we test that the cap is expanded using a conservative
  // error bound to yield a hemisphere, which causes the convex hull algorithm
  // to return the full sphere.
  S2ConvexHullQuery query;
  query.AddPoint(MakePointOrDie("0:0"));
  query.AddPoint(MakePointOrDie("0:45"));
  query.AddPoint(MakePointOrDie("0:-135"));
  unique_ptr<S2Loop> result = query.GetConvexHull();
  EXPECT_TRUE(result->is_full());
}

void TestNorthPoleLoop(S1Angle radius, int num_vertices) {
  // If the radius is very close to 90, then it's hard to predict whether the
  // result will be the full loop or not.
  ABSL_DCHECK_GE(fabs(radius.radians() - M_PI_2), 1e-15);

  S2ConvexHullQuery query;
  unique_ptr<S2Loop> loop(
      S2Loop::MakeRegularLoop(S2Point(0, 0, 1), radius, num_vertices));
  query.AddLoop(*loop);
  unique_ptr<S2Loop> result(query.GetConvexHull());
  if (radius > S1Angle::Radians(M_PI_2)) {
    EXPECT_TRUE(result->is_full());
  } else {
    EXPECT_TRUE(result->BoundaryEquals(*loop.get()));
  }
}

TEST(S2ConvexHullQuery, LoopsAroundNorthPole) {
  // Test loops of various sizes around the north pole.
  TestNorthPoleLoop(S1Angle::Degrees(1), 3);
  TestNorthPoleLoop(S1Angle::Degrees(89), 3);

  // The following two loops should yield the full loop.
  TestNorthPoleLoop(S1Angle::Degrees(91), 3);
  TestNorthPoleLoop(S1Angle::Degrees(179), 3);

  TestNorthPoleLoop(S1Angle::Degrees(10), 100);
  TestNorthPoleLoop(S1Angle::Degrees(89), 1000);
}

TEST(S2ConvexHullQuery, PointsInsideHull) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "POINTS_INSIDE_HULL", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));

  // Repeatedly build the convex hull of a set of points, then add more points
  // inside that loop and build the convex hull again.  The result should
  // always be the same.
  constexpr int kIters = 1000;
  for (int iter = 0; iter < kIters; ++iter) {

    // Choose points from within a cap of random size, up to but not including
    // an entire hemisphere.
    S2Cap cap = s2random::Cap(bitgen, 1e-15, 1.999 * M_PI);
    S2ConvexHullQuery query;
    int num_points1 = absl::Uniform(bitgen, 3, 103);
    for (int i = 0; i < num_points1; ++i) {
      query.AddPoint(s2random::SamplePoint(bitgen, cap));
    }
    unique_ptr<S2Loop> hull(query.GetConvexHull());

    // When the convex hull is nearly a hemisphere, the algorithm sometimes
    // returns a full cap instead.  This is because it first computes a
    // bounding rectangle for all the input points/edges and then converts it
    // to a bounding cap, which sometimes yields a non-convex cap (radius
    // larger than 90 degrees).  This should not be a problem in practice
    // (since most convex hulls are not hemispheres), but in order make this
    // test pass reliably it means that we need to reject convex hulls whose
    // bounding cap (when computed from a bounding rectangle) is not convex.
    //
    // TODO(b/203702905): This test can still fail (about 1 iteration in
    // 500,000) because the S2LatLngRect::GetCapBound implementation does not
    // guarantee that A.Contains(B) implies
    // A.GetCapBound().Contains(B.GetCapBound()).
    if (hull->GetCapBound().height() >= 1) continue;

    // Otherwise, add more points inside the convex hull.
    const int num_points2 = 1000;
    for (int i = 0; i < num_points2; ++i) {
      S2Point p = s2random::SamplePoint(bitgen, cap);
      if (hull->Contains(p)) {
        query.AddPoint(p);
      }
    }
    // Finally, build a new convex hull and check that it hasn't changed.
    unique_ptr<S2Loop> hull2(query.GetConvexHull());
    EXPECT_TRUE(hull2->BoundaryEquals(*hull.get())) << "Iteration: " << iter;
  }
}

}  // namespace
