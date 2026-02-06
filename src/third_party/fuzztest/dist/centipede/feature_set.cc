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

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "./centipede/control_flow.h"
#include "./centipede/feature.h"
#include "./common/logging.h"

namespace fuzztest::internal {

//------------------------------------------------------------------------------
//                                FeatureSet
//------------------------------------------------------------------------------

// This implementation is slow (needs to iterate over the entire domain),
// but there is no need for it to be fast.
PCIndexVec FeatureSet::ToCoveragePCs() const {
  PCIndexVec pcs;
  for (size_t idx = 0; idx < feature_domains::Domain::kDomainSize; ++idx) {
    if (frequencies_[feature_domains::kPCs.ConvertToMe(idx)])
      pcs.push_back(idx);
  }
  return pcs;
}

size_t FeatureSet::CountFeatures(feature_domains::Domain domain) const {
  return features_per_domain_[domain.domain_id()];
}

bool FeatureSet::HasUnseenFeatures(const FeatureVec &features) const {
  for (auto feature : features) {
    if (frequencies_[feature] == 0) return true;
  }
  return false;
}

__attribute__((noinline))  // to see it in profile.
size_t
FeatureSet::PruneFeaturesAndCountUnseen(FeatureVec &features) const {
  size_t number_of_unseen_features = 0;
  size_t num_kept = 0;
  for (auto feature : features) {
    if (ShouldDiscardFeature(feature)) continue;
    auto freq = frequencies_[feature];
    if (freq == 0) ++number_of_unseen_features;
    if (freq < FrequencyThreshold(feature)) features[num_kept++] = feature;
  }
  features.resize(num_kept);
  return number_of_unseen_features;
}

void FeatureSet::PruneDiscardedDomains(FeatureVec &features) const {
  size_t num_kept = 0;
  for (auto feature : features) {
    if (ShouldDiscardFeature(feature)) continue;
    features[num_kept++] = feature;
  }
  features.resize(num_kept);
}

void FeatureSet::IncrementFrequencies(const FeatureVec &features) {
  for (auto f : features) {
    auto &freq = frequencies_[f];
    if (freq == 0) {
      ++num_features_;
      ++features_per_domain_[feature_domains::Domain::FeatureToDomainId(f)];
    }
    if (freq < FrequencyThreshold(f)) ++freq;
  }
}

__attribute__((noinline))  // to see it in profile.
uint64_t
FeatureSet::ComputeWeight(const FeatureVec &features) const {
  uint64_t weight = 0;
  for (auto feature : features) {
    // The less frequent is the feature, the more valuable it is.
    // (frequency == 1) => (weight == 256)
    // (frequency == 2) => (weight == 128)
    // and so on.
    // The less frequent is the domain, the more valuable are its features.
    auto domain_id = feature_domains::Domain::FeatureToDomainId(feature);
    auto features_in_domain = features_per_domain_[domain_id];
    CHECK(features_in_domain);
    auto domain_weight = num_features_ / features_in_domain;
    auto feature_frequency = frequencies_[feature];
    CHECK_GT(feature_frequency, 0)
        << VV(feature) << VV(domain_id) << VV(features_in_domain)
        << VV(domain_weight) << VV((int)feature_frequency) << DebugString();
    weight += domain_weight * (256 / feature_frequency);
  }
  return weight;
}

std::string FeatureSet::DebugString() const {
  std::ostringstream os;
  os << VV((int)frequency_threshold_);
  os << VV(num_features_);
  os << this;
  return os.str();
}

std::ostream &operator<<(std::ostream &out, const FeatureSet &fs) {
  auto LogIfNotZero = [&out](size_t value, std::string_view name) {
    if (!value) return;
    out << " " << name << ": " << value;
  };
  out << "ft: " << fs.size();
  LogIfNotZero(fs.CountFeatures(feature_domains::kPCs), "cov");
  LogIfNotZero(fs.CountFeatures(feature_domains::k8bitCounters), "cnt");
  LogIfNotZero(fs.CountFeatures(feature_domains::kDataFlow), "df");
  LogIfNotZero(fs.CountFeatures(feature_domains::kCMPDomains), "cmp");
  LogIfNotZero(fs.CountFeatures(feature_domains::kCallStack), "stk");
  LogIfNotZero(fs.CountFeatures(feature_domains::kBoundedPath), "path");
  LogIfNotZero(fs.CountFeatures(feature_domains::kPCPair), "pair");
  for (size_t i = 0; i < std::size(feature_domains::kUserDomains); ++i) {
    LogIfNotZero(fs.CountFeatures(feature_domains::kUserDomains[i]),
                 absl::StrCat("usr", i));
  }
  LogIfNotZero(fs.CountFeatures(feature_domains::kUnknown), "unknown");
  return out;
}

}  // namespace fuzztest::internal
