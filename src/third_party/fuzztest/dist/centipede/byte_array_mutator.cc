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

#include "./centipede/byte_array_mutator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "./centipede/execution_metadata.h"
#include "./centipede/knobs.h"
#include "./centipede/mutation_input.h"
#include "./common/defs.h"

namespace fuzztest::internal {

//============= CmpDictionary ===============
bool CmpDictionary::SetFromMetadata(const ExecutionMetadata &metadata) {
  dictionary_.clear();
  if (!metadata.ForEachCmpEntry([&](ByteSpan a, ByteSpan b) {
        auto size = a.size();
        if (size > DictEntry::kMaxEntrySize) return;
        if (size < kMinEntrySize) return;
        // TODO(kcc): disregard boring CMP pairs, such as e.g. `1 CMP 0`.
        dictionary_.emplace_back(a, b);
        dictionary_.emplace_back(b, a);
      }))
    return false;
  std::sort(dictionary_.begin(), dictionary_.end());
  return true;
}

void CmpDictionary::SuggestReplacement(
    ByteSpan bytes, std::vector<ByteSpan> &suggestions) const {
  if (!suggestions.capacity()) return;
  suggestions.clear();
  if (bytes.size() < kMinEntrySize) return;
  // Use binary search to find the first entry that starts with the
  // same kMinEntrySize bytes as `bytes`.
  // This is not supper efficient.
  // We need to see the real usage before optimizing.
  // TODO(kcc): investigate using absl/container/btree_map.h instead.
  DictEntry prefix({bytes.begin(), kMinEntrySize});
  auto iter = std::lower_bound(
      dictionary_.begin(), dictionary_.end(), Pair{prefix, prefix},
      [](const Pair &a, const Pair &b) { return a.first < b.first; });
  // Iterate from the first entry that has the same first bytes as `bytes`
  // to the last such entry.
  for (; iter != dictionary_.end(); ++iter) {
    const auto &a = iter->first;
    const auto &b = iter->second;
    // Check if `suggestions` is out of capacity.
    if (suggestions.size() == suggestions.capacity()) break;
    // Check if the first kMinEntrySize bytes are still the same.
    if (!std::equal(bytes.begin(), bytes.begin() + kMinEntrySize, a.begin()))
      break;
    // Check if we have enough bytes to compare with `a`.
    if (bytes.size() < a.size()) continue;
    // If all bytes are the same as `a`, suggest `b`.
    if (std::equal(a.begin(), a.end(), bytes.begin()))
      suggestions.emplace_back(b.begin(), b.size());
  }
}

//============= ByteArrayMutator ===============
size_t ByteArrayMutator::RoundUpToAdd(size_t curr_size, size_t to_add) {
  if (curr_size >= max_len_) return 0;
  const size_t remainder = (curr_size + to_add) % size_alignment_;
  if (remainder != 0) {
    to_add = to_add + size_alignment_ - remainder;
  }
  if (curr_size + to_add > max_len_) return max_len_ - curr_size;
  return to_add;
}

size_t ByteArrayMutator::RoundDownToRemove(size_t curr_size, size_t to_remove) {
  if (curr_size <= size_alignment_) return 0;
  if (to_remove >= curr_size) return curr_size - size_alignment_;

  size_t result_size = curr_size - to_remove;
  result_size -= (result_size % size_alignment_);
  to_remove = curr_size - result_size;
  if (result_size == 0) {
    to_remove -= size_alignment_;
  }
  if (result_size > max_len_) {
    return curr_size - max_len_;
  }
  return to_remove;
}

static const KnobId knob_mutate[3] = {Knobs::NewId("mutate_same_size"),
                                      Knobs::NewId("mutate_decrease_size"),
                                      Knobs::NewId("mutate_increase_size")};

bool ByteArrayMutator::Mutate(ByteArray &data) {
  // Individual mutator may fail to mutate and return false.
  // So we iterate a few times and expect one of the mutations will succeed.
  for (int iter = 0; iter < 15; iter++) {
    Fn mutator = nullptr;
    if (data.size() > max_len_) {
      mutator = &ByteArrayMutator::MutateDecreaseSize;
    } else if (data.size() == max_len_) {
      mutator = knobs_.Choose<Fn>({knob_mutate[0], knob_mutate[1]},
                                  {&ByteArrayMutator::MutateSameSize,
                                   &ByteArrayMutator::MutateDecreaseSize},
                                  rng_());
    } else {
      mutator = knobs_.Choose<Fn>(knob_mutate,
                                  {&ByteArrayMutator::MutateSameSize,
                                   &ByteArrayMutator::MutateIncreaseSize,
                                   &ByteArrayMutator::MutateDecreaseSize},
                                  rng_());
    }
    if ((this->*mutator)(data)) return true;
  }
  return false;
}

static const KnobId knob_mutate_same_size[5] = {
    Knobs::NewId("mutate_same_size_0"), Knobs::NewId("mutate_same_size_1"),
    Knobs::NewId("mutate_same_size_2"), Knobs::NewId("mutate_same_size_3"),
    Knobs::NewId("mutate_same_size_4"),
};

bool ByteArrayMutator::MutateSameSize(ByteArray &data) {
  auto mutator = knobs_.Choose<Fn>(
      knob_mutate_same_size,
      {&ByteArrayMutator::FlipBit, &ByteArrayMutator::SwapBytes,
       &ByteArrayMutator::ChangeByte,
       &ByteArrayMutator::OverwriteFromDictionary,
       &ByteArrayMutator::OverwriteFromCmpDictionary},
      rng_());
  return (this->*mutator)(data);
}

static const KnobId knob_mutate_increase_size[2] = {
    Knobs::NewId("mutate_increase_size_0"),
    Knobs::NewId("mutate_increase_size_1"),
};

bool ByteArrayMutator::MutateIncreaseSize(ByteArray &data) {
  auto mutator = knobs_.Choose<Fn>(
      knob_mutate_increase_size,
      {&ByteArrayMutator::InsertBytes, &ByteArrayMutator::InsertFromDictionary},
      rng_());
  return (this->*mutator)(data);
}

bool ByteArrayMutator::MutateDecreaseSize(ByteArray &data) {
  auto mutator = &ByteArrayMutator::EraseBytes;
  return (this->*mutator)(data);
}

bool ByteArrayMutator::FlipBit(ByteArray &data) {
  uintptr_t random = rng_();
  size_t bit_idx = random % (data.size() * 8);
  size_t byte_idx = bit_idx / 8;
  bit_idx %= 8;
  uint8_t mask = 1 << bit_idx;
  data[byte_idx] ^= mask;
  return true;
}

bool ByteArrayMutator::SwapBytes(ByteArray &data) {
  size_t idx1 = rng_() % data.size();
  size_t idx2 = rng_() % data.size();
  std::swap(data[idx1], data[idx2]);
  return true;
}

bool ByteArrayMutator::ChangeByte(ByteArray &data) {
  size_t idx = rng_() % data.size();
  data[idx] = rng_();
  return true;
}

bool ByteArrayMutator::InsertBytes(ByteArray &data) {
  // Don't insert too many bytes at once.
  const size_t kMaxInsertSize = 20;
  size_t num_new_bytes = rng_() % kMaxInsertSize + 1;
  num_new_bytes = RoundUpToAdd(data.size(), num_new_bytes);
  if (num_new_bytes > kMaxInsertSize) {
    num_new_bytes -= size_alignment_;
  }
  // There are N+1 positions to insert something into an array of N.
  size_t pos = rng_() % (data.size() + 1);
  // Fixed array to avoid memory allocation.
  std::array<uint8_t, kMaxInsertSize> new_bytes;
  for (size_t i = 0; i < num_new_bytes; i++) new_bytes[i] = rng_();
  data.insert(data.begin() + pos, new_bytes.begin(),
              new_bytes.begin() + num_new_bytes);
  return true;
}

bool ByteArrayMutator::EraseBytes(ByteArray &data) {
  if (data.size() <= size_alignment_) return false;
  // Ok to erase a sizable chunk since small inputs are good (if they
  // produce good features).
  size_t num_bytes_to_erase = rng_() % (data.size() / 2) + 1;
  num_bytes_to_erase = RoundDownToRemove(data.size(), num_bytes_to_erase);
  if (num_bytes_to_erase == 0) return false;
  size_t pos = rng_() % (data.size() - num_bytes_to_erase + 1);
  data.erase(data.begin() + pos, data.begin() + pos + num_bytes_to_erase);
  return true;
}

void ByteArrayMutator::AddToDictionary(
    const std::vector<ByteArray> &dict_entries) {
  for (const ByteArray &entry : dict_entries) {
    if (entry.size() > DictEntry::kMaxEntrySize) continue;
    dictionary_.emplace_back(entry);
  }
}

bool ByteArrayMutator::OverwriteFromDictionary(ByteArray &data) {
  if (dictionary_.empty()) return false;
  size_t dict_entry_idx = rng_() % dictionary_.size();
  const auto &dic_entry = dictionary_[dict_entry_idx];
  if (dic_entry.size() > data.size()) return false;
  size_t overwrite_pos = rng_() % (data.size() - dic_entry.size() + 1);
  std::copy(dic_entry.begin(), dic_entry.end(), data.begin() + overwrite_pos);
  return true;
}

bool ByteArrayMutator::OverwriteFromCmpDictionary(ByteArray &data) {
  if (cmp_dictionary_.size() == 0) return false;
  if (data.size() < CmpDictionary::kMinEntrySize) return false;
  // Start with a random position in `data`, search though the entire `data`
  // until some suggestion is found.
  size_t search_start_idx = rng_() % data.size();
  constexpr size_t kMaxNumSuggestions = 100;
  std::vector<ByteSpan> suggestions;
  suggestions.reserve(kMaxNumSuggestions);
  for (size_t i = 0; i < data.size(); i++) {
    size_t idx = (search_start_idx + i) % data.size();
    if (idx + CmpDictionary::kMinEntrySize >= data.size()) continue;
    ByteSpan tail{&data[idx], data.size() - idx};
    cmp_dictionary_.SuggestReplacement(tail, suggestions);
    if (suggestions.empty()) continue;
    auto suggestion = suggestions[rng_() % suggestions.size()];
    if (idx + suggestion.size() <= data.size()) {
      std::copy(suggestion.begin(), suggestion.end(), data.begin() + idx);
      return true;
    }
  }
  return false;
}

bool ByteArrayMutator::InsertFromDictionary(ByteArray &data) {
  if (dictionary_.empty()) return false;
  size_t dict_entry_idx = rng_() % dictionary_.size();
  const auto &dict_entry = dictionary_[dict_entry_idx];
  // There are N+1 positions to insert something into an array of N.
  size_t pos = rng_() % (data.size() + 1);
  data.insert(data.begin() + pos, dict_entry.begin(), dict_entry.end());
  return true;
}

void ByteArrayMutator::CrossOverInsert(ByteArray &data,
                                       const ByteArray &other) {
  if ((data.size() % size_alignment_) + other.size() < size_alignment_) return;
  // insert other[first:first+size] at data[pos]
  size_t size = 1 + rng_() % other.size();
  size = RoundUpToAdd(data.size(), size);
  if (size > other.size()) {
    size -= size_alignment_;
  }
  size_t first = rng_() % (other.size() - size + 1);
  size_t pos = rng_() % (data.size() + 1);
  data.insert(data.begin() + pos, other.begin() + first,
              other.begin() + first + size);
}

void ByteArrayMutator::CrossOverOverwrite(ByteArray &data,
                                          const ByteArray &other) {
  // Overwrite data[pos:pos+size] with other[first:first+size].
  // Overwrite no more than half of data.
  size_t max_size = std::max(1UL, data.size() / 2);
  size_t first = rng_() % other.size();
  max_size = std::min(max_size, other.size() - first);
  size_t size = 1 + rng_() % max_size;
  size_t max_pos = data.size() - size;
  size_t pos = rng_() % (max_pos + 1);
  std::copy(other.begin() + first, other.begin() + first + size,
            data.begin() + pos);
}

const KnobId knob_cross_over_insert_or_overwrite =
    Knobs::NewId("cross_over_insert_or_overwrite");

void ByteArrayMutator::CrossOver(ByteArray &data, const ByteArray &other) {
  if (data.size() >= max_len_) {
    CrossOverOverwrite(data, other);
  } else {
    if (knobs_.GenerateBool(knob_cross_over_insert_or_overwrite, rng_())) {
      CrossOverInsert(data, other);
    } else {
      CrossOverOverwrite(data, other);
    }
  }
}

// Controls how much crossover is used during mutations.
// https://en.wikipedia.org/wiki/Crossover_(genetic_algorithm)
// TODO(kcc): add tests with different values of knobs.
const KnobId knob_mutate_or_crossover = Knobs::NewId("mutate_or_crossover");

std::vector<ByteArray> ByteArrayMutator::MutateMany(
    const std::vector<MutationInputRef> &inputs, size_t num_mutants) {
  if (inputs.empty()) abort();
  // TODO(xinhaoyuan): Consider metadata in other inputs instead of always the
  // first one.
  SetMetadata(inputs[0].metadata != nullptr ? *inputs[0].metadata
                                            : ExecutionMetadata());
  size_t num_inputs = inputs.size();
  std::vector<ByteArray> mutants;
  mutants.reserve(num_mutants);
  for (size_t i = 0; i < num_mutants; ++i) {
    auto mutant = inputs[rng_() % num_inputs].data;
    if (mutant.size() <= max_len_ &&
        knobs_.GenerateBool(knob_mutate_or_crossover, rng_())) {
      // Do crossover only if the mutant is not over the max_len_.
      // Perform crossover with some other input. It may be the same input.
      const auto &other_input = inputs[rng_() % num_inputs].data;
      CrossOver(mutant, other_input);
    } else {
      // Perform mutation.
      Mutate(mutant);
    }
    mutants.push_back(std::move(mutant));
  }
  return mutants;
}

}  // namespace fuzztest::internal
