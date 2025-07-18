// Copyright 2017 Google Inc. All Rights Reserved.
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

#ifndef S2_S2BUILDERUTIL_TESTING_H_
#define S2_S2BUILDERUTIL_TESTING_H_

#include <memory>
#include <string>
#include <vector>

#include "s2/_fp_contract_off.h"  // IWYU pragma: keep
#include "s2/id_set_lexicon.h"
#include "s2/s2builder.h"
#include "s2/s2builder_graph.h"
#include "s2/s2builder_layer.h"
#include "s2/s2error.h"
#include "s2/s2point.h"
#include "s2/s2shape.h"
#include "s2/s2shape_index.h"

namespace s2builderutil {

// A class that copies an S2Builder::Graph and owns the underlying data
// (unlike S2Builder::Graph, which is just a view).
class GraphClone {
 public:
  GraphClone() = default;  // Must call Init().
  explicit GraphClone(const S2Builder::Graph& g) { Init(g); }
  void Init(const S2Builder::Graph& g);
  const S2Builder::Graph& graph() { return g_; }

 private:
  S2Builder::GraphOptions options_;
  std::vector<S2Point> vertices_;
  std::vector<S2Builder::Graph::Edge> edges_;
  std::vector<S2Builder::Graph::InputEdgeIdSetId> input_edge_id_set_ids_;
  IdSetLexicon input_edge_id_set_lexicon_;
  std::vector<S2Builder::Graph::LabelSetId> label_set_ids_;
  IdSetLexicon label_set_lexicon_;
  S2Builder::IsFullPolygonPredicate is_full_polygon_predicate_;
  S2Builder::Graph g_;
};

// A layer type that copies an S2Builder::Graph into a GraphClone object
// (which owns the underlying data, unlike S2Builder::Graph itself).
class GraphCloningLayer : public S2Builder::Layer {
 public:
  GraphCloningLayer(const S2Builder::GraphOptions& graph_options,
                    GraphClone* gc)
      : graph_options_(graph_options), gc_(gc) {}

  S2Builder::GraphOptions graph_options() const override {
    return graph_options_;
  }

  void Build(const S2Builder::Graph& g, S2Error* error) override {
    gc_->Init(g);
  }

 private:
  GraphOptions graph_options_;
  GraphClone* gc_;
};

// A layer type that copies an S2Builder::Graph and appends it to a vector,
// and appends the corresponding GraphClone object (which owns the Graph data)
// to a separate vector.
class GraphAppendingLayer : public S2Builder::Layer {
 public:
  GraphAppendingLayer(
      const S2Builder::GraphOptions& graph_options,
      std::vector<S2Builder::Graph>* graphs,
      std::vector<std::unique_ptr<GraphClone>>* clones)
      : graph_options_(graph_options), graphs_(graphs), clones_(clones) {}

  S2Builder::GraphOptions graph_options() const override {
    return graph_options_;
  }

  void Build(const S2Builder::Graph& g, S2Error* error) override {
    clones_->push_back(std::make_unique<GraphClone>(g));
    graphs_->push_back(clones_->back()->graph());
  }

 private:
  GraphOptions graph_options_;
  std::vector<S2Builder::Graph>* graphs_;
  std::vector<std::unique_ptr<GraphClone>>* clones_;
};

// A layer type that expects that the edges in the S2Builder::Graph passed to
// its Build() method should match the edges in the given S2ShapeIndex
// (including multiplicities).  This allows testing whether an algorithm
// produces a given multiset of edges without needing to specify a particular
// ordering of those edges.
class IndexMatchingLayer : public S2Builder::Layer {
 public:
  // Tests whether the edges passed to its Build() method match the edges in
  // the given S2ShapeIndex (including multiplicities).  If any differences
  // are found, sets "error" to a descriptive error message.
  //
  // If "dimension" is non-negative then only shapes of the given dimension
  // are used.  (This allows for use with classes such as S2BooleanOperation
  // that output one S2Builder::Graph for each dimension.)
  explicit IndexMatchingLayer(const S2Builder::GraphOptions& graph_options,
                              const S2ShapeIndex* index, int dimension = -1)
      : graph_options_(graph_options), index_(*index), dimension_(dimension) {
  }

  // S2Builder interface:
  GraphOptions graph_options() const override {
    return graph_options_;
  }

  void Build(const Graph& g, S2Error* error) override;

 private:
  using EdgeVector = std::vector<S2Shape::Edge>;
  static std::string ToString(const EdgeVector& edges);

  GraphOptions graph_options_;
  const S2ShapeIndex& index_;
  int dimension_;
};

}  // namespace s2builderutil

#endif  // S2_S2BUILDERUTIL_TESTING_H_
