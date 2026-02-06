// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "./fuzztest/internal/domains/flatbuffers_domain_impl.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/distributions.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "flatbuffers/base.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/reflection_generated.h"
#include "./fuzztest/domain_core.h"
#include "./fuzztest/internal/any.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/domains/domain_type_erasure.h"
#include "./fuzztest/internal/serialization.h"

namespace fuzztest::internal {

FlatbuffersTableUntypedDomainImpl::FlatbuffersTableUntypedDomainImpl(
    const reflection::Schema* absl_nonnull schema,
    const reflection::Object* absl_nonnull table_object)
    : schema_(schema), table_object_(table_object) {}

FlatbuffersTableUntypedDomainImpl::FlatbuffersTableUntypedDomainImpl(
    const FlatbuffersTableUntypedDomainImpl& other)
    : DomainBase(other),
      schema_(other.schema_),
      table_object_(other.table_object_) {
  absl::MutexLock l_other(&other.mutex_);
  absl::MutexLock l_this(&mutex_);
  domains_ = other.domains_;
}

FlatbuffersTableUntypedDomainImpl& FlatbuffersTableUntypedDomainImpl::operator=(
    const FlatbuffersTableUntypedDomainImpl& other) {
  DomainBase::operator=(other);
  schema_ = other.schema_;
  table_object_ = other.table_object_;
  absl::MutexLock l_other(&other.mutex_);
  absl::MutexLock l_this(&mutex_);
  domains_ = other.domains_;
  return *this;
}

FlatbuffersTableUntypedDomainImpl::FlatbuffersTableUntypedDomainImpl(
    FlatbuffersTableUntypedDomainImpl&& other)
    : schema_(other.schema_), table_object_(other.table_object_) {
  absl::MutexLock l_other(&other.mutex_);
  absl::MutexLock l_this(&mutex_);
  domains_ = std::move(other.domains_);
  DomainBase::operator=(std::move(other));
}

FlatbuffersTableUntypedDomainImpl& FlatbuffersTableUntypedDomainImpl::operator=(
    FlatbuffersTableUntypedDomainImpl&& other) {
  schema_ = other.schema_;
  table_object_ = other.table_object_;
  absl::MutexLock l_other(&other.mutex_);
  absl::MutexLock l_this(&mutex_);
  domains_ = std::move(other.domains_);
  DomainBase::operator=(std::move(other));
  return *this;
}

FlatbuffersTableUntypedDomainImpl::corpus_type
FlatbuffersTableUntypedDomainImpl::Init(absl::BitGenRef prng) {
  if (auto seed = this->MaybeGetRandomSeed(prng)) {
    return *seed;
  }
  corpus_type val;
  for (const auto* field : *table_object_->fields()) {
    VisitFlatbufferField(field, InitializeVisitor{*this, prng, val});
  }
  return val;
}

// Mutates the corpus value.
void FlatbuffersTableUntypedDomainImpl::Mutate(
    corpus_type& val, absl::BitGenRef prng,
    const domain_implementor::MutationMetadata& metadata, bool only_shrink) {
  uint64_t field_count = 0;
  for (const auto* field : *table_object_->fields()) {
    VisitFlatbufferField(field, CountNumberOfMutableFieldsVisitor{
                                    *this, field_count, val, only_shrink});
  }
  auto selected_field_index = absl::Uniform(prng, 0ul, field_count);

  MutateSelectedField(val, prng, metadata, only_shrink, selected_field_index);
}

uint64_t FlatbuffersTableUntypedDomainImpl::CountNumberOfFields(
    corpus_type& val) {
  uint64_t field_count = 0;
  for (const auto* field : *table_object_->fields()) {
    VisitFlatbufferField(
        field, CountNumberOfMutableFieldsVisitor{*this, field_count, val});
  }
  return field_count;
}

uint64_t FlatbuffersTableUntypedDomainImpl::MutateSelectedField(
    corpus_type& val, absl::BitGenRef prng,
    const domain_implementor::MutationMetadata& metadata, bool only_shrink,
    uint64_t selected_field_index) {
  uint64_t field_counter = 0;
  for (const auto* field : *table_object_->fields()) {
    if (!IsSupportedField(field)) {
      if (only_shrink && !val.contains(field->id())) continue;
    }

    if (field_counter == selected_field_index) {
      VisitFlatbufferField(
          field, MutateVisitor{*this, prng, metadata, only_shrink, val});
      return field_counter;
    }
    field_counter++;

    // TODO: Add support for tables.
    // TODO: Add support for vectors.
    // TODO: Add support for unions.

    if (field_counter > selected_field_index) {
      return field_counter;
    }
  }
  return field_counter;
}

absl::Status FlatbuffersTableUntypedDomainImpl::ValidateCorpusValue(
    const corpus_type& corpus_value) const {
  for (const auto* field : *table_object_->fields()) {
    absl::Status result;
    GenericDomainCorpusType field_corpus;
    if (auto it = corpus_value.find(field->id()); it != corpus_value.end()) {
      field_corpus = it->second;
    }
    VisitFlatbufferField(field, ValidateVisitor{*this, field_corpus, result});
    if (!result.ok()) return result;
  }
  return absl::OkStatus();
}

std::optional<FlatbuffersTableUntypedDomainImpl::corpus_type>
FlatbuffersTableUntypedDomainImpl::FromValue(const value_type& value) const {
  if (value == nullptr) {
    return std::nullopt;
  }
  corpus_type ret;
  for (const auto* field : *table_object_->fields()) {
    VisitFlatbufferField(field, FromValueVisitor{*this, value, ret});
  }
  return ret;
}

std::optional<FlatbuffersTableUntypedDomainImpl::corpus_type>
FlatbuffersTableUntypedDomainImpl::ParseCorpus(const IRObject& obj) const {
  corpus_type out;
  auto subs = obj.Subs();
  if (!subs) {
    return std::nullopt;
  }
  // Follows the structure created by `SerializeCorpus` to deserialize the
  // IRObject.

  // subs->size() represents the number of fields in the table.
  out.reserve(subs->size());
  for (const auto& sub : *subs) {
    auto pair_subs = sub.Subs();
    // Each field is represented by a pair of field id and the serialized
    // corpus value.
    if (!pair_subs.has_value() || pair_subs->size() != 2) {
      return std::nullopt;
    }

    // Deserialize the field id.
    auto id = (*pair_subs)[0].GetScalar<typename corpus_type::key_type>();
    if (!id.has_value()) {
      return std::nullopt;
    }

    // Get information about the field from reflection.
    const reflection::Field* absl_nullable field = GetFieldById(*id);
    if (field == nullptr) {
      return std::nullopt;
    }

    // Deserialize the field corpus value.
    std::optional<GenericDomainCorpusType> inner_parsed;
    VisitFlatbufferField(field,
                         ParseVisitor{*this, (*pair_subs)[1], inner_parsed});
    if (!inner_parsed) {
      return std::nullopt;
    }
    out[id.value()] = *std::move(inner_parsed);
  }
  return out;
}

IRObject FlatbuffersTableUntypedDomainImpl::SerializeCorpus(
    const corpus_type& value) const {
  IRObject out;
  auto& subs = out.MutableSubs();
  subs.reserve(value.size());

  // Each field is represented by a pair of field id and the serialized
  // corpus value.
  for (const auto& [id, field_corpus] : value) {
    // Get information about the field from reflection.
    const reflection::Field* absl_nullable field = GetFieldById(id);
    if (field == nullptr) {
      continue;
    }
    IRObject& pair = subs.emplace_back();
    auto& pair_subs = pair.MutableSubs();
    pair_subs.reserve(2);

    // Serialize the field id.
    pair_subs.emplace_back(field->id());

    // Serialize the field corpus value.
    VisitFlatbufferField(
        field, SerializeVisitor{*this, field_corpus, pair_subs.emplace_back()});
  }
  return out;
}

bool FlatbuffersTableUntypedDomainImpl::IsSupportedField(
    const reflection::Field* absl_nonnull field) const {
  auto base_type = field->type()->base_type();
  if (base_type == reflection::BaseType::UType) return false;
  if (flatbuffers::IsScalar(base_type)) return true;
  if (base_type == reflection::BaseType::String) return true;
  return false;
}

uint32_t FlatbuffersTableUntypedDomainImpl::BuildTable(
    const corpus_type& value, flatbuffers::FlatBufferBuilder& builder) const {
  // Add all the fields to the builder.

  // Offsets is the map of field id to its offset in the table.
  absl::flat_hash_map<typename corpus_type::key_type, flatbuffers::uoffset_t>
      offsets;

  // Some fields are stored inline in the flatbuffer table itself (a.k.a
  // "inline fields") and some are referenced by their offsets (a.k.a. "out of
  // line fields").
  //
  // "Out of line fields" shall be added to the builder first, so that we can
  // refer to them in the final table.
  for (const auto& [id, field_corpus] : value) {
    const reflection::Field* absl_nullable field = GetFieldById(id);
    if (field == nullptr) {
      continue;
    }
    // Take care of strings, and tables.
    VisitFlatbufferField(
        field, TableFieldBuilderVisitor{*this, builder, offsets, field_corpus});
  }

  // Now it is time to build the final table.
  uint32_t table_start = builder.StartTable();
  for (const auto& [id, field_corpus] : value) {
    const reflection::Field* absl_nullable field = GetFieldById(id);
    if (field == nullptr) {
      continue;
    }

    // Visit all fields.
    //
    // Inline fields will be stored in the table itself, out of line fields
    // will be referenced by their offsets.
    VisitFlatbufferField(
        field, TableBuilderVisitor{*this, builder, offsets, field_corpus});
  }
  return builder.EndTable(table_start);
}

}  // namespace fuzztest::internal
