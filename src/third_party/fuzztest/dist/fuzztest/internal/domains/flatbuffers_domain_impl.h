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

#ifndef FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLATBUFFERS_DOMAIN_IMPL_H_
#define FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLATBUFFERS_DOMAIN_IMPL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/nullability.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "flatbuffers/base.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "flatbuffers/reflection_generated.h"
#include "flatbuffers/string.h"
#include "flatbuffers/table.h"
#include "flatbuffers/verifier.h"
#include "./fuzztest/domain_core.h"
#include "./fuzztest/internal/any.h"
#include "./fuzztest/internal/domains/arbitrary_impl.h"
#include "./fuzztest/internal/domains/domain_base.h"
#include "./fuzztest/internal/domains/domain_type_erasure.h"
#include "./fuzztest/internal/domains/element_of_impl.h"
#include "./fuzztest/internal/logging.h"
#include "./fuzztest/internal/meta.h"
#include "./fuzztest/internal/serialization.h"
#include "./fuzztest/internal/status.h"

namespace fuzztest::internal {

//
// Flatbuffers enum detection.
//
template <typename Underlying,
          typename = std::enable_if_t<std::is_integral_v<Underlying> &&
                                      !std::is_same_v<Underlying, bool>>>
struct FlatbuffersEnumTag {
  using type = Underlying;
};

template <typename T>
struct is_flatbuffers_enum_tag : std::false_type {};

template <typename Underlying>
struct is_flatbuffers_enum_tag<FlatbuffersEnumTag<Underlying>>
    : std::true_type {};

template <typename T>
inline constexpr bool is_flatbuffers_enum_tag_v =
    is_flatbuffers_enum_tag<T>::value;

struct FlatbuffersArrayTag;
struct FlatbuffersObjTag;
struct FlatbuffersUnionTag;
struct FlatbuffersVectorTag;

// Dynamic to static dispatch visitor pattern.
template <typename Visitor>
auto VisitFlatbufferField(const reflection::Field* absl_nonnull field,
                          Visitor visitor) {
  auto field_index = field->type()->index();
  switch (field->type()->base_type()) {
    case reflection::BaseType::Bool:
      visitor.template Visit<bool>(field);
      break;
    case reflection::BaseType::Byte:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<int8_t>>(field);
      } else {
        visitor.template Visit<int8_t>(field);
      }
      break;
    case reflection::BaseType::Short:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<int16_t>>(field);
      } else {
        visitor.template Visit<int16_t>(field);
      }
      break;
    case reflection::BaseType::Int:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<int32_t>>(field);
      } else {
        visitor.template Visit<int32_t>(field);
      }
      break;
    case reflection::BaseType::Long:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<int64_t>>(field);
      } else {
        visitor.template Visit<int64_t>(field);
      }
      break;
    case reflection::BaseType::UByte:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<uint8_t>>(field);
      } else {
        visitor.template Visit<uint8_t>(field);
      }
      break;
    case reflection::BaseType::UShort:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<uint16_t>>(field);
      } else {
        visitor.template Visit<uint16_t>(field);
      }
      break;
    case reflection::BaseType::UInt:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<uint32_t>>(field);
      } else {
        visitor.template Visit<uint32_t>(field);
      }
      break;
    case reflection::BaseType::ULong:
      if (field_index >= 0) {
        visitor.template Visit<FlatbuffersEnumTag<uint64_t>>(field);
      } else {
        visitor.template Visit<uint64_t>(field);
      }
      break;
    case reflection::BaseType::Float:
      visitor.template Visit<float>(field);
      break;
    case reflection::BaseType::Double:
      visitor.template Visit<double>(field);
      break;
    case reflection::BaseType::String:
      visitor.template Visit<std::string>(field);
      break;
    case reflection::BaseType::Vector:
    case reflection::BaseType::Vector64:
      visitor.template Visit<FlatbuffersVectorTag>(field);
      break;
    case reflection::BaseType::Array:
      visitor.template Visit<FlatbuffersArrayTag>(field);
      break;
    case reflection::BaseType::Obj:
      visitor.template Visit<FlatbuffersObjTag>(field);
      break;
    case reflection::BaseType::Union:
      visitor.template Visit<FlatbuffersUnionTag>(field);
      break;
    case reflection::BaseType::UType:
      // Noop
      break;
    default:
      FUZZTEST_INTERNAL_CHECK(false, absl::StrCat("Unsupported base type: ",
                                                  field->type()->base_type()));
  }
}

// Flatbuffers enum domain implementation.
template <typename Underlaying>
class FlatbuffersEnumDomainImpl
    : public domain_implementor::DomainBase<
          /*Derived=*/FlatbuffersEnumDomainImpl<Underlaying>,
          /*ValueType=*/Underlaying,
          /*CorpusType=*/ElementOfImplCorpusType> {
 public:
  using typename FlatbuffersEnumDomainImpl::DomainBase::corpus_type;
  using typename FlatbuffersEnumDomainImpl::DomainBase::value_type;

  explicit FlatbuffersEnumDomainImpl(const reflection::Enum* enum_def)
      : enum_def_(enum_def), inner_(GetEnumValues(enum_def, {})) {}

  FlatbuffersEnumDomainImpl& WithExcludedValues(
      std::initializer_list<value_type> excluded_values) {
    excluded_values_ = {excluded_values.begin(), excluded_values.end()};
    inner_ =
        ElementOfImpl<Underlaying>(GetEnumValues(enum_def_, excluded_values));
    return *this;
  }

  corpus_type Init(absl::BitGenRef prng) {
    if (auto seed = this->MaybeGetRandomSeed(prng)) return *seed;
    return inner_.Init(prng);
  }

  void Mutate(corpus_type& val, absl::BitGenRef prng,
              const domain_implementor::MutationMetadata& metadata,
              bool only_shrink) {
    inner_.Mutate(val, prng, metadata, only_shrink);
  }

  value_type GetValue(corpus_type value) const {
    return inner_.GetValue(value);
  }

  std::optional<corpus_type> FromValue(const value_type& v) const {
    return inner_.FromValue(v);
  }

  std::optional<corpus_type> ParseCorpus(const IRObject& obj) const {
    return inner_.ParseCorpus(obj);
  }

  IRObject SerializeCorpus(const corpus_type& v) const {
    return inner_.SerializeCorpus(v);
  }

  absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const {
    for (const auto* value : *enum_def_->values()) {
      if (excluded_values_.contains(value->value())) continue;
      if (value->value() == static_cast<size_t>(corpus_value)) {
        return absl::OkStatus();
      }
    }
    return absl::InvalidArgumentError(absl::StrCat("Enum value ", corpus_value,
                                                   " is not valid for enum ",
                                                   enum_def_->name()->str()));
  }

  auto GetPrinter() const { return Printer{*this}; }

 private:
  const reflection::Enum* enum_def_;
  absl::flat_hash_set<value_type> excluded_values_;
  ElementOfImpl<Underlaying> inner_;

  static std::vector<value_type> GetEnumValues(
      const reflection::Enum* enum_def,
      std::initializer_list<value_type> excluded_values) {
    std::vector<value_type> values;
    values.reserve(enum_def->values()->size());
    for (const auto* value : *enum_def->values()) {
      FUZZTEST_INTERNAL_CHECK(
          value->value() >= std::numeric_limits<value_type>::min() &&
              value->value() <= std::numeric_limits<value_type>::max(),
          "Enum value from reflection is out of range for the target type.");
      if (std::find(excluded_values.begin(), excluded_values.end(),
                    value->value()) == excluded_values.end()) {
        values.push_back(static_cast<value_type>(value->value()));
      }
    }
    return values;
  }

  struct Printer {
    const FlatbuffersEnumDomainImpl& self;
    void PrintCorpusValue(const corpus_type& value,
                          domain_implementor::RawSink out,
                          domain_implementor::PrintMode mode) const {
      if (mode == domain_implementor::PrintMode::kHumanReadable) {
        auto user_value = self.GetValue(value);
        absl::Format(
            out, "%s",
            self.enum_def_->values()->LookupByKey(user_value)->name()->str());
      } else {
        absl::Format(out, "%d", value);
      }
    }
  };
};

// Forward declaration of the domain factory for flatbuffers fields.
template <typename T>
auto GetDefaultDomain(const reflection::Schema* absl_nonnull schema,
                      const reflection::Field* absl_nonnull field);

// Domain implementation for flatbuffers untyped tables.
// The corpus type is a map of field ids to field values.
class FlatbuffersTableUntypedDomainImpl
    : public fuzztest::domain_implementor::DomainBase<
          /*Derived=*/FlatbuffersTableUntypedDomainImpl,
          /*ValueType=*/const flatbuffers::Table* absl_nonnull,
          /*CorpusType=*/
          absl::flat_hash_map<
              decltype(static_cast<reflection::Field*>(nullptr)->id()),
              GenericDomainCorpusType>> {
 public:
  template <typename T>
  friend class FlatbuffersTableDomainImpl;

  using typename FlatbuffersTableUntypedDomainImpl::DomainBase::corpus_type;
  using typename FlatbuffersTableUntypedDomainImpl::DomainBase::value_type;

  explicit FlatbuffersTableUntypedDomainImpl(
      const reflection::Schema* absl_nonnull schema,
      const reflection::Object* absl_nonnull table_object);

  FlatbuffersTableUntypedDomainImpl(
      const FlatbuffersTableUntypedDomainImpl& other);

  FlatbuffersTableUntypedDomainImpl& operator=(
      const FlatbuffersTableUntypedDomainImpl& other);

  FlatbuffersTableUntypedDomainImpl(FlatbuffersTableUntypedDomainImpl&& other);

  FlatbuffersTableUntypedDomainImpl& operator=(
      FlatbuffersTableUntypedDomainImpl&& other);

  // Initializes the corpus value.
  corpus_type Init(absl::BitGenRef prng);

  // Mutates the corpus value.
  void Mutate(corpus_type& val, absl::BitGenRef prng,
              const domain_implementor::MutationMetadata& metadata,
              bool only_shrink);

  // Counts the number of fields that can be mutated.
  uint64_t CountNumberOfFields(corpus_type& val);

  // Mutates the selected field.
  // The selected field index is based on the flattened tree.
  uint64_t MutateSelectedField(
      corpus_type& val, absl::BitGenRef prng,
      const domain_implementor::MutationMetadata& metadata, bool only_shrink,
      uint64_t selected_field_index);

  auto GetPrinter() const { return Printer{*this}; }

  absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const;

  value_type GetValue(const corpus_type& value) const {
    FUZZTEST_INTERNAL_CHECK(
        false, "GetValue is not supported for the untyped Flatbuffers domain.");
    // Untyped domain does not support GetValue since if it is a nested table it
    // would need the top level table corpus value to be able to build it.
    return nullptr;
  }

  // Converts the table pointer to a corpus value.
  std::optional<corpus_type> FromValue(const value_type& value) const;

  // Converts the IRObject to a corpus value.
  std::optional<corpus_type> ParseCorpus(const IRObject& obj) const;

  // Converts the corpus value to an IRObject.
  IRObject SerializeCorpus(const corpus_type& value) const;

 private:
  const reflection::Schema* absl_nonnull schema_;
  const reflection::Object* absl_nonnull table_object_;
  mutable absl::Mutex mutex_;
  mutable absl::flat_hash_map<typename corpus_type::key_type, CopyableAny>
      domains_ ABSL_GUARDED_BY(mutex_);

  bool IsSupportedField(const reflection::Field* absl_nonnull field) const;

  uint32_t BuildTable(const corpus_type& value,
                      flatbuffers::FlatBufferBuilder& builder) const;

  // Returns the domain for the given field.
  // The domain is cached, and the same instance is returned for the same field.
  template <typename T>
  auto& GetCachedDomain(const reflection::Field* field) const {
    auto get_opt_domain = [this, field]() {
      auto opt_domain = OptionalOf(GetDefaultDomain<T>(schema_, field));
      if (!field->optional()) {
        opt_domain.SetWithoutNull();
      }
      return Domain<value_type_t<decltype(opt_domain)>>{opt_domain};
    };

    using DomainT = decltype(get_opt_domain());
    // Do the operation under a lock to prevent race conditions in `const`
    // methods.
    absl::MutexLock l(&mutex_);
    auto it = domains_.find(field->id());
    if (it == domains_.end()) {
      it = domains_
               .try_emplace(field->id(), std::in_place_type<DomainT>,
                            get_opt_domain())
               .first;
    }
    return it->second.template GetAs<DomainT>();
  }

  const reflection::Field* absl_nullable GetFieldById(
      typename corpus_type::key_type id) const {
    const auto it =
        absl::c_find_if(*table_object_->fields(),
                        [id](const auto* field) { return field->id() == id; });
    return it != table_object_->fields()->end() ? *it : nullptr;
  }

  struct SerializeVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    const GenericDomainCorpusType& corpus_value;
    IRObject& out;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) {
      out = self.GetCachedDomain<T>(field).SerializeCorpus(corpus_value);
    }
  };

  struct FromValueVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    value_type user_value;
    corpus_type& corpus_value;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) const {
      [[maybe_unused]]
      reflection::BaseType base_type = field->type()->base_type();
      auto& domain = self.GetCachedDomain<T>(field);
      value_type_t<std::decay_t<decltype(domain)>> inner_value;

      if constexpr (is_flatbuffers_enum_tag_v<T>) {
        if (!field->optional() || user_value->CheckField(field->offset())) {
          inner_value = user_value->GetField<typename T::type>(
              field->offset(), field->default_integer());
        }
      } else if constexpr (std::is_integral_v<T>) {
        if (!field->optional() || user_value->CheckField(field->offset())) {
          inner_value = std::optional(user_value->GetField<T>(
              field->offset(), field->default_integer()));
        }
      } else if constexpr (std::is_floating_point_v<T>) {
        if (!field->optional() || user_value->CheckField(field->offset())) {
          inner_value = std::optional(
              user_value->GetField<T>(field->offset(), field->default_real()));
        }
      } else if constexpr (std::is_same_v<T, std::string>) {
        if (user_value->CheckField(field->offset())) {
          inner_value = std::optional(
              user_value->GetPointer<flatbuffers::String*>(field->offset())
                  ->str());
        }
      }

      auto inner = domain.FromValue(inner_value);
      if (inner) {
        corpus_value[field->id()] = *std::move(inner);
      }
    };
  };

  // Create out-of-line table fields, see `BuildTable` for details.
  struct TableFieldBuilderVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    flatbuffers::FlatBufferBuilder& builder;
    absl::flat_hash_map<typename corpus_type::key_type, flatbuffers::uoffset_t>&
        offsets;
    const typename corpus_type::mapped_type& corpus_value;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) const {
      if constexpr (std::is_same_v<T, std::string>) {
        auto& domain = self.GetCachedDomain<T>(field);
        auto user_value = domain.GetValue(corpus_value);
        if (user_value.has_value()) {
          auto offset =
              builder.CreateString(user_value->data(), user_value->size()).o;
          offsets.insert({field->id(), offset});
        }
      }
    }
  };

  // Create complete table: store "inline fields" values inline, and store just
  // offsets for "out-of-line fields". See `BuildTable` for details.
  struct TableBuilderVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    flatbuffers::FlatBufferBuilder& builder;
    const absl::flat_hash_map<typename corpus_type::key_type,
                              flatbuffers::uoffset_t>& offsets;
    const typename corpus_type::value_type::second_type& corpus_value;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) const {
      if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T> ||
                    is_flatbuffers_enum_tag_v<T>) {
        auto& domain = self.GetCachedDomain<T>(field);
        auto v = domain.GetValue(corpus_value);
        if (!v) {
          return;
        }
        // Store "inline field" value inline.
        builder.AddElement(field->offset(), v.value());
      } else if constexpr (std::is_same_v<T, std::string>) {
        // "Out-of-line field". Store just offset.
        if (auto it = offsets.find(field->id()); it != offsets.end()) {
          builder.AddOffset(
              field->offset(),
              flatbuffers::Offset<flatbuffers::String>(it->second));
        }
      }
    }
  };

  struct ParseVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    const IRObject& obj;
    std::optional<GenericDomainCorpusType>& out;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) {
      out = self.GetCachedDomain<T>(field).ParseCorpus(obj);
    }
  };

  struct ValidateVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    const GenericDomainCorpusType& corpus_value;
    absl::Status& out;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) {
      auto& domain = self.GetCachedDomain<T>(field);
      out = domain.ValidateCorpusValue(corpus_value);
      if (!out.ok()) {
        out = Prefix(out, absl::StrCat("Invalid value for field ",
                                       field->name()->str()));
      }
    }
  };

  struct InitializeVisitor {
    FlatbuffersTableUntypedDomainImpl& self;
    absl::BitGenRef prng;
    corpus_type& val;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) {
      auto& domain = self.GetCachedDomain<T>(field);
      val[field->id()] = domain.Init(prng);
    }
  };

  struct CountNumberOfMutableFieldsVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    uint64_t& total_weight;
    corpus_type& val;
    bool only_shrink = false;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) const {
      if (!self.IsSupportedField(field)) return;
      if (only_shrink && !val.contains(field->id())) return;

      // Add the weight of the field itself.
      total_weight += 1;

      auto& domain = self.GetCachedDomain<T>(field);
      if (auto it = val.find(field->id()); it != val.end()) {
        // Add the weight of the field corpus.
        total_weight += domain.CountNumberOfFields(it->second);
      }
    }
  };

  struct MutateVisitor {
    FlatbuffersTableUntypedDomainImpl& self;
    absl::BitGenRef prng;
    const domain_implementor::MutationMetadata& metadata;
    bool only_shrink;
    corpus_type& corpus_value;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) {
      auto& domain = self.GetCachedDomain<T>(field);
      auto it = corpus_value.find(field->id());
      if (it == corpus_value.end()) {
        if (only_shrink) return;
        it = corpus_value.try_emplace(field->id(), domain.Init(prng)).first;
      }
      domain.Mutate(it->second, prng, metadata, only_shrink);
    }
  };

  struct Printer {
    const FlatbuffersTableUntypedDomainImpl& self;

    void PrintCorpusValue(const corpus_type& value,
                          domain_implementor::RawSink out,
                          domain_implementor::PrintMode mode) const {
      std::vector<typename corpus_type::key_type> field_ids;
      field_ids.reserve(value.size());
      for (const auto& [id, _] : value) {
        field_ids.push_back(id);
      }
      // Sort the field ids to make the output deterministic.
      std::sort(field_ids.begin(), field_ids.end());

      absl::Format(out, "{");
      bool first = true;
      for (const auto id : field_ids) {
        if (!first) {
          absl::Format(out, ", ");
        }
        const reflection::Field* absl_nullable field = self.GetFieldById(id);
        if (field == nullptr) {
          absl::Format(out, "<unknown field: %d>", id);
        } else {
          VisitFlatbufferField(field,
                               PrinterVisitor{self, value.at(id), out, mode});
        }
        first = false;
      }
      absl::Format(out, "}");
    }
  };

  struct PrinterVisitor {
    const FlatbuffersTableUntypedDomainImpl& self;
    const GenericDomainCorpusType& val;
    domain_implementor::RawSink out;
    domain_implementor::PrintMode mode;

    template <typename T>
    void Visit(const reflection::Field* absl_nonnull field) const {
      auto& domain = self.GetCachedDomain<T>(field);
      absl::Format(out, "%s: ", field->name()->str());
      domain_implementor::PrintValue(domain, val, out, mode);
    }
  };
};

// Domain factory for flatbuffers fields.
template <typename T>
auto GetDefaultDomain(const reflection::Schema* absl_nonnull schema,
                      const reflection::Field* absl_nonnull field) {
  // Used to satisfy the compiler return type deduction rules.
  auto placeholder = Arbitrary<bool>();

  if constexpr (std::is_same_v<T, FlatbuffersArrayTag>) {
    // TODO: support arrays.
    return placeholder;
  } else if constexpr (is_flatbuffers_enum_tag_v<T>) {
    auto enum_object = schema->enums()->Get(field->type()->index());
    return FlatbuffersEnumDomainImpl<typename T::type>(enum_object);
  } else if constexpr (std::is_same_v<T, FlatbuffersObjTag>) {
    // TODO: support objects.
    return placeholder;
  } else if constexpr (std::is_same_v<T, FlatbuffersUnionTag>) {
    // TODO: support unions.
    return placeholder;
  } else if constexpr (std::is_same_v<T, FlatbuffersVectorTag>) {
    // TODO: support vectors.
    return placeholder;
  } else {
    return Arbitrary<T>();
  }
}

// Corpus type for the table domain
struct FlatbuffersTableDomainCorpusType {
  // Map of field ids to field values.
  typename FlatbuffersTableUntypedDomainImpl::corpus_type untyped_corpus;
  // Serialized flatbuffer.
  mutable std::vector<uint8_t> buffer;
};

// Domain implementation for flatbuffers generated table classes.
// The corpus type is a pair of:
// - A map of field ids to field values.
// - The serialized buffer of the table.
template <typename T>
class FlatbuffersTableDomainImpl
    : public fuzztest::domain_implementor::DomainBase<
          /*Derived=*/FlatbuffersTableDomainImpl<T>,
          /*ValueType=*/const T*,
          /*CorpusType=*/FlatbuffersTableDomainCorpusType> {
 public:
  using typename FlatbuffersTableDomainImpl::DomainBase::corpus_type;
  using typename FlatbuffersTableDomainImpl::DomainBase::value_type;

  FlatbuffersTableDomainImpl() {
    flatbuffers::Verifier verifier(T::BinarySchema::data(),
                                   T::BinarySchema::size());
    FUZZTEST_INTERNAL_CHECK(reflection::VerifySchemaBuffer(verifier),
                            "Invalid schema for flatbuffers table.");
    auto schema = reflection::GetSchema(T::BinarySchema::data());
    auto table_object =
        schema->objects()->LookupByKey(T::GetFullyQualifiedName());
    inner_ = FlatbuffersTableUntypedDomainImpl{schema, table_object};
  }

  // Initializes the table with random values.
  corpus_type Init(absl::BitGenRef prng) {
    if (auto seed = this->MaybeGetRandomSeed(prng)) {
      return *seed;
    }
    // Create new map of field ids to field values
    auto val = inner_->Init(prng);
    // Return corpus value: pair of the map and the serialized buffer.
    return FlatbuffersTableDomainCorpusType{val, {}};
  }

  // Returns the number of fields in the table.
  uint64_t CountNumberOfFields(corpus_type& val) {
    return inner_->CountNumberOfFields(val.untyped_corpus);
  }

  // Mutates the given corpus value.
  void Mutate(corpus_type& val, absl::BitGenRef prng,
              const domain_implementor::MutationMetadata& metadata,
              bool only_shrink) {
    // Modify values in the map.
    inner_->Mutate(val.untyped_corpus, prng, metadata, only_shrink);
  }

  // Converts corpus value into the exact flatbuffer.
  value_type GetValue(const corpus_type& value) const {
    value.buffer = BuildBuffer(value.untyped_corpus);
    return flatbuffers::GetRoot<T>(value.buffer.data());
  }

  // Creates corpus value from the exact flatbuffer.
  std::optional<corpus_type> FromValue(const value_type& value) const {
    auto val = inner_->FromValue((const flatbuffers::Table*)value);
    if (!val.has_value()) return std::nullopt;
    return std::optional(
        FlatbuffersTableDomainCorpusType{*val, BuildBuffer(*val)});
  }

  // Returns the printer for the table.
  auto GetPrinter() const { return Printer{*inner_}; }

  // Returns the parsed corpus value.
  std::optional<corpus_type> ParseCorpus(const IRObject& obj) const {
    auto val = inner_->ParseCorpus(obj);
    if (!val.has_value()) return std::nullopt;
    return std::optional(
        FlatbuffersTableDomainCorpusType{*val, BuildBuffer(*val)});
  }

  // Returns the serialized corpus value.
  IRObject SerializeCorpus(const corpus_type& corpus_value) const {
    return inner_->SerializeCorpus(corpus_value.untyped_corpus);
  }

  // Returns the status of the given corpus value.
  absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const {
    return inner_->ValidateCorpusValue(corpus_value.untyped_corpus);
  }

 private:
  std::optional<FlatbuffersTableUntypedDomainImpl> inner_;

  struct Printer {
    const FlatbuffersTableUntypedDomainImpl& inner;

    void PrintCorpusValue(const corpus_type& value,
                          domain_implementor::RawSink out,
                          domain_implementor::PrintMode mode) const {
      inner.GetPrinter().PrintCorpusValue(value.untyped_corpus, out, mode);
    }
  };

  std::vector<uint8_t> BuildBuffer(
      const corpus_type_t<FlatbuffersTableUntypedDomainImpl>& val) const {
    flatbuffers::FlatBufferBuilder builder;
    auto offset = inner_->BuildTable(val, builder);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(offset));
    auto buffer =
        std::vector<uint8_t>(builder.GetBufferPointer(),
                             builder.GetBufferPointer() + builder.GetSize());
    return buffer;
  }
};

template <typename T>
class ArbitraryImpl<const T*, std::enable_if_t<is_flatbuffers_table_v<T>>>
    : public FlatbuffersTableDomainImpl<T> {};

}  // namespace fuzztest::internal
#endif  // FUZZTEST_FUZZTEST_INTERNAL_DOMAINS_FLATBUFFERS_DOMAIN_IMPL_H_
