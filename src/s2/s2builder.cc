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
//
// The algorithm is based on the idea of choosing a set of sites and computing
// their "limited radius Voronoi diagram", which is obtained by intersecting
// each Voronoi region with a disc of fixed radius (the "snap radius")
// centered around the corresponding site.
//
// For each input edge, we then determine the sequence of Voronoi regions
// crossed by that edge, and snap the edge to the corresponding sequence of
// sites.  (In other words, each input edge is replaced by an edge chain.)
//
// The sites are chosen by starting with the set of input vertices, optionally
// snapping them to discrete point set (such as S2CellId centers or lat/lng E7
// coordinates), and then choosing a subset such that no two sites are closer
// than the given "snap_radius".  Note that the sites do not need to be spaced
// regularly -- their positions are completely arbitrary.
//
// Rather than computing the full limited radius Voronoi diagram, instead we
// compute on demand the sequence of Voronoi regions intersected by each edge.
// We do this by first finding all the sites that are within "snap_radius" of
// the edge, sorting them by distance from the edge origin, and then using an
// incremental algorithm.
//
// We implement the minimum edge-vertex separation property by snapping all
// the input edges and checking whether any site (the "site to avoid") would
// then be too close to the edge.  If so we add another site (the "separation
// site") along the input edge, positioned so that the new snapped edge is
// guaranteed to be far enough away from the site to avoid.  We then find all
// the input edges that are within "snap_radius" of the new site, and resnap
// those edges.  (It is very rare that we need to add a separation site, even
// when sites are densely spaced.)
//
// Idempotency is implemented by explicitly checking whether the input
// geometry already meets the output criteria.  This is not as sophisticated
// as Stable Snap Rounding (Hershberger); I have worked out the details and it
// is possible to adapt those ideas here, but it would make the implementation
// significantly more complex.
//
// The only way that different output layers interact is in the choice of
// Voronoi sites:
//
//  - Vertices from all layers contribute to the initial selection of sites.
//
//  - Edges in any layer that pass too close to a site can cause new sites to
//    be added (which affects snapping in all layers).
//
//  - Simplification can be thought of as removing sites.  A site can be
//    removed only if the snapped edges stay within the error bounds of the
//    corresponding input edges in all layers.
//
// Otherwise all layers are processed independently.  For example, sibling
// edge pairs can only cancel each other within a single layer (if desired).

#include "s2/s2builder.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "s2/base/casts.h"
#include "s2/base/log_severity.h"
#include "s2/id_set_lexicon.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
#include "s2/s2builder_graph.h"
#include "s2/s2builder_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2cell_id.h"
#include "s2/s2closest_edge_query.h"
#include "s2/s2closest_edge_query_base.h"
#include "s2/s2closest_point_query.h"
#include "s2/s2closest_point_query_base.h"
#include "s2/s2crossing_edge_query.h"
#include "s2/s2edge_crossings.h"
#include "s2/s2edge_distances.h"
#include "s2/s2error.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2point_index.h"
#include "s2/s2point_span.h"
#include "s2/s2polygon.h"
#include "s2/s2polyline.h"
#include "s2/s2polyline_simplifier.h"
#include "s2/s2predicates.h"
#include "s2/s2shape.h"
#include "s2/s2shapeutil_shape_edge.h"
#include "s2/s2shapeutil_visit_crossing_edge_pairs.h"
#include "s2/s2text_format.h"
#include "s2/util/bits/bits.h"
#include "s2/util/gtl/compact_array.h"

using absl::flat_hash_set;
using gtl::compact_array;
using std::make_unique;
using std::max;
using std::pair;
using std::unique_ptr;
using std::vector;

S2Builder::Options::Options()
    : snap_function_(
          make_unique<s2builderutil::IdentitySnapFunction>(S1Angle::Zero())) {
}

S2Builder::Options::Options(const SnapFunction& snap_function)
    : snap_function_(snap_function.Clone()) {
}

S2Builder::Options::Options(const Options& options)
    : snap_function_(options.snap_function_->Clone()),
      split_crossing_edges_(options.split_crossing_edges_),
      intersection_tolerance_(options.intersection_tolerance_),
      simplify_edge_chains_(options.simplify_edge_chains_),
      idempotent_(options.idempotent_),
      memory_tracker_(options.memory_tracker_) {
}

S2Builder::Options& S2Builder::Options::operator=(const Options& options) {
  snap_function_ = options.snap_function_->Clone();
  split_crossing_edges_ = options.split_crossing_edges_;
  intersection_tolerance_ = options.intersection_tolerance_;
  simplify_edge_chains_ = options.simplify_edge_chains_;
  idempotent_ = options.idempotent_;
  memory_tracker_ = options.memory_tracker_;
  return *this;
}

S1Angle S2Builder::Options::edge_snap_radius() const {
  return snap_function().snap_radius() + intersection_tolerance();
}

S1Angle S2Builder::Options::max_edge_deviation() const {
  // We want max_edge_deviation() to be large enough compared to snap_radius()
  // such that edge splitting is rare.
  //
  // Using spherical trigonometry, if the endpoints of an edge of length L
  // move by at most a distance R, the center of the edge moves by at most
  // asin(sin(R) / cos(L / 2)).  Thus the (max_edge_deviation / snap_radius)
  // ratio increases with both the snap radius R and the edge length L.
  //
  // We arbitrarily limit the edge deviation to be at most 10% more than the
  // snap radius.  With the maximum allowed snap radius of 70 degrees, this
  // means that edges up to 30.6 degrees long are never split.  For smaller
  // snap radii, edges up to 49 degrees long are never split.  (Edges of any
  // length are not split unless their endpoints move far enough so that the
  // actual edge deviation exceeds the limit; in practice, splitting is rare
  // even with long edges.)  Note that it is always possible to split edges
  // when max_edge_deviation() is exceeded; see MaybeAddExtraSites().
  ABSL_DCHECK_LE(snap_function().snap_radius(), SnapFunction::kMaxSnapRadius());
  const double kMaxEdgeDeviationRatio = 1.1;
  return kMaxEdgeDeviationRatio * edge_snap_radius();
}

bool operator==(const S2Builder::GraphOptions& x,
                const S2Builder::GraphOptions& y) {
  return (x.edge_type() == y.edge_type() &&
          x.degenerate_edges() == y.degenerate_edges() &&
          x.duplicate_edges() == y.duplicate_edges() &&
          x.sibling_pairs() == y.sibling_pairs() &&
          x.allow_vertex_filtering() == y.allow_vertex_filtering());
}

// Helper functions for computing error bounds:

static S1ChordAngle RoundUp(S1Angle a) {
  S1ChordAngle ca(a);
  return ca.PlusError(ca.GetS1AngleConstructorMaxError());
}

static S1ChordAngle AddPointToPointError(S1ChordAngle ca) {
  return ca.PlusError(ca.GetS2PointConstructorMaxError());
}

static S1ChordAngle AddPointToEdgeError(S1ChordAngle ca) {
  return ca.PlusError(S2::GetUpdateMinDistanceMaxError(ca));
}

S2Builder::S2Builder() = default;

S2Builder::S2Builder(const Options& options) {
  Init(options);
}

void S2Builder::Init(const Options& options) {
  options_ = options;
  const SnapFunction& snap_function = options.snap_function();
  S1Angle snap_radius = snap_function.snap_radius();
  ABSL_DCHECK_LE(snap_radius, SnapFunction::kMaxSnapRadius());

  // Convert the snap radius to an S1ChordAngle.  This is the "true snap
  // radius" used when evaluating exact predicates (s2predicates.h).
  site_snap_radius_ca_ = S1ChordAngle(snap_radius);

  // When intersection_tolerance() is non-zero we need to use a larger snap
  // radius for edges than for vertices to ensure that both edges are snapped
  // to the edge intersection location.  This is because the computed
  // intersection point is not exact; it may be up to intersection_tolerance()
  // away from its true position.  The computed intersection point might then
  // be snapped to some other vertex up to snap_radius away.  So to ensure
  // that both edges are snapped to a common vertex, we need to increase the
  // snap radius for edges to at least the sum of these two values (calculated
  // conservatively).
  S1Angle edge_snap_radius = options.edge_snap_radius();
  edge_snap_radius_ca_ = RoundUp(edge_snap_radius);
  snapping_requested_ = (edge_snap_radius > S1Angle::Zero());

  // Compute the maximum distance that a vertex can be separated from an
  // edge while still affecting how that edge is snapped.
  max_edge_deviation_ = options.max_edge_deviation();
  edge_site_query_radius_ca_ = S1ChordAngle(
      max_edge_deviation_ + snap_function.min_edge_vertex_separation());

  // Compute the maximum edge length such that even if both endpoints move by
  // the maximum distance allowed (i.e., edge_snap_radius), the center of the
  // edge will still move by less than max_edge_deviation().  This saves us a
  // lot of work since then we don't need to check the actual deviation.
  if (!snapping_requested_) {
    min_edge_length_to_split_ca_ = S1ChordAngle::Infinity();
  } else {
    // This value varies between 30 and 50 degrees depending on the snap radius.
    min_edge_length_to_split_ca_ = S1ChordAngle::Radians(
        2 * acos(sin(edge_snap_radius) / sin(max_edge_deviation_)));
  }

  // In rare cases we may need to explicitly check that the input topology is
  // preserved, i.e. that edges do not cross vertices when snapped.  This is
  // only necessary (1) for vertices added using ForceVertex(), and (2) when the
  // snap radius is smaller than intersection_tolerance() (which is typically
  // either zero or S2::kIntersectionError, about 9e-16 radians).  This
  // condition arises because when a geodesic edge is snapped, the edge center
  // can move further than its endpoints.  This can cause an edge to pass on the
  // wrong side of an input vertex.  (Note that this could not happen in a
  // planar version of this algorithm.)  Usually we don't need to consider this
  // possibility explicitly, because if the snapped edge passes on the wrong
  // side of a vertex then it is also closer than min_edge_vertex_separation()
  // to that vertex, which will cause a separation site to be added.
  //
  // If the condition below is true then we need to check all sites (i.e.,
  // snapped input vertices) for topology changes.  However this is almost never
  // the case because
  //
  //            max_edge_deviation() == 1.1 * edge_snap_radius()
  //      and   min_edge_vertex_separation() >= 0.219 * snap_radius()
  //
  // for all currently implemented snap functions.  The condition below is
  // only true when intersection_tolerance() is non-zero (which causes
  // edge_snap_radius() to exceed snap_radius() by S2::kIntersectionError) and
  // snap_radius() is very small (at most S2::kIntersectionError / 1.19).
  check_all_site_crossings_ = (options.max_edge_deviation() >
                               options.edge_snap_radius() +
                               snap_function.min_edge_vertex_separation());
  if (options.intersection_tolerance() <= S1Angle::Zero()) {
    ABSL_DCHECK(!check_all_site_crossings_);
  }

  // To implement idempotency, we check whether the input geometry could
  // possibly be the output of a previous S2Builder invocation.  This involves
  // testing whether any site/site or edge/site pairs are too close together.
  // This is done using exact predicates, which require converting the minimum
  // separation values to an S1ChordAngle.
  min_site_separation_ = snap_function.min_vertex_separation();
  min_site_separation_ca_ = S1ChordAngle(min_site_separation_);
  min_edge_site_separation_ca_ =
      S1ChordAngle(snap_function.min_edge_vertex_separation());

  // This is an upper bound on the distance computed by S2ClosestPointQuery
  // where the true distance might be less than min_edge_site_separation_ca_.
  min_edge_site_separation_ca_limit_ =
      AddPointToEdgeError(min_edge_site_separation_ca_);

  // Compute the maximum possible distance between two sites whose Voronoi
  // regions touch.  (The maximum radius of each Voronoi region is
  // edge_snap_radius_.)  Then increase this bound to account for errors.
  max_adjacent_site_separation_ca_ =
      AddPointToPointError(RoundUp(2 * edge_snap_radius));

  // Finally, we also precompute sin^2(edge_snap_radius), which is simply the
  // squared distance between a vertex and an edge measured perpendicular to
  // the plane containing the edge, and increase this value by the maximum
  // error in the calculation to compare this distance against the bound.
  double d = sin(edge_snap_radius);
  edge_snap_radius_sin2_ = d * d;
  edge_snap_radius_sin2_ += ((9.5 * d + 2.5 + 2 * sqrt(3)) * d +
                             9 * DBL_EPSILON) * DBL_EPSILON;

  // Initialize the current label set.
  label_set_id_ = label_set_lexicon_.EmptySetId();
  label_set_modified_ = false;

  // If snapping was requested, we try to determine whether the input geometry
  // already meets the output requirements.  This is necessary for
  // idempotency, and can also save work.  If we discover any reason that the
  // input geometry needs to be modified, snapping_needed_ is set to true.
  snapping_needed_ = false;

  tracker_.Init(options.memory_tracker());
}

void S2Builder::clear_labels() {
  label_set_.clear();
  label_set_modified_ = true;
}

void S2Builder::push_label(Label label) {
  ABSL_DCHECK_GE(label, 0);
  label_set_.push_back(label);
  label_set_modified_ = true;
}

void S2Builder::pop_label() {
  label_set_.pop_back();
  label_set_modified_ = true;
}

void S2Builder::set_label(Label label) {
  ABSL_DCHECK_GE(label, 0);
  label_set_.resize(1);
  label_set_[0] = label;
  label_set_modified_ = true;
}

bool S2Builder::IsFullPolygonUnspecified(const S2Builder::Graph& g,
                                         S2Error* error) {
  *error =
      S2Error(S2Error::BUILDER_IS_FULL_PREDICATE_NOT_SPECIFIED,
              "A degenerate polygon was found, but no predicate was specified "
              "to determine whether the polygon is empty or full.  Call "
              "S2Builder::AddIsFullPolygonPredicate() to fix this problem.");
  return false;  // Assumes the polygon is empty.
}

S2Builder::IsFullPolygonPredicate S2Builder::IsFullPolygon(bool is_full) {
  return [is_full](const S2Builder::Graph& g, S2Error* error) {
      return is_full;
    };
}

void S2Builder::StartLayer(unique_ptr<Layer> layer) {
  layer_options_.push_back(layer->graph_options());
  layer_begins_.push_back(input_edges_.size());
  layer_is_full_polygon_predicates_.push_back(IsFullPolygon(false));
  layers_.push_back(std::move(layer));
}

// Input vertices are stored in a vector, with some removal of duplicates.
// Edges are represented as (VertexId, VertexId) pairs.  All edges are stored
// in a single vector; each layer corresponds to a contiguous range.

S2Builder::InputVertexId S2Builder::AddVertex(const S2Point& v) {
  // Remove duplicate vertices that follow the pattern AB, BC, CD.  If we want
  // to do anything more sophisticated, either use a ValueLexicon, or sort the
  // vertices once they have all been added, remove duplicates, and update the
  // edges.
  if (input_vertices_.empty() || v != input_vertices_.back()) {
    if (!tracker_.AddSpace(&input_vertices_, 1)) return -1;
    input_vertices_.push_back(v);
  }
  return input_vertices_.size() - 1;
}

void S2Builder::AddIntersection(const S2Point& vertex) {
  // It is an error to call this method without first setting
  // intersection_tolerance() to a non-zero value.
  ABSL_DCHECK_GT(options_.intersection_tolerance(), S1Angle::Zero());

  // Calling this method also overrides the idempotent() option.
  snapping_needed_ = true;

  AddVertex(vertex);
}

void S2Builder::AddEdge(const S2Point& v0, const S2Point& v1) {
  ABSL_DCHECK(!layers_.empty()) << "Call StartLayer before adding any edges";
  if (v0 == v1 && (layer_options_.back().degenerate_edges() ==
                   GraphOptions::DegenerateEdges::DISCARD)) {
    return;
  }
  InputVertexId j0 = AddVertex(v0);
  InputVertexId j1 = AddVertex(v1);
  if (!tracker_.AddSpace(&input_edges_, 1)) return;
  input_edges_.push_back(InputEdge(j0, j1));

  // If there are any labels, then attach them to this input edge.
  if (label_set_modified_) {
    if (label_set_ids_.empty()) {
      // Populate the missing entries with empty label sets.
      label_set_ids_.assign(input_edges_.size() - 1, label_set_id_);
    }
    label_set_id_ = label_set_lexicon_.Add(label_set_);
    label_set_ids_.push_back(label_set_id_);
    label_set_modified_ = false;
  } else if (!label_set_ids_.empty()) {
    label_set_ids_.push_back(label_set_id_);
  }
}

void S2Builder::AddPolyline(S2PointSpan polyline) {
  for (size_t i = 1; i < polyline.size(); ++i) {
    AddEdge(polyline[i - 1], polyline[i]);
  }
}

void S2Builder::AddPolyline(const S2Polyline& polyline) {
  const int n = polyline.num_vertices();
  for (int i = 1; i < n; ++i) {
    AddEdge(polyline.vertex(i - 1), polyline.vertex(i));
  }
}

void S2Builder::AddLoop(S2PointLoopSpan loop) {
  for (size_t i = 0; i < loop.size(); ++i) {
    AddEdge(loop[i], loop[i + 1]);
  }
}

void S2Builder::AddLoop(const S2Loop& loop) {
  // Ignore loops with a single vertex, as they do not have a boundary.
  if (loop.is_empty_or_full()) return;

  // For loops that represent holes, we add the edge from vertex n-1 to vertex
  // n-2 first.  This is because these edges will be assembled into a
  // clockwise loop, which will eventually be normalized in S2Polygon by
  // calling S2Loop::Invert().  S2Loop::Invert() reverses the order of the
  // vertices, so to end up with the original vertex order (0, 1, ..., n-1) we
  // need to build a clockwise loop with vertex order (n-1, n-2, ..., 0).
  // This is done by adding the edge (n-1, n-2) first, and then ensuring that
  // Build() assembles loops starting from edges in the order they were added.
  const int n = loop.num_vertices();
  for (int i = 0; i < n; ++i) {
    AddEdge(loop.oriented_vertex(i), loop.oriented_vertex(i + 1));
  }
}

void S2Builder::AddPolygon(const S2Polygon& polygon) {
  for (int i = 0; i < polygon.num_loops(); ++i) {
    AddLoop(*polygon.loop(i));
  }
}

void S2Builder::AddShape(const S2Shape& shape) {
  for (int e = 0, n = shape.num_edges(); e < n; ++e) {
    S2Shape::Edge edge = shape.edge(e);
    AddEdge(edge.v0, edge.v1);
  }
}

void S2Builder::AddIsFullPolygonPredicate(IsFullPolygonPredicate predicate) {
  layer_is_full_polygon_predicates_.back() = std::move(predicate);
}

void S2Builder::ForceVertex(const S2Point& vertex) {
  if (!tracker_.AddSpace(&sites_, 1)) return;
  sites_.push_back(vertex);
}

// An S2Shape used to represent the entire collection of S2Builder input edges.
// Vertices are specified as indices into a vertex vector to save space.
namespace {
class VertexIdEdgeVectorShape final : public S2Shape {
 public:
  // Requires that "edges" is constant for the lifetime of this object.
  VertexIdEdgeVectorShape(const vector<pair<int32_t, int32_t>>& edges,
                          const vector<S2Point>& vertices)
      : edges_(edges), vertices_(vertices) {}

  const S2Point& vertex0(int e) const { return vertex(edges_[e].first); }
  const S2Point& vertex1(int e) const { return vertex(edges_[e].second); }

  // S2Shape interface:
  int num_edges() const override { return edges_.size(); }
  Edge edge(int e) const override {
    return Edge(vertices_[edges_[e].first], vertices_[edges_[e].second]);
  }
  int dimension() const override { return 1; }
  ReferencePoint GetReferencePoint() const override {
    return ReferencePoint::Contained(false);
  }
  int num_chains() const override { return edges_.size(); }
  Chain chain(int i) const override { return Chain(i, 1); }
  Edge chain_edge(int i, int j) const override { return edge(i); }
  ChainPosition chain_position(int e) const override {
    return ChainPosition(e, 0);
  }

 private:
  const S2Point& vertex(int i) const { return vertices_[i]; }

  const vector<std::pair<int32_t, int32_t>>& edges_;
  const vector<S2Point>& vertices_;
};
}  // namespace

bool S2Builder::Build(S2Error* error) {
  // ABSL_CHECK rather than ABSL_DCHECK because this is friendlier than crashing
  // on the "error->Clear()" call below.  It would be easy to allow (error ==
  // nullptr) by declaring a local "tmp_error", but it seems better to make
  // clients think about error handling.
  ABSL_CHECK(error != nullptr);
  error_ = error;
  *error_ = S2Error::Ok();

  // Mark the end of the last layer.
  layer_begins_.push_back(input_edges_.size());

  // See the algorithm overview at the top of this file.
  if (snapping_requested_ && !options_.idempotent()) {
    snapping_needed_ = true;
  }
  ChooseSites();
  BuildLayers();
  Reset();
  if (!tracker_.ok()) *error_ = tracker_.error();
  return error_->ok();
}

void S2Builder::Reset() {
  // Note that these calls do not change vector capacities.
  input_vertices_.clear();
  input_edges_.clear();
  layers_.clear();
  layer_options_.clear();
  layer_begins_.clear();
  layer_is_full_polygon_predicates_.clear();
  label_set_ids_.clear();
  label_set_lexicon_.Clear();
  label_set_.clear();
  label_set_modified_ = false;
  sites_.clear();
  edge_sites_.clear();
  snapping_needed_ = false;
}

void S2Builder::ChooseSites() {
  if (!tracker_.ok() || input_vertices_.empty()) return;

  // Note that although we always create an S2ShapeIndex, often it is not
  // actually built (because this happens lazily).  Therefore we only test
  // its memory usage at the places where it is used.
  MutableS2ShapeIndex input_edge_index;
  input_edge_index.set_memory_tracker(tracker_.tracker());
  input_edge_index.Add(make_unique<VertexIdEdgeVectorShape>(input_edges_,
                                                            input_vertices_));
  if (options_.split_crossing_edges()) {
    AddEdgeCrossings(input_edge_index);
  }

  if (snapping_requested_) {
    S2PointIndex<SiteId> site_index;
    auto _ = absl::MakeCleanup([&]() { tracker_.DoneSiteIndex(site_index); });
    AddForcedSites(&site_index);
    ChooseInitialSites(&site_index);
    if (!tracker_.FixSiteIndexTally(site_index)) return;
    CollectSiteEdges(site_index);
  }
  if (snapping_needed_) {
    AddExtraSites(input_edge_index);
  } else {
    ChooseAllVerticesAsSites();
  }
}

void S2Builder::ChooseAllVerticesAsSites() {
  // Sort the input vertices, discard duplicates, and use the result as the
  // list of sites.  (We sort in the same order used by ChooseInitialSites()
  // to avoid inconsistencies in tests.)  We also copy the result back to
  // input_vertices_ and update the input edges to use the new vertex
  // numbering (so that InputVertexId == SiteId).  This simplifies the
  // implementation of SnapEdge() for this case.
  sites_.clear();
  if (!tracker_.AddSpaceExact(&sites_, input_vertices_.size())) return;
  const int64_t kTempPerVertex = sizeof(InputVertexKey) + sizeof(InputVertexId);
  if (!tracker_.TallyTemp(input_vertices_.size() * kTempPerVertex)) return;
  vector<InputVertexKey> sorted = SortInputVertices();
  vector<InputVertexId> vmap(input_vertices_.size());
  for (size_t in = 0; in < sorted.size();) {
    const S2Point& site = input_vertices_[sorted[in].second];
    vmap[sorted[in].second] = sites_.size();
    while (++in < sorted.size() && input_vertices_[sorted[in].second] == site) {
      vmap[sorted[in].second] = sites_.size();
    }
    sites_.push_back(site);
  }
  input_vertices_ = sites_;  // Does not change allocated size.
  for (InputEdge& e : input_edges_) {
    e.first = vmap[e.first];
    e.second = vmap[e.second];
  }
}

vector<S2Builder::InputVertexKey> S2Builder::SortInputVertices() {
  // Sort all the input vertices in the order that we wish to consider them as
  // candidate Voronoi sites.  ChooseAllVerticesAsSites() requires duplicate
  // points be adjacent in the sorted output for deduplication.  Otherwise, any
  // sort order will produce correct output, so we have complete flexibility in
  // choosing the sort key.  We could even leave them unsorted, although this
  // would have the disadvantage that changing the order of the input edges
  // could cause S2Builder to snap to a different set of Voronoi sites.
  //
  // We have chosen to sort them primarily by S2CellId since this improves the
  // performance of many S2Builder phases (due to better spatial locality).  It
  // also allows the possibility of replacing the current S2PointIndex approach
  // with a more efficient recursive divide-and-conquer algorithm.  Ties are
  // broken by comparing the S2Point values of vertices lexicographically.
  //
  // However, sorting by leaf S2CellId alone has two small disadvantages in
  // the case where the candidate sites are densely spaced relative to the
  // snap radius (e.g., when using the IdentitySnapFunction, or when snapping
  // to E6/E7 near the poles, or snapping to S2CellId/E6/E7 using a snap
  // radius larger than the minimum value required):
  //
  //  - First, it tends to bias the Voronoi site locations towards points that
  //    are earlier on the S2CellId Hilbert curve.  For example, suppose that
  //    there are two parallel rows of input vertices on opposite sides of the
  //    edge between two large S2Cells, and the rows are separated by less
  //    than the snap radius.  Then only vertices from the cell with the
  //    smaller S2CellId are selected, because they are considered first and
  //    prevent us from selecting the sites from the other cell (because they
  //    are closer than "snap_radius" to an existing site).
  //
  //  - Second, it tends to choose more Voronoi sites than necessary, because
  //    at each step we choose the first site along the Hilbert curve that is
  //    at least "snap_radius" away from all previously selected sites.  This
  //    tends to yield sites whose "coverage discs" overlap quite a bit,
  //    whereas it would be better to cover all the input vertices with a
  //    smaller set of coverage discs that don't overlap as much.  (This is
  //    the "geometric set cover problem", which is NP-hard.)
  //
  // It is not worth going to much trouble to fix these problems, because they
  // really aren't that important (and don't affect the guarantees made by the
  // algorithm), but here are a couple of heuristics that might help:
  //
  // 1. Sort the input vertices by S2CellId at a coarse level (down to cells
  // that are O(snap_radius) in size), and then sort by a fingerprint of the
  // S2Point coordinates (i.e., quasi-randomly).  This would retain most of
  // the advantages of S2CellId sorting, but makes it more likely that we will
  // select sites that are further apart.
  //
  // 2. Rather than choosing the first uncovered input vertex and snapping it
  // to obtain the next Voronoi site, instead look ahead through following
  // candidates in S2CellId order and choose the furthest candidate whose
  // snapped location covers all previous uncovered input vertices.
  //
  // TODO(ericv): Experiment with these approaches.

  vector<InputVertexKey> keys;
  keys.reserve(input_vertices_.size());
  for (InputVertexId i = 0; static_cast<size_t>(i) < input_vertices_.size();
       ++i) {
    keys.push_back(InputVertexKey(S2CellId(input_vertices_[i]), i));
  }
  std::sort(keys.begin(), keys.end(),
            [this](const InputVertexKey& a, const InputVertexKey& b) {
      if (a.first < b.first) return true;
      if (b.first < a.first) return false;
      if (input_vertices_[a.second] < input_vertices_[b.second]) return true;
      if (input_vertices_[b.second] < input_vertices_[a.second]) return false;
      return a.second < b.second;  // Tie-break to ensure deterministic result.
    });
  return keys;
}

// Check all edge pairs for crossings, and add the corresponding intersection
// points to input_vertices_.  (The intersection points will be snapped and
// merged with the other vertices during site selection.)
void S2Builder::AddEdgeCrossings(const MutableS2ShapeIndex& input_edge_index) {
  input_edge_index.ForceBuild();
  if (!tracker_.ok()) return;

  // We need to build a list of intersections and add them afterwards so that
  // we don't reallocate vertices_ during the VisitCrossings() call.
  vector<S2Point> new_vertices;
  auto _ = absl::MakeCleanup([&]() { tracker_.Untally(new_vertices); });
  s2shapeutil::VisitCrossingEdgePairs(
      input_edge_index, s2shapeutil::CrossingType::INTERIOR,
      [this, &new_vertices](const s2shapeutil::ShapeEdge& a,
                            const s2shapeutil::ShapeEdge& b, bool) {
        if (!tracker_.AddSpace(&new_vertices, 1)) return false;
        new_vertices.push_back(
            S2::GetIntersection(a.v0(), a.v1(), b.v0(), b.v1()));
        return true;
      });
  if (new_vertices.empty()) return;

  snapping_needed_ = true;
  if (!tracker_.AddSpaceExact(&input_vertices_, new_vertices.size())) return;
  input_vertices_.insert(input_vertices_.end(),
                         new_vertices.begin(), new_vertices.end());
}

void S2Builder::AddForcedSites(S2PointIndex<SiteId>* site_index) {
  // Sort the forced sites and remove duplicates.
  std::sort(sites_.begin(), sites_.end());
  sites_.erase(std::unique(sites_.begin(), sites_.end()), sites_.end());
  // Add the forced sites to the index.
  for (SiteId id = 0; static_cast<size_t>(id) < sites_.size(); ++id) {
    if (!tracker_.TallyIndexedSite()) return;
    site_index->Add(sites_[id], id);
  }
  num_forced_sites_ = sites_.size();
}

void S2Builder::ChooseInitialSites(S2PointIndex<SiteId>* site_index) {
  // Prepare to find all points whose distance is <= min_site_separation_ca_.
  S2ClosestPointQueryOptions options;
  options.set_conservative_max_distance(min_site_separation_ca_);
  S2ClosestPointQuery<SiteId> site_query(site_index, options);
  vector<S2ClosestPointQuery<SiteId>::Result> results;

  // Apply the snap_function() to each input vertex, then check whether any
  // existing site is closer than min_vertex_separation().  If not, then add a
  // new site.
  //
  // NOTE(ericv): There are actually two reasonable algorithms, which we call
  // "snap first" (the one above) and "snap last".  The latter checks for each
  // input vertex whether any existing site is closer than snap_radius(), and
  // only then applies the snap_function() and adds a new site.  "Snap last"
  // can yield slightly fewer sites in some cases, but it is also more
  // expensive and can produce surprising results.  For example, if you snap
  // the polyline "0:0, 0:0.7" using IntLatLngSnapFunction(0), the result is
  // "0:0, 0:0" rather than the expected "0:0, 0:1", because the snap radius
  // is approximately sqrt(2) degrees and therefore it is legal to snap both
  // input points to "0:0".  "Snap first" produces "0:0, 0:1" as expected.
  //
  // Track the memory used by SortInputVertices() before calling it.
  if (!tracker_.Tally(input_vertices_.size() * sizeof(InputVertexKey))) return;
  vector<InputVertexKey> sorted_keys = SortInputVertices();
  auto _ = absl::MakeCleanup([&]() { tracker_.Untally(sorted_keys); });
  for (const InputVertexKey& key : sorted_keys) {
    const S2Point& vertex = input_vertices_[key.second];
    S2Point site = SnapSite(vertex);
    // If any vertex moves when snapped, the output cannot be idempotent.
    snapping_needed_ = snapping_needed_ || site != vertex;

    bool add_site = true;
    if (site_snap_radius_ca_ == S1ChordAngle::Zero()) {
      add_site = sites_.empty() || site != sites_.back();
    } else {
      // FindClosestPoints() measures distances conservatively, so we need to
      // recheck the distances using exact predicates.
      //
      // NOTE(ericv): When the snap radius is large compared to the average
      // vertex spacing, we could possibly avoid the call the FindClosestPoints
      // by checking whether sites_.back() is close enough.
      S2ClosestPointQueryPointTarget target(site);
      site_query.FindClosestPoints(&target, &results);
      for (const auto& result : results) {
        if (s2pred::CompareDistance(site, result.point(),
                                    min_site_separation_ca_) <= 0) {
          add_site = false;
          // This pair of sites is too close.  If the sites are distinct, then
          // the output cannot be idempotent.
          snapping_needed_ = snapping_needed_ || site != result.point();
        }
      }
    }
    if (add_site) {
      if (!tracker_.TallyIndexedSite()) return;
      site_index->Add(site, sites_.size());
      if (!tracker_.AddSpace(&sites_, 1)) return;
      sites_.push_back(site);
      site_query.ReInit();
    }
  }
}

S2Point S2Builder::SnapSite(const S2Point& point) const {
  if (!snapping_requested_) return point;
  S2Point site = options_.snap_function().SnapPoint(point);
  S1ChordAngle dist_moved(site, point);
  if (dist_moved > site_snap_radius_ca_) {
    *error_ = S2Error(
        S2Error::BUILDER_SNAP_RADIUS_TOO_SMALL,
        absl::StrFormat("Snap function moved vertex (%.15g, %.15g, %.15g) "
                        "by %.15g, which is more than the specified snap "
                        "radius of %.15g",
                        point.x(), point.y(), point.z(),
                        dist_moved.ToAngle().radians(),
                        site_snap_radius_ca_.ToAngle().radians()));
  }
  return site;
}

// For each edge, find all sites within edge_site_query_radius_ca_ and
// store them in edge_sites_.  Also, to implement idempotency this method also
// checks whether the input vertices and edges may already satisfy the output
// criteria.  If any problems are found then snapping_needed_ is set to true.
void S2Builder::CollectSiteEdges(const S2PointIndex<SiteId>& site_index) {
  // Find all points whose distance is <= edge_site_query_radius_ca_.
  //
  // Memory used by S2ClosestPointQuery is not tracked, but it is temporary,
  // typically insignificant, and does not affect the high water mark.
  S2ClosestPointQueryOptions options;
  options.set_conservative_max_distance(edge_site_query_radius_ca_);
  S2ClosestPointQuery<SiteId> site_query(&site_index, options);
  vector<S2ClosestPointQuery<SiteId>::Result> results;
  if (!tracker_.AddSpaceExact(&edge_sites_, input_edges_.size())) return;
  edge_sites_.resize(input_edges_.size());  // Construct all elements.
  for (InputEdgeId e = 0; static_cast<size_t>(e) < input_edges_.size(); ++e) {
    const InputEdge& edge = input_edges_[e];
    const S2Point& v0 = input_vertices_[edge.first];
    const S2Point& v1 = input_vertices_[edge.second];
    S2ClosestPointQueryEdgeTarget target(v0, v1);
    site_query.FindClosestPoints(&target, &results);
    auto* sites = &edge_sites_[e];
    sites->reserve(results.size());
    for (const auto& result : results) {
      sites->push_back(result.data());
      if (!snapping_needed_ &&
          result.distance() < min_edge_site_separation_ca_limit_ &&
          result.point() != v0 && result.point() != v1 &&
          s2pred::CompareEdgeDistance(result.point(), v0, v1,
                                      min_edge_site_separation_ca_) < 0) {
        snapping_needed_ = true;
      }
    }
    SortSitesByDistance(v0, sites);
    if (!tracker_.TallyEdgeSites(*sites)) return;
  }
}

// Sorts the sites in increasing order of distance to X.
void S2Builder::SortSitesByDistance(const S2Point& x,
                                    compact_array<SiteId>* sites) const {
  std::sort(sites->begin(), sites->end(), [&x, this](SiteId i, SiteId j) {
    if (i == j) return false;
    int diff = s2pred::CompareDistances(x, sites_[i], sites_[j]);
    if (diff < 0) return true;
    if (diff > 0) return false;
    return i < j;  // Tie-break to ensure deterministic result.
  });
}

// Like the above, but inserts "new_site_id" into an already-sorted list.
void S2Builder::InsertSiteByDistance(SiteId new_site_id, const S2Point& x,
                                     compact_array<SiteId>* sites) {
  if (!tracker_.ReserveEdgeSite(sites)) return;
  sites->insert(std::lower_bound(
      sites->begin(), sites->end(), new_site_id,
      [&x, this](SiteId i, SiteId j) {
        return s2pred::CompareDistances(x, sites_[i], sites_[j]) < 0;
      }), new_site_id);
}

// There are two situations where we need to add extra Voronoi sites in order to
// ensure that the snapped edges meet the output requirements:
//
//  (1) If a snapped edge deviates from its input edge by more than
//      max_edge_deviation(), we add a new site on the input edge near the
//      middle of the snapped edge.  This causes the snapped edge to split
//      into two pieces, so that it follows the input edge more closely.
//
//  (2) If a snapped edge is closer than min_edge_vertex_separation() to any
//      nearby site (the "site to avoid") or passes on the wrong side of it
//      relative to the input edge, then we add a new site (the "separation
//      site") along the input edge near the site to avoid.  This causes the
//      snapped edge to follow the input edge more closely, so that it is
//      guaranteed to pass on the correct side of the site to avoid with a
//      separation of at least the required distance.
//
// We check these conditions by snapping all the input edges to a chain of
// Voronoi sites and then testing each edge in the chain.  If a site needs to
// be added, we mark all nearby edges for re-snapping.
void S2Builder::AddExtraSites(const MutableS2ShapeIndex& input_edge_index) {
  // Note that we could save some work in AddSnappedEdges() by saving the
  // snapped edge chains in a vector, but currently this is not worthwhile
  // since SnapEdge() accounts for less than 5% of the runtime.

  // Using 18 buckets is equivalent to `reserve(16)`, which was the
  // `expected_max_elements` used by the old `dense_hash_set` version.
  // This will actually get us 31 buckets since it gets rounded up.
  // We could experiment with different values here.
  flat_hash_set<InputEdgeId> edges_to_resnap(/*bucket_count=*/18);

  vector<SiteId> chain;  // Temporary storage.
  int num_edges_after_snapping = 0;

  // CheckEdge() defines the body of the loops below.
  const auto CheckEdge = [&](InputEdgeId e) -> bool {
      if (!tracker_.ok()) return false;
      SnapEdge(e, &chain);
      edges_to_resnap.erase(e);
      num_edges_after_snapping += chain.size();
      MaybeAddExtraSites(e, chain, input_edge_index, &edges_to_resnap);
      return true;
    };

  // The first pass is different because we snap every edge.  In the following
  // passes we only snap edges that are near the extra sites that were added.
  ABSL_VLOG(1) << "Before pass 0: sites=" << sites_.size();
  for (InputEdgeId e = 0; static_cast<size_t>(e) < input_edges_.size(); ++e) {
    if (!CheckEdge(e)) return;
  }
  ABSL_VLOG(1) << "Pass 0: edges snapped=" << input_edges_.size()
               << ", output edges=" << num_edges_after_snapping
               << ", sites=" << sites_.size();

  for (int num_passes = 1; !edges_to_resnap.empty(); ++num_passes) {
    auto edges_to_snap = edges_to_resnap;
    edges_to_resnap.clear();
    num_edges_after_snapping = 0;
    for (InputEdgeId e : edges_to_snap) {
      if (!CheckEdge(e)) return;
    }
    ABSL_VLOG(1) << "Pass " << num_passes
                 << ": edges snapped=" << edges_to_snap.size()
                 << ", output edges=" << num_edges_after_snapping
                 << ", sites=" << sites_.size();
  }
}

void S2Builder::MaybeAddExtraSites(
    InputEdgeId edge_id, absl::Span<const SiteId> chain,
    const MutableS2ShapeIndex& input_edge_index,
    flat_hash_set<InputEdgeId>* edges_to_resnap) {
  // If the memory tracker has a periodic callback function, tally an amount
  // of memory proportional to the work being done so that the caller has an
  // opportunity to cancel the operation if necessary.
  if (!tracker_.TallyTemp(chain.size() * sizeof(chain[0]))) return;

  // If the input includes NaN vertices, snapping can produce an empty chain.
  if (chain.empty()) return;

  // The snapped edge chain is always a subsequence of the nearby sites
  // (edge_sites_), so we walk through the two arrays in parallel looking for
  // sites that weren't snapped.  These are the "sites to avoid".  We also keep
  // track of the current snapped edge, since it is the only edge that can be
  // too close or pass on the wrong side of a site to avoid.  Vertices beyond
  // the chain endpoints in either direction can be ignored because only the
  // interiors of chain edges can be too close to a site to avoid.
  const InputEdge& edge = input_edges_[edge_id];
  const S2Point& a0 = input_vertices_[edge.first];
  const S2Point& a1 = input_vertices_[edge.second];
  const auto& nearby_sites = edge_sites_[edge_id];
  for (size_t i = 0, j = 0; j < nearby_sites.size(); ++j) {
    SiteId id = nearby_sites[j];
    if (id == chain[i]) {
      // This site is a vertex of the snapped edge chain.
      if (++i == chain.size()) {
        break;  // Sites beyond the end of the snapped chain can be ignored.
      }
      // Check whether this snapped edge deviates too far from its original
      // position.  If so, we split the edge by adding an extra site.
      const S2Point& v0 = sites_[chain[i - 1]];
      const S2Point& v1 = sites_[chain[i]];
      if (S1ChordAngle(v0, v1) < min_edge_length_to_split_ca_) continue;
      if (!S2::IsEdgeBNearEdgeA(a0, a1, v0, v1, max_edge_deviation_)) {
        // Add a new site on the input edge, positioned so that it splits the
        // snapped edge into two approximately equal pieces.  Then we find all
        // the edges near the new site (including this one) and add them to
        // the snap queue.
        //
        // Note that with large snap radii, it is possible that the snapped
        // edge wraps around the sphere the "wrong way".  To handle this we
        // find the preferred split location by projecting both endpoints onto
        // the input edge and taking their midpoint.
        S2Point mid = (S2::Project(v0, a0, a1) +
                       S2::Project(v1, a0, a1)).Normalize();
        S2Point new_site = GetSeparationSite(mid, v0, v1, edge_id);
        AddExtraSite(new_site, input_edge_index, edges_to_resnap);

        // In the case where the edge wrapped around the sphere the "wrong
        // way", it is not safe to continue checking this edge.  It will be
        // marked for resnapping and we will come back to it in the next pass.
        return;
      }
    } else {
      // This site is near the input edge but is not part of the snapped chain.
      if (i == 0) {
        continue;  // Sites before the start of the chain can be ignored.
      }
      // We need to ensure that non-forced sites are separated by at least
      // min_edge_vertex_separation() from the snapped chain.  This happens
      // automatically as part of the algorithm except where there are portions
      // of the input edge that are not within edge_snap_radius() of any site.
      // These portions of the original edge are called "coverage gaps".
      // Therefore if we find that a site to avoid that is too close to the
      // snapped edge chain, we can fix the problem by adding a new site (the
      // "separation site") in the corresponding coverage gap located as closely
      // as possible to the site to avoid.  This technique is is guaranteed to
      // produce the required minimum separation, and the entire process of
      // adding separation sites is guaranteed to terminate.
      const S2Point& site_to_avoid = sites_[id];
      const S2Point& v0 = sites_[chain[i - 1]];
      const S2Point& v1 = sites_[chain[i]];
      bool add_separation_site = false;
      if (!is_forced(id) &&
          min_edge_site_separation_ca_ > S1ChordAngle::Zero() &&
          s2pred::CompareEdgeDistance(
              site_to_avoid, v0, v1, min_edge_site_separation_ca_) < 0) {
        add_separation_site = true;
      }
      // Similarly, we also add a separation site whenever a snapped edge passes
      // on the wrong side of a site to avoid.  Normally we don't need to worry
      // about this, since if an edge passes on the wrong side of a nearby site
      // then it is also too close to it.  However if the snap radius is very
      // small and intersection_tolerance() is non-zero then we need to check
      // this condition explicitly (see the "check_all_site_crossings_" flag for
      // details).  We also need to check this condition explicitly for forced
      // vertices.  Again, we can solve this problem by adding a "separation
      // site" in the corresponding coverage gap located as closely as possible
      // to the site to avoid.
      //
      // It is possible to show that when all points are projected onto the
      // great circle through (a0, a1), no improper crossing occurs unless the
      // the site to avoid is located between a0 and a1, and also between v0
      // and v1.  TODO(ericv): Verify whether all these checks are necessary.
      if (!add_separation_site &&
          (is_forced(id) || check_all_site_crossings_) &&
          (s2pred::Sign(a0, a1, site_to_avoid) !=
           s2pred::Sign(v0, v1, site_to_avoid))&&
          s2pred::CompareEdgeDirections(a0, a1, a0, site_to_avoid) > 0 &&
          s2pred::CompareEdgeDirections(a0, a1, site_to_avoid, a1) > 0 &&
          s2pred::CompareEdgeDirections(a0, a1, v0, site_to_avoid) > 0 &&
          s2pred::CompareEdgeDirections(a0, a1, site_to_avoid, v1) > 0) {
        add_separation_site = true;
      }
      if (add_separation_site) {
        // We add a new site (the separation site) in the coverage gap along the
        // input edge, located as closely as possible to the site to avoid.
        // Then we find all the edges near the new site (including this one) and
        // add them to the snap queue.
        S2Point new_site = GetSeparationSite(site_to_avoid, v0, v1, edge_id);
        ABSL_DCHECK_NE(site_to_avoid, new_site);
        AddExtraSite(new_site, input_edge_index, edges_to_resnap);

        // Skip the remaining sites near this chain edge, and then continue
        // scanning this chain.  Note that this is safe even though the call
        // to AddExtraSite() above added a new site to "nearby_sites".
        for (; nearby_sites[j + 1] != chain[i]; ++j) {}
      }
    }
  }
}

// Adds a new site, then updates "edge_sites_" for all edges near the new site
// and adds them to "edges_to_resnap" for resnapping.
void S2Builder::AddExtraSite(const S2Point& new_site,
                             const MutableS2ShapeIndex& input_edge_index,
                             flat_hash_set<InputEdgeId>* edges_to_resnap) {
  if (!sites_.empty()) ABSL_DCHECK_NE(new_site, sites_.back());
  if (!tracker_.AddSpace(&sites_, 1)) return;
  SiteId new_site_id = sites_.size();
  sites_.push_back(new_site);

  // Find all edges whose distance is <= edge_site_query_radius_ca_.
  S2ClosestEdgeQuery::Options options;
  options.set_conservative_max_distance(edge_site_query_radius_ca_);
  options.set_include_interiors(false);

  if (!input_edge_index.is_fresh()) input_edge_index.ForceBuild();
  if (!tracker_.ok()) return;

  // Memory used by S2ClosestEdgeQuery is not tracked, but it is temporary,
  // typically insignificant, and does not affect the high water mark.
  S2ClosestEdgeQuery query(&input_edge_index, options);
  S2ClosestEdgeQuery::PointTarget target(new_site);
  for (const auto& result : query.FindClosestEdges(&target)) {
    InputEdgeId e = result.edge_id();
    const S2Point& v0 = input_vertices_[input_edges_[e].first];
    InsertSiteByDistance(new_site_id, v0, &edge_sites_[e]);
    edges_to_resnap->insert(e);
  }
}

S2Point S2Builder::GetSeparationSite(const S2Point& site_to_avoid,
                                     const S2Point& v0, const S2Point& v1,
                                     InputEdgeId input_edge_id) const {
  // Define the "coverage disc" of a site S to be the disc centered at S with
  // radius "snap_radius".  Similarly, define the "coverage interval" of S for
  // an edge XY to be the intersection of XY with the coverage disc of S.  The
  // SnapFunction implementations guarantee that the only way that a snapped
  // edge can be closer than min_edge_vertex_separation() to a non-snapped
  // site (i.e., site_to_avoid) if is there is a gap in the coverage of XY
  // near this site.  We can fix this problem simply by adding a new site to
  // fill this gap, located as closely as possible to the site to avoid.
  //
  // To calculate the coverage gap, we look at the two snapped sites on
  // either side of site_to_avoid, and find the endpoints of their coverage
  // intervals.  The we place a new site in the gap, located as closely as
  // possible to the site to avoid.  Note that the new site may move when it
  // is snapped by the snap_function, but it is guaranteed not to move by
  // more than snap_radius and therefore its coverage interval will still
  // intersect the gap.
  const InputEdge& edge = input_edges_[input_edge_id];
  const S2Point& x = input_vertices_[edge.first];
  const S2Point& y = input_vertices_[edge.second];
  Vector3_d xy_dir = y - x;
  S2Point n = S2::RobustCrossProd(x, y);
  S2Point new_site = S2::Project(site_to_avoid, x, y, n);
  S2Point gap_min = GetCoverageEndpoint(v0, n);
  S2Point gap_max = GetCoverageEndpoint(v1, -n);
  if ((new_site - gap_min).DotProd(xy_dir) < 0) {
    new_site = gap_min;
  } else if ((gap_max - new_site).DotProd(xy_dir) < 0) {
    new_site = gap_max;
  }
  new_site = SnapSite(new_site);
  ABSL_DCHECK_NE(v0, new_site);
  ABSL_DCHECK_NE(v1, new_site);
  return new_site;
}

// Given a site P and an edge XY with normal N, intersect XY with the disc of
// radius snap_radius() around P, and return the intersection point that is
// further along the edge XY toward Y.
S2Point S2Builder::GetCoverageEndpoint(const S2Point& p, const S2Point& n)
    const {
  // Consider the plane perpendicular to P that cuts off a spherical cap of
  // radius snap_radius().  This plane intersects the plane through the edge
  // XY (perpendicular to N) along a line, and that line intersects the unit
  // sphere at two points Q and R, and we want to return the point R that is
  // further along the edge XY toward Y.
  //
  // Let M be the midpoint of QR.  This is the point along QR that is closest
  // to P.  We can now express R as the sum of two perpendicular vectors OM
  // and MR in the plane XY.  Vector MR is in the direction N x P, while
  // vector OM is in the direction (N x P) x N, where N = X x Y.
  //
  // The length of OM can be found using the Pythagorean theorem on triangle
  // OPM, and the length of MR can be found using the Pythagorean theorem on
  // triangle OMR.
  //
  // In the calculations below, we save some work by scaling all the vectors
  // by n.CrossProd(p).Norm2(), and normalizing at the end.
  double n2 = n.Norm2();
  double nDp = n.DotProd(p);
  S2Point nXp = n.CrossProd(p);
  S2Point nXpXn = n2 * p - nDp * n;
  Vector3_d om = sqrt(1 - edge_snap_radius_sin2_) * nXpXn;
  double mr2 = edge_snap_radius_sin2_ * n2 - nDp * nDp;

  // MR is constructed so that it points toward Y (rather than X).
  Vector3_d mr = sqrt(max(0.0, mr2)) * nXp;
  return (om + mr).Normalize();
}

void S2Builder::SnapEdge(InputEdgeId e, vector<SiteId>* chain) const {
  chain->clear();
  const InputEdge& edge = input_edges_[e];
  if (!snapping_needed_) {
    // Note that the input vertices have been renumbered such that
    // InputVertexId and SiteId are the same (see ChooseAllVerticesAsSites).
    chain->push_back(edge.first);
    chain->push_back(edge.second);
    return;
  }

  const S2Point& x = input_vertices_[edge.first];
  const S2Point& y = input_vertices_[edge.second];

  // Optimization: if there is only one nearby site, return.
  // Optimization: if there are exactly two nearby sites, and one is close
  // enough to each vertex, then return.

  // Now iterate through the sites.  We keep track of the sequence of sites
  // that are visited.
  const auto& candidates = edge_sites_[e];
  for (SiteId site_id : candidates) {
    const S2Point& c = sites_[site_id];
    // Skip any sites that are too far away.  (There will be some of these,
    // because we also keep track of "sites to avoid".)  Note that some sites
    // may be close enough to the line containing the edge, but not to the
    // edge itself, so we can just use the dot product with the edge normal.
    if (s2pred::CompareEdgeDistance(c, x, y, edge_snap_radius_ca_) > 0) {
      continue;
    }
    // Check whether the new site C excludes the previous site B.  If so,
    // repeat with the previous site, and so on.
    bool add_site_c = true;
    for (; !chain->empty(); chain->pop_back()) {
      S2Point b = sites_[chain->back()];

      // First, check whether B and C are so far apart that their clipped
      // Voronoi regions can't intersect.
      S1ChordAngle bc(b, c);
      if (bc >= max_adjacent_site_separation_ca_) break;

      // Otherwise, we want to check whether site C prevents the Voronoi
      // region of B from intersecting XY, or vice versa.  This can be
      // determined by computing the "coverage interval" (the segment of XY
      // intersected by the coverage disc of radius snap_radius) for each
      // site.  If the coverage interval of one site contains the coverage
      // interval of the other, then the contained site can be excluded.
      s2pred::Excluded result = s2pred::GetVoronoiSiteExclusion(
          b, c, x, y, edge_snap_radius_ca_);
      if (result == s2pred::Excluded::FIRST) continue;  // Site B excluded by C
      if (result == s2pred::Excluded::SECOND) {
        add_site_c = false;  // Site C is excluded by B.
        break;
      }
      ABSL_DCHECK_EQ(s2pred::Excluded::NEITHER, result);

      // Otherwise check whether the previous site A is close enough to B and
      // C that it might further clip the Voronoi region of B.
      if (chain->size() < 2) break;
      S2Point a = sites_[chain->end()[-2]];
      S1ChordAngle ac(a, c);
      if (ac >= max_adjacent_site_separation_ca_) break;

      // If triangles ABC and XYB have the same orientation, the circumcenter
      // Z of ABC is guaranteed to be on the same side of XY as B.
      int xyb = s2pred::Sign(x, y, b);
      if (s2pred::Sign(a, b, c) == xyb) {
        break;  // The circumcenter is on the same side as B but further away.
      }
      // Other possible optimizations:
      //  - if AB > max_adjacent_site_separation_ca_ then keep B.
      //  - if d(B, XY) < 0.5 * min(AB, BC) then keep B.

      // If the circumcenter of ABC is on the same side of XY as B, then B is
      // excluded by A and C combined.  Otherwise B is needed and we can exit.
      if (s2pred::EdgeCircumcenterSign(x, y, a, b, c) != xyb) break;
    }
    if (add_site_c) {
      chain->push_back(site_id);
    }
  }
  ABSL_DCHECK(!chain->empty());
  if (S2_DEBUG_MODE) {
    for (SiteId site_id : candidates) {
      if (s2pred::CompareDistances(y, sites_[chain->back()],
                                   sites_[site_id]) > 0) {
        ABSL_LOG(ERROR) << "Snapping invariant broken!";
      }
    }
  }
}

void S2Builder::BuildLayers() {
  if (!tracker_.ok()) return;

  // Each output edge has an "input edge id set id" (an int32_t) representing
  // the set of input edge ids that were snapped to this edge.  The actual
  // InputEdgeIds can be retrieved using "input_edge_id_set_lexicon".
  vector<vector<Edge>> layer_edges;
  vector<vector<InputEdgeIdSetId>> layer_input_edge_ids;
  IdSetLexicon input_edge_id_set_lexicon;
  vector<vector<S2Point>> layer_vertices;
  BuildLayerEdges(&layer_edges, &layer_input_edge_ids,
                  &input_edge_id_set_lexicon);
  auto _ = absl::MakeCleanup([&]() {
    for (size_t i = 0; i < layers_.size(); ++i) {
      tracker_.Untally(layer_edges[i]);
      tracker_.Untally(layer_input_edge_ids[i]);
      if (!layer_vertices.empty()) tracker_.Untally(layer_vertices[i]);
    }
  });

  // If there are a large number of layers, then we build a minimal subset of
  // vertices for each layer.  This ensures that layer types that iterate over
  // vertices will run in time proportional to the size of that layer rather
  // than the size of all layers combined.
  static const int kMinLayersForVertexFiltering = 10;
  if (layers_.size() >= kMinLayersForVertexFiltering) {
    // Disable vertex filtering if it is disallowed by any layer.  (This could
    // be optimized, but in current applications either all layers allow
    // filtering or none of them do.)
    bool allow_vertex_filtering = true;
    for (const auto& options : layer_options_) {
      allow_vertex_filtering &= options.allow_vertex_filtering();
    }
    if (allow_vertex_filtering) {
      // Track the temporary memory used by FilterVertices().  Note that
      // although vertex filtering can increase the number of vertices stored
      // (i.e., if the same vertex is referred to by multiple layers), it
      // never increases storage quadratically because there can be at most
      // two filtered vertices per edge.
      if (!tracker_.TallyFilterVertices(sites_.size(), layer_edges)) return;
      auto _ = absl::MakeCleanup([this]() { tracker_.DoneFilterVertices(); });
      layer_vertices.resize(layers_.size());
      vector<Graph::VertexId> filter_tmp;  // Temporary used by FilterVertices.
      for (size_t i = 0; i < layers_.size(); ++i) {
        layer_vertices[i] = Graph::FilterVertices(sites_, &layer_edges[i],
                                                  &filter_tmp);
        if (!tracker_.Tally(layer_vertices[i])) return;
      }
      tracker_.Clear(&sites_);  // Releases memory.
    }
  }
  if (!tracker_.ok()) return;

  for (size_t i = 0; i < layers_.size(); ++i) {
    const vector<S2Point>& vertices = (layer_vertices.empty() ?
                                       sites_ : layer_vertices[i]);
    Graph graph(layer_options_[i], &vertices, &layer_edges[i],
                &layer_input_edge_ids[i], &input_edge_id_set_lexicon,
                &label_set_ids_, &label_set_lexicon_,
                layer_is_full_polygon_predicates_[i]);
    layers_[i]->Build(graph, error_);
    // Don't free the layer data until all layers have been built, in order to
    // support building multiple layers at once (e.g. ClosedSetNormalizer).
  }
}

// Snaps and possibly simplifies the edges for each layer, populating the
// given output arguments.  The resulting edges can be used to construct an
// S2Builder::Graph directly (no further processing is necessary).
//
// This method is not "const" because Graph::ProcessEdges can modify
// layer_options_ in some cases (changing undirected edges to directed ones).
void S2Builder::BuildLayerEdges(
    vector<vector<Edge>>* layer_edges,
    vector<vector<InputEdgeIdSetId>>* layer_input_edge_ids,
    IdSetLexicon* input_edge_id_set_lexicon) {
  // Edge chains are simplified only when a non-zero snap radius is specified.
  // If so, we build a map from each site to the set of input vertices that
  // snapped to that site.  (Note that site_vertices is relatively small and
  // that its memory tracking is deferred until TallySimplifyEdgeChains.)
  vector<compact_array<InputVertexId>> site_vertices;
  bool simplify = snapping_needed_ && options_.simplify_edge_chains();
  if (simplify) site_vertices.resize(sites_.size());

  layer_edges->resize(layers_.size());
  layer_input_edge_ids->resize(layers_.size());
  for (size_t i = 0; i < layers_.size(); ++i) {
    AddSnappedEdges(layer_begins_[i], layer_begins_[i+1], layer_options_[i],
                    &(*layer_edges)[i], &(*layer_input_edge_ids)[i],
                    input_edge_id_set_lexicon, &site_vertices);
  }

  // We simplify edge chains before processing the per-layer GraphOptions
  // because simplification can create duplicate edges and/or sibling edge
  // pairs which may need to be removed.
  if (simplify) {
    SimplifyEdgeChains(site_vertices, layer_edges, layer_input_edge_ids,
                       input_edge_id_set_lexicon);
    vector<compact_array<InputVertexId>>().swap(site_vertices);
  }

  // At this point we have no further need for nearby site data, so we clear
  // it to save space.  We keep input_vertices_ and input_edges_ so that
  // S2Builder::Layer implementations can access them if desired.  (This is
  // useful for determining how snapping has changed the input geometry.)
  tracker_.ClearEdgeSites(&edge_sites_);
  for (size_t i = 0; i < layers_.size(); ++i) {
    // The errors generated by ProcessEdges are really warnings, so we simply
    // record them and continue.
    Graph::ProcessEdges(&layer_options_[i], &(*layer_edges)[i],
                        &(*layer_input_edge_ids)[i],
                        input_edge_id_set_lexicon, error_, &tracker_);
    if (!tracker_.ok()) return;
  }
}

// Snaps all the input edges for a given layer, populating the given output
// arguments.  If (*site_vertices) is non-empty then it is updated so that
// (*site_vertices)[site] contains a list of all input vertices that were
// snapped to that site.
void S2Builder::AddSnappedEdges(
    InputEdgeId begin, InputEdgeId end, const GraphOptions& options,
    vector<Edge>* edges, vector<InputEdgeIdSetId>* input_edge_ids,
    IdSetLexicon* input_edge_id_set_lexicon,
    vector<compact_array<InputVertexId>>* site_vertices) {
  bool discard_degenerate_edges = (options.degenerate_edges() ==
                                   GraphOptions::DegenerateEdges::DISCARD);
  vector<SiteId> chain;
  for (InputEdgeId e = begin; e < end; ++e) {
    InputEdgeIdSetId id = input_edge_id_set_lexicon->AddSingleton(e);
    SnapEdge(e, &chain);
    if (chain.empty()) {
      continue;
    }

    int num_snapped_edges = max<int>(1, chain.size() - 1);
    if (options.edge_type() == EdgeType::UNDIRECTED) num_snapped_edges *= 2;
    if (!tracker_.AddSpace(edges, num_snapped_edges)) return;
    if (!tracker_.AddSpace(input_edge_ids, num_snapped_edges)) return;
    MaybeAddInputVertex(input_edges_[e].first, chain[0], site_vertices);
    if (chain.size() == 1) {
      if (discard_degenerate_edges) continue;
      AddSnappedEdge(chain[0], chain[0], id, options.edge_type(),
                     edges, input_edge_ids);
    } else {
      MaybeAddInputVertex(input_edges_[e].second, chain.back(), site_vertices);
      for (size_t i = 1; i < chain.size(); ++i) {
        AddSnappedEdge(chain[i-1], chain[i], id, options.edge_type(),
                       edges, input_edge_ids);
      }
    }
  }
}

// If "site_vertices" is non-empty, ensures that (*site_vertices)[id] contains
// "v".  Duplicate entries are allowed.  The purpose of this function is to
// build a map so that SimplifyEdgeChains() can quickly find all the input
// vertices that snapped to a particular site.
inline void S2Builder::MaybeAddInputVertex(
    InputVertexId v, SiteId id,
    vector<compact_array<InputVertexId>>* site_vertices) const {
  if (site_vertices->empty()) return;

  // Optimization: check if we just added this vertex.  This is worthwhile
  // because the input edges usually form a continuous chain, i.e. the
  // destination of one edge is the same as the source of the next edge.
  auto& vertices = (*site_vertices)[id];
  if (vertices.empty() || vertices.back() != v) {
    // Memory tracking is deferred until SimplifyEdgeChains.
    vertices.push_back(v);
  }
}

// Adds the given edge to "edges" and "input_edge_ids".  If undirected edges
// are being used, also adds an edge in the opposite direction.
inline void S2Builder::AddSnappedEdge(
    SiteId src, SiteId dst, InputEdgeIdSetId id, EdgeType edge_type,
    vector<Edge>* edges, vector<InputEdgeIdSetId>* input_edge_ids) const {
  edges->push_back(Edge(src, dst));
  input_edge_ids->push_back(id);
  if (edge_type == EdgeType::UNDIRECTED) {
    edges->push_back(Edge(dst, src));
    // Automatically created edges do not have input edge ids or labels.  This
    // can be used to distinguish the original direction of the undirected edge.
    input_edge_ids->push_back(IdSetLexicon::EmptySetId());
  }
}

// A class that encapsulates the state needed for simplifying edge chains.
class S2Builder::EdgeChainSimplifier {
 public:
  // The graph "g" contains all edges from all layers.  "edge_layers"
  // indicates the original layer for each edge.  "site_vertices" is a map
  // from SiteId to the set of InputVertexIds that were snapped to that site.
  // "layer_edges" and "layer_input_edge_ids" are output arguments where the
  // simplified edge chains will be placed.  The input and output edges are
  // not sorted.
  EdgeChainSimplifier(
      const S2Builder& builder, const Graph& g,
      const vector<int>& edge_layers,
      const vector<compact_array<InputVertexId>>& site_vertices,
      vector<vector<Edge>>* layer_edges,
      vector<vector<InputEdgeIdSetId>>* layer_input_edge_ids,
      IdSetLexicon* input_edge_id_set_lexicon);

  void Run();

 private:
  using VertexId = Graph::VertexId;

  class InteriorVertexMatcher;

  void OutputEdge(EdgeId e);
  int graph_edge_layer(EdgeId e) const;
  int input_edge_layer(InputEdgeId id) const;
  bool IsInterior(VertexId v);
  void SimplifyChain(VertexId v0, VertexId v1);
  Graph::VertexId FollowChain(VertexId v0, VertexId v1) const;
  void OutputAllEdges(VertexId v0, VertexId v1);
  bool TargetInputVertices(VertexId v, S2PolylineSimplifier* simplifier) const;
  bool AvoidSites(VertexId v0, VertexId v1, VertexId v2,
                  flat_hash_set<VertexId>* used_vertices,
                  S2PolylineSimplifier* simplifier) const;
  void MergeChain(absl::Span<const VertexId> vertices);
  void AssignDegenerateEdges(absl::Span<const InputEdgeId> degenerate_ids,
                             vector<vector<InputEdgeId>>* merged_ids) const;

  // LINT.IfChange
  const S2Builder& builder_;
  const Graph& g_;
  Graph::VertexInMap in_;
  Graph::VertexOutMap out_;
  const vector<int>& edge_layers_;
  const vector<compact_array<InputVertexId>>& site_vertices_;
  vector<vector<Edge>>* layer_edges_;
  vector<vector<InputEdgeIdSetId>>* layer_input_edge_ids_;
  IdSetLexicon* input_edge_id_set_lexicon_;

  // Convenience member copied from builder_.
  const vector<InputEdgeId>& layer_begins_;

  // is_interior_[v] indicates that VertexId "v" is eligible to be an interior
  // vertex of a simplified edge chain.  You can think of it as vertex whose
  // indegree and outdegree are both 1 (although the actual definition is a
  // bit more complicated because of duplicate edges and layers).
  vector<bool> is_interior_;

  // used_[e] indicates that EdgeId "e" has already been processed.
  vector<bool> used_;

  // Temporary objects declared here to avoid repeated allocation.
  vector<VertexId> tmp_vertices_;
  vector<EdgeId> tmp_edges_;
  flat_hash_set<VertexId> tmp_vertex_set_;

  // The output edges after simplification.
  vector<Edge> new_edges_;
  vector<InputEdgeIdSetId> new_input_edge_ids_;
  vector<int> new_edge_layers_;
};

// Simplifies edge chains, updating its input/output arguments as necessary.
void S2Builder::SimplifyEdgeChains(
    const vector<compact_array<InputVertexId>>& site_vertices,
    vector<vector<Edge>>* layer_edges,
    vector<vector<InputEdgeIdSetId>>* layer_input_edge_ids,
    IdSetLexicon* input_edge_id_set_lexicon) {
  if (layers_.empty()) return;
  if (!tracker_.TallySimplifyEdgeChains(site_vertices, *layer_edges)) return;

  // Merge the edges from all layers (in order to build a single graph).
  vector<Edge> merged_edges;
  vector<InputEdgeIdSetId> merged_input_edge_ids;
  vector<int> merged_edge_layers;
  MergeLayerEdges(*layer_edges, *layer_input_edge_ids,
                  &merged_edges, &merged_input_edge_ids, &merged_edge_layers);

  // The following fields will be reconstructed by EdgeChainSimplifier.
  for (auto& edges : *layer_edges) edges.clear();
  for (auto& input_edge_ids : *layer_input_edge_ids) input_edge_ids.clear();

  // The graph options are irrelevant for edge chain simplification, but we
  // try to set them appropriately anyway.
  S2Builder::GraphOptions graph_options(EdgeType::DIRECTED,
                                        GraphOptions::DegenerateEdges::KEEP,
                                        GraphOptions::DuplicateEdges::KEEP,
                                        GraphOptions::SiblingPairs::KEEP);
  Graph graph(graph_options, &sites_, &merged_edges, &merged_input_edge_ids,
              input_edge_id_set_lexicon, nullptr, nullptr,
              IsFullPolygonPredicate());
  EdgeChainSimplifier simplifier(
      *this, graph, merged_edge_layers, site_vertices,
      layer_edges, layer_input_edge_ids, input_edge_id_set_lexicon);
  simplifier.Run();
}
// LINT.ThenChange(:TallySimplifyEdgeChains)

// Merges the edges from all layers and sorts them in lexicographic order so
// that we can construct a single graph.  The sort is stable, which means that
// any duplicate edges within each layer will still be sorted by InputEdgeId.
void S2Builder::MergeLayerEdges(
    absl::Span<const vector<Edge>> layer_edges,
    absl::Span<const vector<InputEdgeIdSetId>> layer_input_edge_ids,
    vector<Edge>* edges, vector<InputEdgeIdSetId>* input_edge_ids,
    vector<int>* edge_layers) const {
  vector<LayerEdgeId> order;
  for (size_t i = 0; i < layer_edges.size(); ++i) {
    for (size_t e = 0; e < layer_edges[i].size(); ++e) {
      order.push_back(LayerEdgeId(i, e));
    }
  }
  std::sort(order.begin(), order.end(),
            [layer_edges](const LayerEdgeId& ai, const LayerEdgeId& bi) {
              return StableLessThan(layer_edges[ai.first][ai.second],
                                    layer_edges[bi.first][bi.second], ai, bi);
            });
  edges->reserve(order.size());
  input_edge_ids->reserve(order.size());
  edge_layers->reserve(order.size());
  for (const LayerEdgeId& id : order) {
    edges->push_back(layer_edges[id.first][id.second]);
    input_edge_ids->push_back(layer_input_edge_ids[id.first][id.second]);
    edge_layers->push_back(id.first);
  }
}

// A comparison function that allows stable sorting with std::sort (which is
// fast but not stable).  It breaks ties between equal edges by comparing
// their LayerEdgeIds.
inline bool S2Builder::StableLessThan(
    const Edge& a, const Edge& b,
    const LayerEdgeId& ai, const LayerEdgeId& bi) {
  // The compiler doesn't optimize this as well as it should:
  //   return std::make_pair(a, ai) < std::make_pair(b, bi);
  if (a.first < b.first) return true;
  if (b.first < a.first) return false;
  if (a.second < b.second) return true;
  if (b.second < a.second) return false;
  return ai < bi;  // Stable sort.
}

S2Builder::EdgeChainSimplifier::EdgeChainSimplifier(
    const S2Builder& builder, const Graph& g, const vector<int>& edge_layers,
    const vector<compact_array<InputVertexId>>& site_vertices,
    vector<vector<Edge>>* layer_edges,
    vector<vector<InputEdgeIdSetId>>* layer_input_edge_ids,
    IdSetLexicon* input_edge_id_set_lexicon)
    : builder_(builder), g_(g), in_(g), out_(g), edge_layers_(edge_layers),
      site_vertices_(site_vertices), layer_edges_(layer_edges),
      layer_input_edge_ids_(layer_input_edge_ids),
      input_edge_id_set_lexicon_(input_edge_id_set_lexicon),
      layer_begins_(builder_.layer_begins_),
      is_interior_(g.num_vertices()), used_(g.num_edges()),
      // See `AddExtraSites` for explanation of `bucket_count`.
      tmp_vertex_set_(/*bucket_count=*/18) {
  new_edges_.reserve(g.num_edges());
  new_input_edge_ids_.reserve(g.num_edges());
  new_edge_layers_.reserve(g.num_edges());
}

void S2Builder::EdgeChainSimplifier::Run() {
  // Determine which vertices can be interior vertices of an edge chain.
  for (VertexId v = 0; v < g_.num_vertices(); ++v) {
    is_interior_[v] = IsInterior(v);
  }
  // Attempt to simplify all edge chains that start from a non-interior
  // vertex.  (This takes care of all chains except loops.)
  for (EdgeId e = 0; e < g_.num_edges(); ++e) {
    if (used_[e]) continue;
    Edge edge = g_.edge(e);
    if (is_interior_[edge.first]) continue;
    if (!is_interior_[edge.second]) {
      OutputEdge(e);  // An edge between two non-interior vertices.
    } else {
      SimplifyChain(edge.first, edge.second);
    }
  }
  // If there are any edges left, they form one or more disjoint loops where
  // all vertices are interior vertices.
  //
  // TODO(ericv): It would be better to start from the edge with the smallest
  // min_input_edge_id(), since that would make the output more predictable
  // for testing purposes.  It also means that we won't create an edge that
  // spans the start and end of a polyline if the polyline is snapped into a
  // loop.  (Unfortunately there are pathological examples that prevent us
  // from guaranteeing this in general, e.g. there could be two polylines in
  // different layers that snap to the same loop but start at different
  // positions.  In general we only consider input edge ids to be a hint
  // towards the preferred output ordering.)
  for (EdgeId e = 0; e < g_.num_edges(); ++e) {
    if (used_[e]) continue;
    Edge edge = g_.edge(e);
    if (edge.first == edge.second) {
      // Note that it is safe to output degenerate edges as we go along,
      // because this vertex has at least one non-degenerate outgoing edge and
      // therefore we will (or just did) start an edge chain here.
      OutputEdge(e);
    } else {
      SimplifyChain(edge.first, edge.second);
    }
  }
  // TODO(ericv): The graph is not needed past here, so we could save some
  // memory by clearing the underlying Edge and InputEdgeIdSetId vectors.

  // Finally, copy the output edges into the appropriate layers.  They don't
  // need to be sorted because the input edges were also unsorted.
  for (size_t e = 0; e < new_edges_.size(); ++e) {
    int layer = new_edge_layers_[e];
    (*layer_edges_)[layer].push_back(new_edges_[e]);
    (*layer_input_edge_ids_)[layer].push_back(new_input_edge_ids_[e]);
  }
}

// Copies the given edge to the output and marks it as used.
inline void S2Builder::EdgeChainSimplifier::OutputEdge(EdgeId e) {
  new_edges_.push_back(g_.edge(e));
  new_input_edge_ids_.push_back(g_.input_edge_id_set_id(e));
  new_edge_layers_.push_back(edge_layers_[e]);
  used_[e] = true;
}

// Returns the layer that a given graph edge belongs to.
inline int S2Builder::EdgeChainSimplifier::graph_edge_layer(EdgeId e) const {
  return edge_layers_[e];
}

// Returns the layer than a given input edge belongs to.
int S2Builder::EdgeChainSimplifier::input_edge_layer(InputEdgeId id) const {
  // NOTE(ericv): If this method shows up in profiling, the result could be
  // stored with each edge (i.e., edge_layers_ and new_edge_layers_).
  ABSL_DCHECK_GE(id, 0);
  return (std::upper_bound(layer_begins_.begin(), layer_begins_.end(), id) -
          (layer_begins_.begin() + 1));
}

// A helper class for determining whether a vertex can be an interior vertex
// of a simplified edge chain.  Such a vertex must be adjacent to exactly two
// vertices (across all layers combined), and in each layer the number of
// incoming edges from one vertex must equal the number of outgoing edges to
// the other vertex (in both directions).  Furthermore the vertex cannot have
// any degenerate edges in a given layer unless it has at least one
// non-degenerate edge in that layer as well.  (Note that usually there will
// not be any degenerate edges at all, since most layer types discard them.)
//
// The last condition is necessary to prevent the following: suppose that one
// layer has a chain ABC and another layer has a degenerate edge BB (with no
// other edges incident to B).  Then we can't simplify ABC to AC because there
// would be no suitable replacement for the edge BB (since the input edge that
// mapped to BB can't be replaced by any of the edges AA, AC, or CC without
// moving further than snap_radius).
class S2Builder::EdgeChainSimplifier::InteriorVertexMatcher {
 public:
  // Checks whether "v0" can be an interior vertex of an edge chain.
  explicit InteriorVertexMatcher(VertexId v0) : v0_(v0) {}

  // Starts analyzing the edges of a new layer.
  void StartLayer() {
    excess_out_ = n0_ = n1_ = n2_ = 0;
  }

  // This method should be called for each edge incident to "v0" in a given
  // layer.  (For degenerate edges, it should be called twice.)
  void Tally(VertexId v, bool outgoing) {
    excess_out_ += outgoing ? 1 : -1;  // outdegree - indegree
    if (v == v0_) {
      ++n0_;  // Counts both endpoints of each degenerate edge.
    } else {
      // We keep track of the total number of edges (incoming or outgoing)
      // connecting v0 to up to two adjacent vertices.
      if (v1_ < 0) v1_ = v;
      if (v1_ == v) {
        ++n1_;
      } else {
        if (v2_ < 0) v2_ = v;
        if (v2_ == v) {
          ++n2_;
        } else {
          too_many_endpoints_ = true;
        }
      }
    }
  }

  // This method should be called after processing the edges for each layer.
  // It returns true if "v0" is an interior vertex based on the edges so far.
  bool Matches() const {
    // We check that there are the same number of incoming and outgoing edges
    // in each direction by verifying that (1) indegree(v0) == outdegree(v0)
    // and (2) the total number of edges (incoming and outgoing) to "v1" and
    // "v2" are equal.  We also check the condition on degenerate edges that
    // is documented above.
    return (!too_many_endpoints_ && excess_out_ == 0 && n1_ == n2_ &&
            (n0_ == 0 || n1_ > 0));
  }

 private:
  VertexId v0_ = -1, v1_ = -1, v2_ = -1;
  int n0_ = 0, n1_ = 0, n2_ = 0;
  int excess_out_ = 0;  // outdegree(v0) - indegree(v0)
  // Have we seen more than two adjacent vertices?
  bool too_many_endpoints_ = false;
};

// Returns true if VertexId "v" can be an interior vertex of a simplified edge
// chain.  (See the InteriorVertexMatcher class for what this implies.)
bool S2Builder::EdgeChainSimplifier::IsInterior(VertexId v) {
  // Check a few simple prerequisites.
  if (out_.degree(v) == 0) return false;
  if (out_.degree(v) != in_.degree(v)) return false;
  if (builder_.is_forced(v)) return false;  // Keep forced vertices.

  // Sort the edges so that they are grouped by layer.
  vector<EdgeId>& edges = tmp_edges_;  // Avoid allocating each time.
  edges.clear();
  for (EdgeId e : out_.edge_ids(v)) edges.push_back(e);
  for (EdgeId e : in_.edge_ids(v)) edges.push_back(e);
  std::sort(edges.begin(), edges.end(), [this](EdgeId x, EdgeId y) {
      return graph_edge_layer(x) < graph_edge_layer(y);
    });
  // Now feed the edges in each layer to the InteriorVertexMatcher.
  InteriorVertexMatcher matcher(v);
  for (auto e = edges.begin(); e != edges.end(); ) {
    int layer = graph_edge_layer(*e);
    matcher.StartLayer();
    for (; e != edges.end() && graph_edge_layer(*e) == layer; ++e) {
      Edge edge = g_.edge(*e);
      if (edge.first == v) matcher.Tally(edge.second, true /*outgoing*/);
      if (edge.second == v) matcher.Tally(edge.first, false /*outgoing*/);
    }
    if (!matcher.Matches()) return false;
  }
  return true;
}

// Follows the edge chain starting with (v0, v1) until either we find a
// non-interior vertex or we return to the original vertex v0.  At each vertex
// we simplify a subchain of edges that is as long as possible.
void S2Builder::EdgeChainSimplifier::SimplifyChain(VertexId v0, VertexId v1) {
  // Avoid allocating "chain" each time by reusing it.
  vector<VertexId>& chain = tmp_vertices_;
  // Contains the set of vertices that have either been avoided or added to
  // the chain so far.  This is necessary so that AvoidSites() doesn't try to
  // avoid vertices that have already been added to the chain.
  flat_hash_set<VertexId>& used_vertices = tmp_vertex_set_;
  S2PolylineSimplifier simplifier;
  VertexId vstart = v0;
  bool done = false;
  do {
    // Simplify a subchain of edges starting with (v0, v1).
    chain.push_back(v0);
    used_vertices.insert(v0);
    simplifier.Init(g_.vertex(v0));
    // Note that if the first edge (v0, v1) is longer than the maximum length
    // allowed for simplification, then AvoidSites() will return false and we
    // exit the loop below after the first iteration.
    const bool simplify = AvoidSites(v0, v0, v1, &used_vertices, &simplifier);
    do {
      chain.push_back(v1);
      used_vertices.insert(v1);
      done = !is_interior_[v1] || v1 == vstart;
      if (done) break;

      // Attempt to extend the chain to the next vertex.
      VertexId vprev = v0;
      v0 = v1;
      v1 = FollowChain(vprev, v0);
    } while (simplify && TargetInputVertices(v0, &simplifier) &&
             AvoidSites(chain[0], v0, v1, &used_vertices, &simplifier) &&
             simplifier.Extend(g_.vertex(v1)));

    if (chain.size() == 2) {
      OutputAllEdges(chain[0], chain[1]);  // Could not simplify.
    } else {
      MergeChain(chain);
    }
    // Note that any degenerate edges that were not merged into a chain are
    // output by EdgeChainSimplifier::Run().
    chain.clear();
    used_vertices.clear();
  } while (!done);
}

// Given an edge (v0, v1) where v1 is an interior vertex, returns the (unique)
// next vertex in the edge chain.
S2Builder::Graph::VertexId S2Builder::EdgeChainSimplifier::FollowChain(
    VertexId v0, VertexId v1) const {
  ABSL_DCHECK(is_interior_[v1]);
  for (EdgeId e : out_.edge_ids(v1)) {
    VertexId v = g_.edge(e).second;
    if (v != v0 && v != v1) return v;
  }
  ABSL_LOG(FATAL) << "Could not find next edge in edge chain";
}

// Copies all input edges between v0 and v1 (in both directions) to the output.
void S2Builder::EdgeChainSimplifier::OutputAllEdges(VertexId v0, VertexId v1) {
  for (EdgeId e : out_.edge_ids(v0, v1)) OutputEdge(e);
  for (EdgeId e : out_.edge_ids(v1, v0)) OutputEdge(e);
}

// Ensures that the simplified edge passes within "edge_snap_radius" of all
// the *input* vertices that snapped to the given vertex "v".
bool S2Builder::EdgeChainSimplifier::TargetInputVertices(
    VertexId v, S2PolylineSimplifier* simplifier) const {
  for (InputVertexId i : site_vertices_[v]) {
    if (!simplifier->TargetDisc(builder_.input_vertices_[i],
                                builder_.edge_snap_radius_ca_)) {
      return false;
    }
  }
  return true;
}

// Given the starting vertex v0 and last edge (v1, v2) of an edge chain,
// restricts the allowable range of angles in order to ensure that all sites
// near the edge (v1, v2) are avoided by at least min_edge_vertex_separation.
bool S2Builder::EdgeChainSimplifier::AvoidSites(
    VertexId v0, VertexId v1, VertexId v2,
    flat_hash_set<VertexId>* used_vertices,
    S2PolylineSimplifier* simplifier) const {
  const S2Point& p0 = g_.vertex(v0);
  const S2Point& p1 = g_.vertex(v1);
  const S2Point& p2 = g_.vertex(v2);
  S1ChordAngle r1(p0, p1);
  S1ChordAngle r2(p0, p2);

  // The distance from the start of the edge chain must increase monotonically
  // for each vertex, since we don't want to simplify chains that backtrack on
  // themselves (we want a parametric approximation, not a geometric one).
  if (r2 < r1) return false;

  // We also limit the maximum edge length in order to guarantee that the
  // simplified edge stays with max_edge_deviation() of all the input edges
  // that snap to it.
  if (r2 >= builder_.min_edge_length_to_split_ca_) return false;

  // Otherwise it is sufficient to consider the nearby sites (edge_sites_) for
  // a single input edge that snapped to (v1, v2) or (v2, v1).  This is
  // because each edge has a list of all sites within (max_edge_deviation +
  // min_edge_vertex_separation), and since the output edge is within
  // max_edge_deviation of all input edges, this list includes all sites
  // within min_edge_vertex_separation of the output edge.
  //
  // Usually there is only one edge to choose from, but it's not much more
  // effort to choose the edge with the shortest list of edge_sites_.
  InputEdgeId best = -1;
  const auto& edge_sites = builder_.edge_sites_;
  for (EdgeId e : out_.edge_ids(v1, v2)) {
    for (InputEdgeId id : g_.input_edge_ids(e)) {
      if (best < 0 || edge_sites[id].size() < edge_sites[best].size())
        best = id;
    }
  }
  for (EdgeId e : out_.edge_ids(v2, v1)) {
    for (InputEdgeId id : g_.input_edge_ids(e)) {
      if (best < 0 || edge_sites[id].size() < edge_sites[best].size())
        best = id;
    }
  }
  ABSL_DCHECK_GE(best, 0);  // Because there is at least one outgoing edge.

  for (VertexId v : edge_sites[best]) {
    // Sites whose distance from "p0" is at least "r2" are not relevant yet.
    const S2Point& p = g_.vertex(v);
    S1ChordAngle r(p0, p);
    if (r >= r2) continue;

    // The following test prevents us from avoiding previous vertices of the
    // edge chain that also happen to be nearby the current edge.  (It also
    // happens to ensure that each vertex is avoided at most once, but this is
    // just an optimization.)
    if (!used_vertices->insert(v).second) continue;

    // We need to figure out whether this site is to the left or right of the
    // edge chain.  For the first edge this is easy.  Otherwise, since we are
    // only considering sites in the radius range (r1, r2), we can do this by
    // checking whether the site is to the left of the wedge (p0, p1, p2).
    bool disc_on_left = (v1 == v0) ? (s2pred::Sign(p1, p2, p) > 0)
                                   : s2pred::OrderedCCW(p0, p2, p, p1);
    if (!simplifier->AvoidDisc(p, builder_.min_edge_site_separation_ca_,
                               disc_on_left)) {
      return false;
    }
  }
  return true;
}

// Given the vertices in a simplified edge chain, adds the corresponding
// simplified edge(s) to the output.  Note that (1) the edge chain may exist
// in multiple layers, (2) the edge chain may exist in both directions, (3)
// there may be more than one copy of an edge chain (in either direction)
// within a single layer.
void S2Builder::EdgeChainSimplifier::MergeChain(
    absl::Span<const VertexId> vertices) {
  // Suppose that all interior vertices have M outgoing edges and N incoming
  // edges.  Our goal is to group the edges into M outgoing chains and N
  // incoming chains, and then replace each chain by a single edge.
  vector<vector<InputEdgeId>> merged_input_ids;
  vector<InputEdgeId> degenerate_ids;
  int num_out;  // Edge count in the outgoing direction.
  for (size_t i = 1; i < vertices.size(); ++i) {
    VertexId v0 = vertices[i-1];
    VertexId v1 = vertices[i];
    auto out_edges = out_.edge_ids(v0, v1);
    auto in_edges = out_.edge_ids(v1, v0);
    if (i == 1) {
      // Allocate space to store the input edge ids associated with each edge.
      num_out = out_edges.size();
      merged_input_ids.resize(num_out + in_edges.size());
      for (vector<InputEdgeId>& ids : merged_input_ids) {
        ids.reserve(vertices.size() - 1);
      }
    } else {
      // For each interior vertex, we build a list of input edge ids
      // associated with degenerate edges.  Each input edge ids will be
      // assigned to one of the output edges later.  (Normally there are no
      // degenerate edges at all since most layer types don't want them.)
      ABSL_DCHECK(is_interior_[v0]);
      for (EdgeId e : out_.edge_ids(v0, v0)) {
        for (InputEdgeId id : g_.input_edge_ids(e)) {
          degenerate_ids.push_back(id);
        }
        used_[e] = true;
      }
    }
    // Because the edges were created in layer order, and all sorts used are
    // stable, the edges are still in layer order.  Therefore we can simply
    // merge together all the edges in the same relative position.
    int j = 0;
    for (EdgeId e : out_edges) {
      for (InputEdgeId id : g_.input_edge_ids(e)) {
        merged_input_ids[j].push_back(id);
      }
      used_[e] = true;
      ++j;
    }
    for (EdgeId e : in_edges) {
      for (InputEdgeId id : g_.input_edge_ids(e)) {
        merged_input_ids[j].push_back(id);
      }
      used_[e] = true;
      ++j;
    }
    ABSL_DCHECK_EQ(merged_input_ids.size(), j);
  }
  if (!degenerate_ids.empty()) {
    std::sort(degenerate_ids.begin(), degenerate_ids.end());
    AssignDegenerateEdges(degenerate_ids, &merged_input_ids);
  }
  // Output the merged edges.
  VertexId v0 = vertices[0], v1 = vertices[1], vb = vertices.back();
  for (EdgeId e : out_.edge_ids(v0, v1)) {
    new_edges_.push_back(Edge(v0, vb));
    new_edge_layers_.push_back(graph_edge_layer(e));
  }
  for (EdgeId e : out_.edge_ids(v1, v0)) {
    new_edges_.push_back(Edge(vb, v0));
    new_edge_layers_.push_back(graph_edge_layer(e));
  }
  for (const auto& ids : merged_input_ids) {
    new_input_edge_ids_.push_back(input_edge_id_set_lexicon_->Add(ids));
  }
}

// Given a list of the input edge ids associated with degenerate edges in the
// interior of an edge chain, assigns each input edge id to one of the output
// edges.
void S2Builder::EdgeChainSimplifier::AssignDegenerateEdges(
    absl::Span<const InputEdgeId> degenerate_ids,
    vector<vector<InputEdgeId>>* merged_ids) const {
  // Each degenerate edge is assigned to an output edge in the appropriate
  // layer.  If there is more than one candidate, we use heuristics so that if
  // the input consists of a chain of edges provided in consecutive order
  // (some of which became degenerate), then all those input edges are
  // assigned to the same output edge.  For example, suppose that one output
  // edge is labeled with input edges 3,4,7,8, while another output edge is
  // labeled with input edges 50,51,54,57.  Then if we encounter degenerate
  // edges labeled with input edges 5 and 6, we would prefer to assign them to
  // the first edge (yielding the continuous range 3,4,5,6,7,8).
  //
  // The heuristic below is only smart enough to handle the case where the
  // candidate output edges have non-overlapping ranges of input edges.
  // (Otherwise there is probably not a good heuristic for assigning the
  // degenerate edges in any case.)

  // Duplicate edge ids don't affect the heuristic below, so we don't bother
  // removing them.  (They will be removed by IdSetLexicon::Add.)
  for (auto& ids : *merged_ids) std::sort(ids.begin(), ids.end());

  // Sort the output edges by their minimum input edge id.  This is sufficient
  // for the purpose of determining which layer they belong to.  With
  // EdgeType::UNDIRECTED, some edges might not have any input edge ids (i.e.,
  // if they consist entirely of siblings of input edges).  We simply remove
  // such edges from the lists of candidates.
  vector<unsigned> order;
  order.reserve(merged_ids->size());
  for (size_t i = 0; i < merged_ids->size(); ++i) {
    if (!(*merged_ids)[i].empty()) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&merged_ids](int i, int j) {
      return (*merged_ids)[i][0] < (*merged_ids)[j][0];
    });

  // Now determine where each degenerate edge should be assigned.
  for (InputEdgeId degenerate_id : degenerate_ids) {
    int layer = input_edge_layer(degenerate_id);

    // Find the first output edge whose range of input edge ids starts after
    // "degenerate_id".  If the previous edge belongs to the correct layer,
    // then we assign the degenerate edge to it.
    auto it = std::upper_bound(order.begin(), order.end(), degenerate_id,
                               [&merged_ids](InputEdgeId x, unsigned y) {
                                 return x < (*merged_ids)[y][0];
                               });
    if (it != order.begin()) {
      if ((*merged_ids)[it[-1]][0] >= layer_begins_[layer]) --it;
    }
    ABSL_DCHECK_EQ(layer, input_edge_layer((*merged_ids)[it[0]][0]));
    (*merged_ids)[it[0]].push_back(degenerate_id);
  }
}

/////////////////////// S2Builder::MemoryTracker /////////////////////////


// Called to track memory used to store the set of sites near a given edge.
bool S2Builder::MemoryTracker::TallyEdgeSites(
    const compact_array<SiteId>& sites) {
  int64_t size = GetCompactArrayAllocBytes(sites);
  edge_sites_bytes_ += size;
  return Tally(size);
}

// Ensures that "sites" contains space for at least one more edge site.
bool S2Builder::MemoryTracker::ReserveEdgeSite(compact_array<SiteId>* sites) {
  int64_t new_size = sites->size() + 1;
  if (new_size <= sites->capacity()) return true;
  int64_t old_bytes = GetCompactArrayAllocBytes(*sites);
  sites->reserve(new_size);
  int64_t added_bytes = GetCompactArrayAllocBytes(*sites) - old_bytes;
  edge_sites_bytes_ += added_bytes;
  return Tally(added_bytes);
}

// Releases and tracks the memory used to store nearby edge sites.
bool S2Builder::MemoryTracker::ClearEdgeSites(
    vector<compact_array<SiteId>>* edge_sites) {
  Tally(-edge_sites_bytes_);
  edge_sites_bytes_ = 0;
  return Clear(edge_sites);
}

// Called when a site is added to the S2PointIndex.
bool S2Builder::MemoryTracker::TallyIndexedSite() {
  // S2PointIndex stores its data in a btree.  In general btree nodes are only
  // guaranteed to be half full, but in our case all nodes are full except for
  // the rightmost node at each btree level because the values are added in
  // sorted order.
  int64_t delta_bytes = GetBtreeMinBytesPerEntry<
      absl::btree_multimap<S2CellId, S2PointIndex<SiteId>::PointData>>();
  site_index_bytes_ += delta_bytes;
  return Tally(delta_bytes);
}

// Corrects the approximate S2PointIndex memory tracking done above.
bool S2Builder::MemoryTracker::FixSiteIndexTally(
    const S2PointIndex<SiteId>& index) {
  int64_t delta_bytes = index.SpaceUsed() - site_index_bytes_;
  site_index_bytes_ += delta_bytes;
  return Tally(delta_bytes);
}

// Tracks memory due to destroying the site index.
bool S2Builder::MemoryTracker::DoneSiteIndex(
    const S2PointIndex<SiteId>& index) {
  Tally(-site_index_bytes_);
  site_index_bytes_ = 0;
  return ok();
}

// Called to indicate that edge simplification was requested.
// LINT.IfChange(TallySimplifyEdgeChains)
bool S2Builder::MemoryTracker::TallySimplifyEdgeChains(
    absl::Span<const compact_array<InputVertexId>> site_vertices,
    absl::Span<const vector<Edge>> layer_edges) {
  if (!is_active()) return true;

  // The simplify_edge_chains() option uses temporary memory per site
  // (output vertex) and per output edge, as outlined below.
  //
  // Per site:
  //  vector<compact_array<InputVertexId>> site_vertices;  // BuildLayerEdges
  //   - compact_array non-inlined space is tallied separately
  //  vector<bool> is_interior_;  // EdgeChainSimplifier
  //  Graph::VertexInMap in_;     // EdgeChainSimplifier
  //  Graph::VertexOutMap out_;   // EdgeChainSimplifier
  const int64_t kTempPerSite =
      sizeof(compact_array<InputVertexId>) + sizeof(bool) + 2 * sizeof(EdgeId);

  // Per output edge:
  //  vector<bool> used_;                              // EdgeChainSimplifier
  //  Graph::VertexInMap in_;                          // EdgeChainSimplifier
  //  vector<Edge> merged_edges;                       // SimplifyEdgeChains
  //  vector<InputEdgeIdSetId> merged_input_edge_ids;  // SimplifyEdgeChains
  //  vector<int> merged_edge_layers;                  // SimplifyEdgeChains
  //  vector<Edge> new_edges_;                         // EdgeChainSimplifier
  //  vector<InputEdgeIdSetId> new_input_edge_ids_;    // EdgeChainSimplifier
  //  vector<int> new_edge_layers_;                    // EdgeChainSimplifier
  //
  // Note that the temporary vector<LayerEdgeId> in MergeLayerEdges() does not
  // affect peak usage.
  const int64_t kTempPerEdge = sizeof(bool) + sizeof(EdgeId) +
                               2 * sizeof(Edge) + 2 * sizeof(InputEdgeIdSetId) +
                               2 * sizeof(int);
  int64_t simplify_bytes = site_vertices.size() * kTempPerSite;
  for (const auto& array : site_vertices) {
    simplify_bytes += GetCompactArrayAllocBytes(array);
  }
  for (const auto& edges : layer_edges) {
    simplify_bytes += edges.size() * kTempPerEdge;
  }
  return TallyTemp(simplify_bytes);
}
// LINT.ThenChange()

// Tracks the temporary memory used by Graph::FilterVertices.
// LINT.IfChange(TallyFilterVertices)
bool S2Builder::MemoryTracker::TallyFilterVertices(
    int num_sites, absl::Span<const vector<Edge>> layer_edges) {
  if (!is_active()) return true;

  // Vertex filtering (see BuildLayers) uses temporary space of one VertexId
  // per Voronoi site plus 2 VertexIds per layer edge, plus space for all the
  // vertices after filtering.
  //
  //  vector<VertexId> *tmp;      // Graph::FilterVertices
  //  vector<VertexId> used;      // Graph::FilterVertices
  const int64_t kTempPerSite = sizeof(Graph::VertexId);
  const int64_t kTempPerEdge = 2 * sizeof(Graph::VertexId);

  size_t max_layer_edges = 0;
  for (const auto& edges : layer_edges) {
    max_layer_edges = max(max_layer_edges, edges.size());
  }
  filter_vertices_bytes_ = (num_sites * kTempPerSite +
                            max_layer_edges * kTempPerEdge);
  return Tally(filter_vertices_bytes_);
}
// LINT.ThenChange()

bool S2Builder::MemoryTracker::DoneFilterVertices() {
  Tally(-filter_vertices_bytes_);
  filter_vertices_bytes_ = 0;
  return ok();
}
