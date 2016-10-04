/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/TypedObjectPrediction.h"

using namespace js;
using namespace jit;

static const size_t ALL_FIELDS = SIZE_MAX;

// Sets the prediction to be the common prefix of descrA and descrB,
// considering at most the first max fields.
//
// In the case where the current prediction is a specific struct,
// and we are now seeing a second struct, then descrA and descrB will be
// the current and new struct and max will be ALL_FIELDS.
//
// In the case where the current prediction is already a prefix, and
// we are now seeing an additional struct, then descrA will be the
// current struct and max will be the current prefix length, and
// descrB will be the new struct.
//
// (Note that in general it is not important which struct is passed as
// descrA and which struct is passed as descrB, as the operation is
// symmetric.)
void
TypedObjectPrediction::markAsCommonPrefix(const StructTypeDescr& descrA,
                                          const StructTypeDescr& descrB,
                                          size_t max)
{
    // count is the number of fields in common. It begins as the min
    // of the number of fields from descrA, descrB, and max, and then
    // is decremented as we find uncommon fields.
    if (max > descrA.fieldCount())
        max = descrA.fieldCount();
    if (max > descrB.fieldCount())
        max = descrB.fieldCount();

    size_t i = 0;
    for (; i < max; i++) {
        if (&descrA.fieldName(i) != &descrB.fieldName(i))
            break;
        if (&descrA.fieldDescr(i) != &descrB.fieldDescr(i))
            break;
        MOZ_ASSERT(descrA.fieldOffset(i) == descrB.fieldOffset(i));
    }

    if (i == 0) {
        // empty prefix is not particularly useful.
        markInconsistent();
    } else {
        setPrefix(descrA, i);
    }
}

void
TypedObjectPrediction::addDescr(const TypeDescr& descr)
{
    switch (predictionKind()) {
      case Empty:
        return setDescr(descr);

      case Inconsistent:
        return; // keep same state

      case Descr: {
        if (&descr == data_.descr)
            return; // keep same state

        if (descr.kind() != data_.descr->kind())
            return markInconsistent();

        if (descr.kind() != type::Struct)
            return markInconsistent();

        const StructTypeDescr& structDescr = descr.as<StructTypeDescr>();
        const StructTypeDescr& currentDescr = data_.descr->as<StructTypeDescr>();
        markAsCommonPrefix(structDescr, currentDescr, ALL_FIELDS);
        return;
      }

      case Prefix:
        if (descr.kind() != type::Struct)
            return markInconsistent();

        markAsCommonPrefix(*data_.prefix.descr,
                           descr.as<StructTypeDescr>(),
                           data_.prefix.fields);
        return;
    }

    MOZ_CRASH("Bad predictionKind");
}

type::Kind
TypedObjectPrediction::kind() const
{
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        break;

      case TypedObjectPrediction::Descr:
        return descr().kind();

      case TypedObjectPrediction::Prefix:
        return prefix().descr->kind();
    }

    MOZ_CRASH("Bad prediction kind");
}

bool
TypedObjectPrediction::ofArrayKind() const
{
    switch (kind()) {
      case type::Scalar:
      case type::Reference:
      case type::Simd:
      case type::Struct:
        return false;

      case type::Array:
        return true;
    }

    MOZ_CRASH("Bad kind");
}

bool
TypedObjectPrediction::hasKnownSize(int32_t* out) const
{
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        return false;

      case TypedObjectPrediction::Descr:
        *out = descr().size();
        return true;

      case TypedObjectPrediction::Prefix:
        // We only know a prefix of the struct fields, hence we do not
        // know its complete size.
        return false;

      default:
        MOZ_CRASH("Bad prediction kind");
    }
}

const TypedProto*
TypedObjectPrediction::getKnownPrototype() const
{
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        return nullptr;

      case TypedObjectPrediction::Descr:
        if (descr().is<ComplexTypeDescr>())
            return &descr().as<ComplexTypeDescr>().instancePrototype();
        return nullptr;

      case TypedObjectPrediction::Prefix:
        // We only know a prefix of the struct fields, hence we cannot
        // say for certain what its prototype will be.
        return nullptr;

      default:
        MOZ_CRASH("Bad prediction kind");
    }
}

template<typename T>
typename T::Type
TypedObjectPrediction::extractType() const
{
    MOZ_ASSERT(kind() == T::Kind);
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        break;

      case TypedObjectPrediction::Descr:
        return descr().as<T>().type();

      case TypedObjectPrediction::Prefix:
        break; // Prefixes are always structs, never scalars etc
    }

    MOZ_CRASH("Bad prediction kind");
}

ScalarTypeDescr::Type
TypedObjectPrediction::scalarType() const
{
    return extractType<ScalarTypeDescr>();
}

ReferenceTypeDescr::Type
TypedObjectPrediction::referenceType() const
{
    return extractType<ReferenceTypeDescr>();
}

SimdTypeDescr::Type
TypedObjectPrediction::simdType() const
{
    return extractType<SimdTypeDescr>();
}

bool
TypedObjectPrediction::hasKnownArrayLength(int32_t* length) const
{
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        return false;

      case TypedObjectPrediction::Descr:
        // In later patches, this condition will always be true
        // so long as this represents an array
        if (descr().is<ArrayTypeDescr>()) {
            *length = descr().as<ArrayTypeDescr>().length();
            return true;
        }
        return false;

      case TypedObjectPrediction::Prefix:
        // Prefixes are always structs, never arrays
        return false;

      default:
        MOZ_CRASH("Bad prediction kind");
    }
}

TypedObjectPrediction
TypedObjectPrediction::arrayElementType() const
{
    MOZ_ASSERT(ofArrayKind());
    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        break;

      case TypedObjectPrediction::Descr:
        return TypedObjectPrediction(descr().as<ArrayTypeDescr>().elementType());

      case TypedObjectPrediction::Prefix:
        break; // Prefixes are always structs, never arrays
    }
    MOZ_CRASH("Bad prediction kind");
}

bool
TypedObjectPrediction::hasFieldNamedPrefix(const StructTypeDescr& descr,
                                           size_t fieldCount,
                                           jsid id,
                                           size_t* fieldOffset,
                                           TypedObjectPrediction* out,
                                           size_t* index) const
{
    // Find the index of the field |id| if any.
    if (!descr.fieldIndex(id, index))
        return false;

    // Check whether the index falls within our known safe prefix.
    if (*index >= fieldCount)
        return false;

    // Load the offset and type.
    *fieldOffset = descr.fieldOffset(*index);
    *out = TypedObjectPrediction(descr.fieldDescr(*index));
    return true;
}

bool
TypedObjectPrediction::hasFieldNamed(jsid id,
                                     size_t* fieldOffset,
                                     TypedObjectPrediction* fieldType,
                                     size_t* fieldIndex) const
{
    MOZ_ASSERT(kind() == type::Struct);

    switch (predictionKind()) {
      case TypedObjectPrediction::Empty:
      case TypedObjectPrediction::Inconsistent:
        return false;

      case TypedObjectPrediction::Descr:
        return hasFieldNamedPrefix(
            descr().as<StructTypeDescr>(), ALL_FIELDS,
            id, fieldOffset, fieldType, fieldIndex);

      case TypedObjectPrediction::Prefix:
        return hasFieldNamedPrefix(
            *prefix().descr, prefix().fields,
            id, fieldOffset, fieldType, fieldIndex);

      default:
        MOZ_CRASH("Bad prediction kind");
    }
}
