// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/feature_set.h"

#include <bitset>
#include <cstddef>
#include <cstdint>

#include "gtest/gtest.h"
#include "./centipede/feature.h"

namespace fuzztest::internal {
namespace {

TEST(FeatureSet, ComputeWeight) {
  FeatureSet feature_set(10, {});

  auto W = [&](const FeatureVec &features) -> uint64_t {
    return feature_set.ComputeWeight(features);
  };

  feature_set.IncrementFrequencies({1, 2, 3});
  EXPECT_EQ(W({1}), W({2}));
  EXPECT_EQ(W({1}), W({3}));
  EXPECT_DEATH(W({4}), "");

  feature_set.IncrementFrequencies({1, 2});
  EXPECT_GT(W({3}), W({2}));
  EXPECT_GT(W({3}), W({1}));
  EXPECT_GT(W({3, 1}), W({2, 1}));
  EXPECT_GT(W({3, 2}), W({2}));

  feature_set.IncrementFrequencies({1});
  EXPECT_GT(W({3}), W({2}));
  EXPECT_GT(W({2}), W({1}));
  EXPECT_GT(W({3, 2}), W({3, 1}));
}

TEST(FeatureSet, ComputeWeightWithDifferentDomains) {
  FeatureSet feature_set(10, {});
  // Increment the feature frequencies such that the domain #1 is the rarest and
  // the domain #3 is the most frequent.
  auto f1 = feature_domains::k8bitCounters.begin();
  auto f2 = feature_domains::kCMP.begin();
  auto f3 = feature_domains::kBoundedPath.begin();
  feature_set.IncrementFrequencies(
      {/* one feature from domain #1 */ f1,
       /* two features from domain #2 */ f2, f2 + 1,
       /* three features from domain #3 */ f3, f3 + 1, f3 + 2});

  auto weight = [&](const FeatureVec &features) -> uint64_t {
    return feature_set.ComputeWeight(features);
  };

  // Test that features from a less frequent domain have more weight.
  EXPECT_GT(weight({f1}), weight({f2}));
  EXPECT_GT(weight({f2}), weight({f3}));
}

TEST(FeatureSet, HasUnseenFeatures_IncrementFrequencies) {
  size_t frequency_threshold = 2;
  FeatureSet feature_set(frequency_threshold, {});
  FeatureVec features = {10};
  EXPECT_TRUE(feature_set.HasUnseenFeatures(features));

  feature_set.IncrementFrequencies(features);
  EXPECT_FALSE(feature_set.HasUnseenFeatures(features));

  features = {10, 20};
  EXPECT_TRUE(feature_set.HasUnseenFeatures(features));
  feature_set.IncrementFrequencies(features);
  EXPECT_FALSE(feature_set.HasUnseenFeatures(features));

  features = {50};
  EXPECT_TRUE(feature_set.HasUnseenFeatures(features));
  feature_set.IncrementFrequencies(features);

  features = {10, 20};
  EXPECT_FALSE(feature_set.HasUnseenFeatures(features));
}

TEST(FeatureSet, PruneFeaturesAndCountUnseen_IncrementFrequencies) {
  size_t frequency_threshold = 3;
  FeatureSet feature_set(frequency_threshold, {});
  FeatureVec features;
  // Shorthand for PruneFeaturesAndCountUnseen.
  auto PruneAndCountUnseen = [&]() -> size_t {
    return feature_set.PruneFeaturesAndCountUnseen(features);
  };
  // Shorthand for IncrementFrequencies.
  auto Increment = [&](const FeatureVec &features) {
    feature_set.IncrementFrequencies(features);
  };

  // PruneAndCountUnseen on the empty set.
  features = {10, 20};
  EXPECT_EQ(PruneAndCountUnseen(), 2);
  EXPECT_EQ(feature_set.size(), 0);
  EXPECT_EQ(features, FeatureVec({10, 20}));

  // Add {10} for the first time.
  features = {10, 20};
  Increment({10});
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 1);
  EXPECT_EQ(features, FeatureVec({10, 20}));

  // Add {10} for the second time.
  features = {10, 20};
  Increment({10});
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 1);
  EXPECT_EQ(features, FeatureVec({10, 20}));

  // Add {10} for the third time. {10} becomes "frequent", prune removes it.
  features = {10, 20};
  Increment({10});
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 1);
  EXPECT_EQ(features, FeatureVec({20}));

  // Add {30} for the first time. {10, 20} still gets pruned to {20}.
  features = {10, 20};
  Increment({30});
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 2);
  EXPECT_EQ(features, FeatureVec({20}));

  // {10, 20, 30} => {20, 30}; 1 unseen.
  features = {10, 20, 30};
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 2);
  EXPECT_EQ(features, FeatureVec({20, 30}));

  // {10, 20, 30} => {20}; 1 unseen.
  features = {10, 20, 30};
  Increment({30});
  Increment({30});
  EXPECT_EQ(PruneAndCountUnseen(), 1);
  EXPECT_EQ(feature_set.size(), 2);
  EXPECT_EQ(features, FeatureVec({20}));

  // {10, 20, 30} => {20}; 0 unseen.
  features = {10, 20, 30};
  Increment({20});
  Increment({20});
  EXPECT_EQ(PruneAndCountUnseen(), 0);
  EXPECT_EQ(feature_set.size(), 3);
  EXPECT_EQ(features, FeatureVec({20}));

  // {10, 20, 30} => {}; 0 unseen.
  features = {10, 20, 30};
  Increment({20});
  EXPECT_EQ(PruneAndCountUnseen(), 0);
  EXPECT_EQ(feature_set.size(), 3);
  EXPECT_EQ(features, FeatureVec({}));
}

TEST(FeatureSet, PruneDiscardedDomains) {
  for (size_t i = 0; i < feature_domains::kNumDomains; ++i) {
    SCOPED_TRACE(i);

    // Ban one domain.
    std::bitset<feature_domains::kNumDomains> discarded_domains;
    discarded_domains.set(i);
    FeatureSet feature_set(10, discarded_domains);

    FeatureVec features;
    FeatureVec expected;
    for (size_t j = 0; j < feature_domains::kNumDomains; ++j) {
      feature_t f = feature_domains::Domain(j).ConvertToMe(0);
      // Input vector with a feature in every domain.
      features.push_back(f);
      if (j != i) expected.push_back(f);
    }

    FeatureVec f1 = features;
    feature_set.PruneDiscardedDomains(f1);
    EXPECT_EQ(f1.size(), features.size() - 1);
    EXPECT_EQ(f1, expected);

    // PruneFeaturesAndCountUnseen should, at minimum, prune the same domains as
    // PruneDiscardedDomains.
    FeatureVec f2 = features;
    feature_set.PruneFeaturesAndCountUnseen(f2);
    EXPECT_EQ(f2.size(), features.size() - 1);
    EXPECT_EQ(f2, expected);
  }
}

}  // namespace
}  // namespace fuzztest::internal
