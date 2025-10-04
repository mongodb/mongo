// Copyright 2009 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/filtered_re2.h"

#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_log.h"
#include "absl/strings/string_view.h"
#include "re2/prefilter.h"
#include "re2/prefilter_tree.h"

namespace re2 {

FilteredRE2::FilteredRE2()
    : compiled_(false),
      prefilter_tree_(new PrefilterTree()) {
}

FilteredRE2::FilteredRE2(int min_atom_len)
    : compiled_(false),
      prefilter_tree_(new PrefilterTree(min_atom_len)) {
}

FilteredRE2::~FilteredRE2() {
  for (size_t i = 0; i < re2_vec_.size(); i++)
    delete re2_vec_[i];
}

FilteredRE2::FilteredRE2(FilteredRE2&& other)
    : re2_vec_(std::move(other.re2_vec_)),
      compiled_(other.compiled_),
      prefilter_tree_(std::move(other.prefilter_tree_)) {
  other.re2_vec_.clear();
  other.re2_vec_.shrink_to_fit();
  other.compiled_ = false;
  other.prefilter_tree_.reset(new PrefilterTree());
}

FilteredRE2& FilteredRE2::operator=(FilteredRE2&& other) {
  this->~FilteredRE2();
  (void) new (this) FilteredRE2(std::move(other));
  return *this;
}

RE2::ErrorCode FilteredRE2::Add(absl::string_view pattern,
                                const RE2::Options& options, int* id) {
  RE2* re = new RE2(pattern, options);
  RE2::ErrorCode code = re->error_code();

  if (!re->ok()) {
    if (options.log_errors()) {
      ABSL_LOG(ERROR) << "Couldn't compile regular expression, skipping: "
                      << pattern << " due to error " << re->error();
    }
    delete re;
  } else {
    *id = static_cast<int>(re2_vec_.size());
    re2_vec_.push_back(re);
  }

  return code;
}

void FilteredRE2::Compile(std::vector<std::string>* atoms) {
  if (compiled_) {
    ABSL_LOG(ERROR) << "Compile called already.";
    return;
  }

  // Similarly to PrefilterTree::Compile(), make compiling
  // a no-op if it's attempted before adding any patterns.
  if (re2_vec_.empty()) {
    return;
  }

  for (size_t i = 0; i < re2_vec_.size(); i++) {
    Prefilter* prefilter = Prefilter::FromRE2(re2_vec_[i]);
    prefilter_tree_->Add(prefilter);
  }
  atoms->clear();
  prefilter_tree_->Compile(atoms);
  compiled_ = true;
}

int FilteredRE2::SlowFirstMatch(absl::string_view text) const {
  for (size_t i = 0; i < re2_vec_.size(); i++)
    if (RE2::PartialMatch(text, *re2_vec_[i]))
      return static_cast<int>(i);
  return -1;
}

int FilteredRE2::FirstMatch(absl::string_view text,
                            const std::vector<int>& atoms) const {
  if (!compiled_) {
    ABSL_LOG(DFATAL) << "FirstMatch called before Compile.";
    return -1;
  }
  std::vector<int> regexps;
  prefilter_tree_->RegexpsGivenStrings(atoms, &regexps);
  for (size_t i = 0; i < regexps.size(); i++)
    if (RE2::PartialMatch(text, *re2_vec_[regexps[i]]))
      return regexps[i];
  return -1;
}

bool FilteredRE2::AllMatches(absl::string_view text,
                             const std::vector<int>& atoms,
                             std::vector<int>* matching_regexps) const {
  matching_regexps->clear();
  std::vector<int> regexps;
  prefilter_tree_->RegexpsGivenStrings(atoms, &regexps);
  for (size_t i = 0; i < regexps.size(); i++)
    if (RE2::PartialMatch(text, *re2_vec_[regexps[i]]))
      matching_regexps->push_back(regexps[i]);
  return !matching_regexps->empty();
}

void FilteredRE2::AllPotentials(const std::vector<int>& atoms,
                                std::vector<int>* potential_regexps) const {
  prefilter_tree_->RegexpsGivenStrings(atoms, potential_regexps);
}

void FilteredRE2::RegexpsGivenStrings(const std::vector<int>& matched_atoms,
                                      std::vector<int>* passed_regexps) {
  prefilter_tree_->RegexpsGivenStrings(matched_atoms, passed_regexps);
}

void FilteredRE2::PrintPrefilter(int regexpid) {
  prefilter_tree_->PrintPrefilter(regexpid);
}

}  // namespace re2
