/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TypedObjectPrediction_h
#define jit_TypedObjectPrediction_h

#include "builtin/TypedObject.h"
#include "jit/JitAllocPolicy.h"

namespace js {
namespace jit {

// A TypedObjectPrediction summarizes what we know about the type of a
// typed object at a given point (if anything). The prediction will
// begin as precise as possible and degrade to less precise as more
// typed object types are merged using |addDescr()|.
//
// To create a TypedObjectPrediction from TI, one initially creates an
// empty prediction using the |TypedObjectPrediction()| constructor,
// and then invokes |addDescr()| with the prototype of each typed
// object. The prediction will automatically downgrade to less and
// less specific settings as needed. Note that creating a prediction
// in this way can never yield precise array dimensions, since TI only
// tracks the prototype.
//
// TypedObjectPredictions can also result from other predictions using
// the query methods (e.g., |arrayElementType()|). In those cases, the
// precise array dimensions may be known.
//
// To query a prediction, you must first check whether it is "useless"
// using |isUseless()|. If this is true, there is no usable
// information to be extracted. Otherwise, you can inquire after the
// |kind()| of the data (struct, array, etc) and from there make more
// specific queries.
class TypedObjectPrediction {
  public:
    enum PredictionKind {
        // No data.
        Empty,

        // Inconsistent data.
        Inconsistent,

        // Multiple different struct types flow into the same location,
        // but they share fields in common. Prefix indicates that the first
        // N fields of some struct type are known to be valid. This occurs
        // in a subtyping scenario.
        Prefix,

        // The TypeDescr of the value is known. This is the most specific
        // possible value and includes precise array bounds.
        Descr
    };

    struct PrefixData {
        const StructTypeDescr* descr;
        size_t fields;
    };

    union Data {
        const TypeDescr* descr;
        PrefixData prefix;
    };

  private:
    PredictionKind kind_;
    Data data_;

    PredictionKind predictionKind() const {
        return kind_;
    }

    void markInconsistent() {
        kind_ = Inconsistent;
    }

    const TypeDescr& descr() const {
        MOZ_ASSERT(predictionKind() == Descr);
        return *data_.descr;
    }

    const PrefixData& prefix() const {
        MOZ_ASSERT(predictionKind() == Prefix);
        return data_.prefix;
    }

    void setDescr(const TypeDescr& descr) {
        kind_ = Descr;
        data_.descr = &descr;
    }

    void setPrefix(const StructTypeDescr& descr, size_t fields) {
        kind_ = Prefix;
        data_.prefix.descr = &descr;
        data_.prefix.fields = fields;
    }

    void markAsCommonPrefix(const StructTypeDescr& descrA,
                            const StructTypeDescr& descrB,
                            size_t max);

    template<typename T>
    typename T::Type extractType() const;

    bool hasFieldNamedPrefix(const StructTypeDescr& descr,
                             size_t fieldCount,
                             jsid id,
                             size_t* fieldOffset,
                             TypedObjectPrediction* out,
                             size_t* index) const;

  public:

    ///////////////////////////////////////////////////////////////////////////
    // Constructing a prediction. Generally, you start with an empty
    // prediction and invoke addDescr() repeatedly.

    TypedObjectPrediction() {
        kind_ = Empty;
    }

    explicit TypedObjectPrediction(const TypeDescr& descr) {
        setDescr(descr);
    }

    TypedObjectPrediction(const StructTypeDescr& descr, size_t fields) {
        setPrefix(descr, fields);
    }

    void addDescr(const TypeDescr& descr);

    ///////////////////////////////////////////////////////////////////////////
    // Queries that are always valid.

    bool isUseless() const {
        return predictionKind() == Empty || predictionKind() == Inconsistent;
    }

    // Determines whether we can predict the prototype for the typed
    // object instance. Returns null if we cannot or if the typed
    // object is of scalar/reference kind, in which case instances are
    // not objects and hence do not have a (publicly available)
    // prototype.
    const TypedProto* getKnownPrototype() const;

    ///////////////////////////////////////////////////////////////////////////
    // Queries that are valid if not useless.

    type::Kind kind() const;

    bool ofArrayKind() const;

    // Returns true if the size of this typed object is statically
    // known and sets |*out| to that size. Otherwise returns false.
    //
    // The size may not be statically known if (1) the object is
    // an array whose dimensions are unknown or (2) only a prefix
    // of its type is known.
    bool hasKnownSize(int32_t* out) const;

    //////////////////////////////////////////////////////////////////////
    // Simple operations
    //
    // Only valid when |kind()| is Scalar, Reference, or Simd (as appropriate).

    ScalarTypeDescr::Type scalarType() const;
    ReferenceTypeDescr::Type referenceType() const;
    SimdTypeDescr::Type simdType() const;

    ///////////////////////////////////////////////////////////////////////////
    // Queries valid only for arrays.

    // Returns true if the length of the array is statically known,
    // and sets |*length| appropriately. Otherwise returns false.
    bool hasKnownArrayLength(int32_t* length) const;

    // Returns a prediction for the array element type, if any.
    TypedObjectPrediction arrayElementType() const;

    //////////////////////////////////////////////////////////////////////
    // Struct operations
    //
    // Only valid when |kind() == TypeDescr::Struct|

    // Returns true if the predicted type includes a field named |id|
    // and sets |*fieldOffset|, |*fieldType|, and |*fieldIndex| with
    // the offset (in bytes), type, and index of the field
    // respectively.  Otherwise returns false.
    bool hasFieldNamed(jsid id,
                       size_t* fieldOffset,
                       TypedObjectPrediction* fieldType,
                       size_t* fieldIndex) const;
};

} // namespace jit
} // namespace js

#endif
