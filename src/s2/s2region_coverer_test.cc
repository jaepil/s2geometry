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

#include "s2/s2region_coverer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/log/log_streamer.h"
#include "absl/random/random.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"

#include "s2/base/commandlineflags.h"
#include "s2/base/log_severity.h"
#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
#include "s2/s2cap.h"
#include "s2/s2cell.h"
#include "s2/s2cell_id.h"
#include "s2/s2cell_union.h"
#include "s2/s2latlng.h"
#include "s2/s2point.h"
#include "s2/s2polyline.h"
#include "s2/s2random.h"
#include "s2/s2region.h"
#include "s2/s2testing.h"

S2_DEFINE_string(max_cells, "4,8",
              "Comma-separated list of values to use for 'max_cells'");

S2_DEFINE_int32(iters, S2_DEBUG_MODE ? 1000 : 100000,
                "Number of random caps to try for each max_cells value");

namespace {

using absl::flat_hash_map;
using absl::StrCat;
using std::max;
using std::min;
using std::priority_queue;
using std::string;
using std::vector;

TEST(S2RegionCoverer, RandomCells) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "RANDOM_CELLS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));

  S2RegionCoverer::Options options;
  options.set_max_cells(1);
  S2RegionCoverer coverer(options);

  // Test random cell ids at all levels.
  for (int i = 0; i < 10000; ++i) {
    S2CellId id = s2random::CellId(bitgen);
    SCOPED_TRACE(StrCat("Iteration ", i, ", cell ID token ", id.ToToken()));
    vector<S2CellId> covering = coverer.GetCovering(S2Cell(id)).Release();
    EXPECT_EQ(1, covering.size());
    EXPECT_EQ(id, covering[0]);
  }
}

static void CheckCovering(const S2RegionCoverer::Options& options,
                          const S2Region& region,
                          const vector<S2CellId>& covering,
                          bool interior) {
  // Keep track of how many cells have the same options.min_level() ancestor.
  flat_hash_map<S2CellId, int> min_level_cells;
  for (S2CellId cell_id : covering) {
    int level = cell_id.level();
    EXPECT_GE(level, options.min_level());
    EXPECT_LE(level, options.max_level());
    EXPECT_EQ((level - options.min_level()) % options.level_mod(), 0);
    min_level_cells[cell_id.parent(options.min_level())] += 1;
  }
  if (covering.size() > options.max_cells()) {
    // If the covering has more than the requested number of cells, then check
    // that the cell count cannot be reduced by using the parent of some cell.
    for (const auto [_, cells] : min_level_cells) {
      EXPECT_EQ(cells, 1);
    }
  }
  if (interior) {
    for (S2CellId cell_id : covering) {
      EXPECT_TRUE(region.Contains(S2Cell(cell_id)));
    }
  } else {
    S2CellUnion cell_union(covering);
    S2Testing::CheckCovering(region, cell_union, true);
  }
}

TEST(S2RegionCoverer, RandomCaps) {
  absl::BitGen bitgen(
      S2Testing::MakeTaggedSeedSeq("RANDOM_CAPS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  static constexpr int kMaxLevel = S2CellId::kMaxLevel;
  S2RegionCoverer::Options options;
  for (int i = 0; i < 1000; ++i) {
    options.set_min_level(
        absl::Uniform(absl::IntervalClosedClosed, bitgen, 0, kMaxLevel));
    options.set_max_level(absl::Uniform(absl::IntervalClosedClosed, bitgen,
                                        options.min_level(), kMaxLevel));
    options.set_max_cells(s2random::SkewedInt(bitgen, 10));
    options.set_level_mod(absl::Uniform(bitgen, 1, 4));
    double max_area =  min(4 * M_PI, (3 * options.max_cells() + 1) *
                           S2Cell::AverageArea(options.min_level()));
    S2Cap cap =
        s2random::Cap(bitgen, 0.1 * S2Cell::AverageArea(kMaxLevel), max_area);
    S2RegionCoverer coverer(options);
    vector<S2CellId> covering, interior;
    coverer.GetCovering(cap, &covering);
    CheckCovering(options, cap, covering, false);
    coverer.GetInteriorCovering(cap, &interior);
    CheckCovering(options, cap, interior, true);

    // Check that GetCovering is deterministic.
    vector<S2CellId> covering2;
    coverer.GetCovering(cap, &covering2);
    EXPECT_EQ(covering, covering2);

    // Also check S2CellUnion::Denormalize().  The denormalized covering
    // may still be different and smaller than "covering" because
    // S2RegionCoverer does not guarantee that it will not output all four
    // children of the same parent.
    S2CellUnion cells(covering);
    vector<S2CellId> denormalized;
    cells.Denormalize(options.min_level(), options.level_mod(), &denormalized);
    CheckCovering(options, cap, denormalized, false);
  }
}

TEST(S2RegionCoverer, SimpleCoverings) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "SIMPLE_COVERINGS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  static constexpr int kMaxLevel = S2CellId::kMaxLevel;
  S2RegionCoverer::Options options;
  options.set_max_cells(std::numeric_limits<int32_t>::max());
  for (int i = 0; i < 1000; ++i) {
    int level = absl::Uniform(bitgen, 0, kMaxLevel + 1);
    options.set_min_level(level);
    options.set_max_level(level);
    double max_area =  min(4 * M_PI, 1000 * S2Cell::AverageArea(level));
    S2Cap cap =
        s2random::Cap(bitgen, 0.1 * S2Cell::AverageArea(kMaxLevel), max_area);
    vector<S2CellId> covering;
    S2RegionCoverer::GetSimpleCovering(cap, cap.center(), level, &covering);
    CheckCovering(options, cap, covering, false);
  }
}

// We keep a priority queue of the caps that had the worst approximation
// ratios so that we can print them at the end.
struct WorstCap {
  double ratio;
  S2Cap cap;
  int num_cells;
  bool operator<(const WorstCap& o) const { return ratio > o.ratio; }
  WorstCap(double r, const S2Cap& c, int n) : ratio(r), cap(c), num_cells(n) {}
};

static void TestAccuracy(int max_cells) {
  SCOPED_TRACE(StrCat(max_cells, " cells"));
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "TEST_ACCURACY", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));

  static constexpr int kNumMethods = 1;
  // This code is designed to evaluate several approximation algorithms and
  // figure out which one works better.  The way to do this is to hack the
  // S2RegionCoverer interface to add a global variable to control which
  // algorithm (or variant of an algorithm) is selected, and then assign to
  // this variable in the "method" loop below.  The code below will then
  // collect statistics on all methods, including how often each one wins in
  // terms of cell count and approximation area.

  S2RegionCoverer coverer;
  coverer.mutable_options()->set_max_cells(max_cells);

  double ratio_total[kNumMethods] = {0};
  double min_ratio[kNumMethods];  // initialized in loop below
  double max_ratio[kNumMethods] = {0};
  vector<double> ratios[kNumMethods];
  int cell_total[kNumMethods] = {0};
  int area_winner_tally[kNumMethods] = {0};
  int cell_winner_tally[kNumMethods] = {0};
  static constexpr int kMaxWorstCaps = 10;
  priority_queue<WorstCap> worst_caps[kNumMethods];

  for (int method = 0; method < kNumMethods; ++method) {
    min_ratio[method] = 1e20;
  }
  for (int i = 0; i < absl::GetFlag(FLAGS_iters); ++i) {
    // Choose the log of the cap area to be uniformly distributed over
    // the allowable range.  Don't try to approximate regions that are so
    // small they can't use the given maximum number of cells efficiently.
    const double min_cap_area = S2Cell::AverageArea(S2CellId::kMaxLevel)
                                * max_cells * max_cells;
    // Coverings for huge caps are not interesting, so limit the max area too.
    S2Cap cap = s2random::Cap(bitgen, min_cap_area, 0.1 * M_PI);
    double cap_area = cap.GetArea();

    double min_area = 1e30;
    int min_cells = 1 << 30;
    double area[kNumMethods];
    int cells[kNumMethods];
    for (int method = 0; method < kNumMethods; ++method) {
      // If you want to play with different methods, do this:
      // S2RegionCoverer::method_number = method;

      vector<S2CellId> covering;
      coverer.GetCovering(cap, &covering);

      double union_area = 0;
      for (S2CellId cell_id : covering) {
        union_area += S2Cell(cell_id).ExactArea();
      }
      cells[method] = covering.size();
      min_cells = min(cells[method], min_cells);
      area[method] = union_area;
      min_area = min(area[method], min_area);
      cell_total[method] += cells[method];
      double ratio = area[method] / cap_area;
      ratio_total[method] += ratio;
      min_ratio[method] = min(ratio, min_ratio[method]);
      max_ratio[method] = max(ratio, max_ratio[method]);
      ratios[method].push_back(ratio);
      if (worst_caps[method].size() < kMaxWorstCaps) {
        worst_caps[method].push(WorstCap(ratio, cap, cells[method]));
      } else if (ratio > worst_caps[method].top().ratio) {
        worst_caps[method].pop();
        worst_caps[method].push(WorstCap(ratio, cap, cells[method]));
      }
    }
    for (int method = 0; method < kNumMethods; ++method) {
      if (area[method] == min_area) ++area_winner_tally[method];
      if (cells[method] == min_cells) ++cell_winner_tally[method];
    }
  }
  for (int method = 0; method < kNumMethods; ++method) {
    absl::PrintF("\nMax cells %d, method %d:\n", max_cells, method);
    absl::PrintF(
        "  Average cells: %.4f\n",
        cell_total[method] / static_cast<double>(absl::GetFlag(FLAGS_iters)));
    absl::PrintF("  Average area ratio: %.4f\n",
                 ratio_total[method] / absl::GetFlag(FLAGS_iters));
    vector<double>& mratios = ratios[method];
    std::sort(mratios.begin(), mratios.end());
    absl::PrintF("  Median ratio: %.4f\n", mratios[mratios.size() / 2]);
    absl::PrintF("  Max ratio: %.4f\n", max_ratio[method]);
    absl::PrintF("  Min ratio: %.4f\n", min_ratio[method]);
    if (kNumMethods > 1) {
      absl::PrintF("  Cell winner probability: %.4f\n",
                   cell_winner_tally[method] /
                       static_cast<double>(absl::GetFlag(FLAGS_iters)));
      absl::PrintF("  Area winner probability: %.4f\n",
                   area_winner_tally[method] /
                       static_cast<double>(absl::GetFlag(FLAGS_iters)));
    }
    absl::PrintF("  Caps with the worst approximation ratios:\n");
    for (; !worst_caps[method].empty(); worst_caps[method].pop()) {
      const WorstCap& w = worst_caps[method].top();
      S2LatLng ll(w.cap.center());
      absl::PrintF(
          "    Ratio %.4f, Cells %d, "
          "Center (%.8f, %.8f), Km %.6f\n",
          w.ratio, w.num_cells, ll.lat().degrees(), ll.lng().degrees(),
          w.cap.GetRadius().radians() * 6367.0);
    }
  }
}

TEST(S2RegionCoverer, Accuracy) {
  for (auto max_cells_str :
       absl::StrSplit(absl::GetFlag(FLAGS_max_cells), ',', absl::SkipEmpty())) {
    int max_cells;
    ABSL_CHECK(absl::SimpleAtoi(max_cells_str, &max_cells));
    TestAccuracy(max_cells);
  }
}

TEST(S2RegionCoverer, InteriorCovering) {
  // We construct the region the following way. Start with S2 cell of level l.
  // Remove from it one of its grandchildren (level l+2). If we then set
  //   min_level < l + 1
  //   max_level > l + 2
  //   max_cells = 3
  // the best interior covering should contain 3 children of the initial cell,
  // that were not effected by removal of a grandchild.
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "INTERIOR_COVERING", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int level = 12;
  S2CellId small_cell = S2CellId(s2random::Point(bitgen)).parent(level + 2);
  S2CellId large_cell = small_cell.parent(level);
  S2CellUnion diff =
      S2CellUnion({large_cell}).Difference(S2CellUnion({small_cell}));
  S2RegionCoverer::Options options;
  options.set_max_cells(3);
  options.set_max_level(level + 3);
  options.set_min_level(level);
  S2RegionCoverer coverer(options);
  vector<S2CellId> interior;
  coverer.GetInteriorCovering(diff, &interior);
  ASSERT_EQ(interior.size(), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(interior[i].level(), level + 1);
  }
}

TEST(GetFastCovering, HugeFixedLevelCovering) {
  // Test a "fast covering" with a huge number of cells due to min_level().
  S2RegionCoverer::Options options;
  options.set_min_level(10);
  S2RegionCoverer coverer(options);
  vector<S2CellId> covering;
  S2Cell region(S2CellId::FromDebugString("1/23"));
  coverer.GetFastCovering(region, &covering);
  EXPECT_GE(covering.size(), 1 << 16);
}

bool IsCanonical(absl::Span<const string> input_str,
                 const S2RegionCoverer::Options& options) {
  vector<S2CellId> input;
  for (const auto& str : input_str) {
    input.push_back(S2CellId::FromDebugString(str));
  }
  S2RegionCoverer coverer(options);
  return coverer.IsCanonical(input);
}

TEST(IsCanonical, InvalidS2CellId) {
  EXPECT_TRUE(IsCanonical({"1/"}, S2RegionCoverer::Options()));
  EXPECT_FALSE(IsCanonical({"invalid"}, S2RegionCoverer::Options()));
}

TEST(IsCanonical, Unsorted) {
  EXPECT_TRUE(IsCanonical({"1/1", "1/3"}, S2RegionCoverer::Options()));
  EXPECT_FALSE(IsCanonical({"1/3", "1/1"}, S2RegionCoverer::Options()));
}

TEST(IsCanonical, Overlapping) {
  EXPECT_TRUE(IsCanonical({"1/2", "1/33"}, S2RegionCoverer::Options()));
  EXPECT_FALSE(IsCanonical({"1/3", "1/33"}, S2RegionCoverer::Options()));
}

TEST(IsCanonical, MinLevel) {
  S2RegionCoverer::Options options;
  options.set_min_level(2);
  EXPECT_TRUE(IsCanonical({"1/31"}, options));
  EXPECT_FALSE(IsCanonical({"1/3"}, options));
}

TEST(IsCanonical, MaxLevel) {
  S2RegionCoverer::Options options;
  options.set_max_level(2);
  EXPECT_TRUE(IsCanonical({"1/31"}, options));
  EXPECT_FALSE(IsCanonical({"1/312"}, options));
}

TEST(IsCanonical, LevelMod) {
  S2RegionCoverer::Options options;
  options.set_level_mod(2);
  EXPECT_TRUE(IsCanonical({"1/31"}, options));
  EXPECT_FALSE(IsCanonical({"1/312"}, options));
}

TEST(IsCanonical, MaxCells) {
  S2RegionCoverer::Options options;
  options.set_max_cells(2);
  EXPECT_TRUE(IsCanonical({"1/1", "1/3"}, options));
  EXPECT_FALSE(IsCanonical({"1/1", "1/3", "2/"}, options));
  EXPECT_TRUE(IsCanonical({"1/123", "2/1", "3/0122"}, options));
}

TEST(IsCanonical, Normalized) {
  // Test that no sequence of cells could be replaced by an ancestor.
  S2RegionCoverer::Options options;
  EXPECT_TRUE(IsCanonical({"1/01", "1/02", "1/03", "1/10", "1/11"}, options));
  EXPECT_FALSE(IsCanonical({"1/00", "1/01", "1/02", "1/03", "1/10"}, options));

  EXPECT_TRUE(IsCanonical({"0/22", "1/01", "1/02", "1/03", "1/10"}, options));
  EXPECT_FALSE(IsCanonical({"0/22", "1/00", "1/01", "1/02", "1/03"}, options));

  options.set_max_cells(20);
  options.set_level_mod(2);
  EXPECT_TRUE(IsCanonical(
      {"1/1101", "1/1102", "1/1103", "1/1110",
       "1/1111", "1/1112", "1/1113", "1/1120",
       "1/1121", "1/1122", "1/1123", "1/1130",
       "1/1131", "1/1132", "1/1133", "1/1200"}, options));
  EXPECT_FALSE(IsCanonical(
      {"1/1100", "1/1101", "1/1102", "1/1103",
       "1/1110", "1/1111", "1/1112", "1/1113",
       "1/1120", "1/1121", "1/1122", "1/1123",
       "1/1130", "1/1131", "1/1132", "1/1133"}, options));
}

void TestCanonicalizeCovering(absl::Span<const string> input_str,
                              absl::Span<const string> expected_str,
                              const S2RegionCoverer::Options& options,
                              const bool test_cell_union = true) {
  vector<S2CellId> actual, expected;
  for (const auto& str : input_str) {
    actual.push_back(S2CellId::FromDebugString(str));
  }
  for (const auto& str : expected_str) {
    expected.push_back(S2CellId::FromDebugString(str));
  }
  S2RegionCoverer coverer(options);
  EXPECT_FALSE(coverer.IsCanonical(actual));

  if (test_cell_union) {
    // Test version taking and returning an `S2CellUnion`; this must be done
    // first, since we use `actual` here and the other version modifies its
    // argument.
    const S2CellUnion input_union(actual);
    // Non-canonical input may become canonical after (or vice versa) after
    // converting to S2CellUnion, so don't test whether or not the input is
    // canonical.
    const S2CellUnion actual_union = coverer.CanonicalizeCovering(input_union);
    EXPECT_EQ(expected, actual_union.cell_ids());
    EXPECT_TRUE(coverer.IsCanonical(actual_union.cell_ids()));
    EXPECT_TRUE(coverer.IsCanonical(actual_union));
    // `actual` didn't change.
    EXPECT_FALSE(coverer.IsCanonical(actual));
  }

  // Test modifying version.
  coverer.CanonicalizeCovering(&actual);
  EXPECT_TRUE(coverer.IsCanonical(actual));
  vector<string> actual_str;
  EXPECT_EQ(expected, actual);
}

TEST(CanonicalizeCovering, UnsortedDuplicateCells) {
  S2RegionCoverer::Options options;
  TestCanonicalizeCovering({"1/200", "1/13122", "1/20", "1/131", "1/13100"},
                           {"1/131", "1/20"}, options);
}

TEST(CanonicalizeCovering, MaxLevelExceeded) {
  S2RegionCoverer::Options options;
  options.set_max_level(2);
  TestCanonicalizeCovering({"0/3001", "0/3002", "4/012301230123"},
                           {"0/30", "4/01"}, options);
}

TEST(CanonicalizeCovering, WrongLevelMod) {
  S2RegionCoverer::Options options;
  options.set_min_level(1);
  options.set_level_mod(3);
  TestCanonicalizeCovering({"0/0", "1/11", "2/222", "3/3333"},
                           {"0/0", "1/1", "2/2", "3/3333"}, options);
}

TEST(CanonicalizeCovering, ReplacedByParent) {
  // Test that 16 children are replaced by their parent when level_mod == 2.
  S2RegionCoverer::Options options;
  options.set_level_mod(2);
  TestCanonicalizeCovering(
      {"0/00", "0/01", "0/02", "0/03", "0/10", "0/11", "0/12", "0/13",
       "0/20", "0/21", "0/22", "0/23", "0/30", "0/31", "0/32", "0/33"},
      {"0/"}, options);
}

TEST(CanonicalizeCovering, DenormalizedCellUnion) {
  // Test that all 4 children of a cell may be used when this is necessary to
  // satisfy min_level() or level_mod();
  S2RegionCoverer::Options options;
  options.set_min_level(1);
  options.set_level_mod(2);
  TestCanonicalizeCovering(
      {"0/", "1/130", "1/131", "1/132", "1/133"},
      {"0/0", "0/1", "0/2", "0/3", "1/130", "1/131", "1/132", "1/133"}, options,
      // Denormalized input will be changed by the `S2CellUnion` variants,
      // so don't test it.
      /*test_cell_union=*/false);
}

TEST(CanonicalizeCovering, MaxCellsMergesSmallest) {
  // When there are too many cells, the smallest cells should be merged first.
  S2RegionCoverer::Options options;
  options.set_max_cells(3);
  TestCanonicalizeCovering(
      {"0/", "1/0", "1/1", "2/01300", "2/0131313"},
      {"0/", "1/", "2/013"}, options);
}

TEST(CanonicalizeCovering, MaxCellsMergesRepeatedly) {
  // Check that when merging creates a cell when all 4 children are present,
  // those cells are merged into their parent (repeatedly if necessary).
  S2RegionCoverer::Options options;
  options.set_max_cells(8);
  TestCanonicalizeCovering(
      {"0/0121", "0/0123", "1/0", "1/1", "1/2", "1/30", "1/32", "1/33",
       "1/311", "1/312", "1/313", "1/3100", "1/3101", "1/3103",
       "1/31021", "1/31023"},
      {"0/0121", "0/0123", "1/"}, options);
}

vector<string> ToTokens(const S2CellUnion& cell_union) {
  vector<string> tokens;
  for (auto& cell_id : cell_union) {
    tokens.push_back(cell_id.ToToken());
  }
  return tokens;
}

TEST(JavaCcConsistency, CheckCovering) {
  vector<S2Point> points = {
      S2LatLng::FromDegrees(-33.8663457, 151.1960891).ToPoint(),
      S2LatLng::FromDegrees(-33.866094000000004, 151.19517439999998).ToPoint()};
  S2Polyline polyline(points);
  S2RegionCoverer coverer;
  coverer.mutable_options()->set_min_level(0);
  coverer.mutable_options()->set_max_level(22);
  coverer.mutable_options()->set_max_cells(INT_MAX);
  S2CellUnion covering = coverer.GetCovering(polyline);
  vector<string> expected(
      {"6b12ae36313d", "6b12ae36313f", "6b12ae363141", "6b12ae363143",
       "6b12ae363145", "6b12ae363159", "6b12ae36315b", "6b12ae363343",
       "6b12ae363345", "6b12ae36334d", "6b12ae36334f", "6b12ae363369",
       "6b12ae36336f", "6b12ae363371", "6b12ae363377", "6b12ae363391",
       "6b12ae363393", "6b12ae36339b", "6b12ae36339d", "6b12ae3633e3",
       "6b12ae3633e5", "6b12ae3633ed", "6b12ae3633ef", "6b12ae37cc11",
       "6b12ae37cc13", "6b12ae37cc1b", "6b12ae37cc1d", "6b12ae37cc63",
       "6b12ae37cc65", "6b12ae37cc6d", "6b12ae37cc6f", "6b12ae37cc89",
       "6b12ae37cc8f", "6b12ae37cc91", "6b12ae37cc97", "6b12ae37ccb1",
       "6b12ae37ccb3", "6b12ae37ccbb", "6b12ae37ccbd", "6b12ae37cea5",
       "6b12ae37cea7", "6b12ae37cebb"});
  EXPECT_EQ(expected, ToTokens(covering));
}

}  // namespace
