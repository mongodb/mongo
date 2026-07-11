// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"

namespace mongo::stage_builder {

// The signature of an expression is the set of all the types that the Value produced at runtime can
// assume (including TypeTags::Nothing).
struct TypeSignature {
    using MaskType = uint64_t;
    struct AllTypesTag {};

    // For all valid TypeSignatures, the highest bit of the mask will always be 0.
    static constexpr unsigned int highBitIdx = 8 * sizeof(MaskType) - 1;

    static_assert(8 * sizeof(MaskType) > static_cast<size_t>(sbe::value::TypeTags::TypeTagsMax));

    // Predefined constants for common types.
    static TypeSignature kAnyType, kBlockType, kCellType, kAnyScalarType, kAnyBSONType, kArrayType,
        kBooleanType, kDateTimeType, kNothingType, kNullishType, kNumericType, kObjectType,
        kStringType, kCollatableType;

    TypeSignature() noexcept = default;

    TypeSignature(MaskType mask) noexcept : typesMask(mask) {}

    TypeSignature(AllTypesTag) noexcept
        : typesMask((1ull << static_cast<size_t>(sbe::value::TypeTags::TypeTagsMax)) - 1u) {}

    bool operator==(const TypeSignature& o) const {
        return o.typesMask == typesMask;
    }

    // Return whether this signature is a strict subset of the other signature.
    bool isSubset(TypeSignature other) const {
        return (typesMask & other.typesMask) == typesMask;
    }
    // Return whether this signature shares at least one type with the other signature.
    bool containsAny(TypeSignature other) const {
        return (typesMask & other.typesMask) != 0;
    }
    // Return whether no type is encoded in the signature.
    bool isEmpty() const {
        return typesMask == 0;
    }
    // Return a new signature containing all the types of this signature plus the ones from the
    // other signature.
    TypeSignature include(TypeSignature other) const {
        return TypeSignature{typesMask | other.typesMask};
    }
    // Return a new signature containing all the types of this signature minus the ones from the
    // other signature.
    TypeSignature exclude(TypeSignature other) const {
        return TypeSignature{typesMask & ~other.typesMask};
    }
    // Return a new signature containing all the types in common between this signature and the
    // other signature.
    TypeSignature intersect(TypeSignature other) const {
        return TypeSignature{typesMask & other.typesMask};
    }
    // Return whether all the types in this signature can be safely compared with all the types in
    // the other signature.
    bool canCompareWith(TypeSignature other) const;

    std::string debugString() const;

    // Simple bitmask using one bit for each enum in the TypeTags definition.
    MaskType typesMask = 0;
};

// An alternative to 'boost::optional<TypeSignature>' that is more compact.
class OptTypeSignature {
public:
    using MaskType = TypeSignature::MaskType;

    // For all valid TypeSignatures, the highest bit of the mask will always be 0. For the
    // special "sentinel" value ('sentinelMaskVal'), the highest bit of the mask is set to 1.
    static constexpr MaskType sentinelMaskVal = 1ull << TypeSignature::highBitIdx;

    OptTypeSignature() noexcept = default;

    OptTypeSignature(TypeSignature ts) noexcept : _ts(ts) {}

    OptTypeSignature(boost::optional<TypeSignature> ts) noexcept
        : _ts(ts ? *ts : TypeSignature{sentinelMaskVal}) {}

    operator bool() const noexcept {
        return has_value();
    }
    TypeSignature operator*() const {
        tassert(8455818, "Expected OptTypeSignature to have value", has_value());
        return _ts;
    }

    bool has_value() const {
        return _ts.typesMask != sentinelMaskVal;
    }
    void reset() {
        _ts = TypeSignature{sentinelMaskVal};
    }
    boost::optional<TypeSignature> get() const noexcept {
        return has_value() ? boost::make_optional(_ts) : boost::none;
    }
    void set(const OptTypeSignature& ots) noexcept {
        *this = ots;
    }

private:
    TypeSignature _ts{sentinelMaskVal};
};

// Return the signature corresponding to the given SBE type.
TypeSignature getTypeSignature(sbe::value::TypeTags type);

template <typename Head, typename... Tail>
TypeSignature getTypeSignature(Head type, Tail... tail) {
    return getTypeSignature(type).include(getTypeSignature(tail...));
}

// Return the set of SBE types encoded in the provided signature that can be stored in a BSON
// object.
std::vector<sbe::value::TypeTags> getBSONTypesFromSignature(TypeSignature signature);

}  // namespace mongo::stage_builder
