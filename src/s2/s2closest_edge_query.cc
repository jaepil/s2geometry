// Copyright 2013 Google Inc. All Rights Reserved.
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

#include "s2/s2closest_edge_query.h"

#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
#include "s2/s2closest_edge_query_base.h"
#include "s2/s2edge_distances.h"

void S2ClosestEdgeQuery::Options::set_conservative_max_distance(
    S1ChordAngle max_distance) {
  set_max_distance(Distance(max_distance.PlusError(
      S2::GetUpdateMinDistanceMaxError(max_distance)).Successor()));
}

void S2ClosestEdgeQuery::Options::set_conservative_max_distance(
    S1Angle max_distance) {
  set_conservative_max_distance(S1ChordAngle(max_distance));
}

int S2ClosestEdgeQuery::PointTarget::max_brute_force_index_size() const {
  // Using BM_FindClosest (which finds the single closest edge), the
  // break-even points are approximately 80, 100, and 250 edges for point
  // cloud, fractal, and regular loop geometry respectively.
  return 120;
}

int S2ClosestEdgeQuery::EdgeTarget::max_brute_force_index_size() const {
  // Using BM_FindClosestToEdge (which finds the single closest edge), the
  // break-even points are approximately 40, 50, and 100 edges for point
  // cloud, fractal, and regular loop geometry respectively.
  return 60;
}

int S2ClosestEdgeQuery::CellTarget::max_brute_force_index_size() const {
  // Using BM_FindClosestToCell (which finds the single closest edge), the
  // break-even points are approximately 20, 25, and 40 edges for point cloud,
  // fractal, and regular loop geometry respectively.
  return 30;
}

int S2ClosestEdgeQuery::ShapeIndexTarget::max_brute_force_index_size() const {
  // For BM_FindClosestToSameSizeAbuttingIndex (which uses two nearby indexes
  // with similar edge counts), the break-even points are approximately 20,
  // 30, and 40 edges for point cloud, fractal, and regular loop geometry
  // respectively.
  return 25;
}

S2ClosestEdgeQuery::S2ClosestEdgeQuery() {
  // Prevent inline constructor bloat by defining here.
}

S2ClosestEdgeQuery::~S2ClosestEdgeQuery() {
  // Prevent inline destructor bloat by defining here.
}

bool S2ClosestEdgeQuery::IsDistanceLess(Target* target, S1ChordAngle limit,
                                        ShapeFilter filter) {
  static_assert(sizeof(Options) <= 32, "Consider not copying Options here");
  Options tmp_options = options_;
  tmp_options.set_max_results(1);
  tmp_options.set_max_distance(limit);
  tmp_options.set_max_error(S1ChordAngle::Straight());
  return !base_.FindClosestEdge(target, tmp_options, filter).is_empty();
}

bool S2ClosestEdgeQuery::IsDistanceLessOrEqual(Target* target,
                                               S1ChordAngle limit,
                                               ShapeFilter filter) {
  static_assert(sizeof(Options) <= 32, "Consider not copying Options here");
  Options tmp_options = options_;
  tmp_options.set_max_results(1);
  tmp_options.set_inclusive_max_distance(limit);
  tmp_options.set_max_error(S1ChordAngle::Straight());
  return !base_.FindClosestEdge(target, tmp_options, filter).is_empty();
}

bool S2ClosestEdgeQuery::IsConservativeDistanceLessOrEqual(Target* target,
                                                           S1ChordAngle limit,
                                                           ShapeFilter filter) {
  static_assert(sizeof(Options) <= 32, "Consider not copying Options here");
  Options tmp_options = options_;
  tmp_options.set_max_results(1);
  tmp_options.set_conservative_max_distance(limit);
  tmp_options.set_max_error(S1ChordAngle::Straight());
  return !base_.FindClosestEdge(target, tmp_options, filter).is_empty();
}

void S2ClosestEdgeQuery::VisitClosestShapes(Target* target, Options options,
                      ResultVisitor visitor, const ShapeFilter filter) {
  // Often we'll see the same shape's edges consecutively, caching the last
  // accepted shape id will prevent us from having to hit the hash set most of
  // the time.
  int last_shape = -1;
  absl::flat_hash_set<int> seen_shapes;
  VisitClosestEdges(target, options,
      [&](const Result& result) {
        const int shape_id = result.shape_id();
        if (shape_id != last_shape && seen_shapes.insert(shape_id).second) {
          last_shape = shape_id;
          return visitor(result);
        }
        return true;
      },
      filter);
}
