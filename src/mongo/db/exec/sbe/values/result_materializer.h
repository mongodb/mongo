// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_listset.h"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {

/**
 * Converts the SBE value 'view' into a Value that does not reference the SBE value's memory.
 */
Value convertToValue(value::TagValueView view);

/**
 * What the executor does with one top-level field of an SBE plan's result base object when it
 * builds the query result. These match the top-level Keep/Drop/SetArg/AddArg actions of a
 * MakeObjSpec with NonObjInputBehavior::kNewObj and a traversal depth of 0.
 */
enum class ResultFieldAction { kKeep, kDrop, kSet, kAdd };

struct ResultField {
    ResultField(std::string name, ResultFieldAction action, value::SlotAccessor* accessor = nullptr)
        : name(std::move(name)),
          action(action),
          accessor(accessor),
          bsonAccessor(accessor ? accessor->as<value::BSONObjValueAccessor>() : nullptr) {}

    std::string name;
    ResultFieldAction action;
    value::SlotAccessor* accessor;  // kSet/kAdd
    // accessor->as<>() resolved once here. A scan or fetch record slot, so it always holds a
    // bsonObject when read.
    value::BSONObjValueAccessor* bsonAccessor;
};

/**
 * A field of an SBE Object result base.
 */
using ObjectField = std::pair<std::string_view, value::TagValueView>;

/**
 * Walks the fields of a result base. item() is the current field, a BSONElement or an ObjectField.
 */
template <typename Cursor>
concept ResultBaseCursor = requires(Cursor& cursor) {
    { cursor.atEnd() } -> std::same_as<bool>;
    cursor.moveNext();
    { cursor.fieldName() } -> std::same_as<std::string_view>;
    cursor.item();
};

template <typename Writer>
concept ResultWriter =
    requires(Writer& writer, const BSONElement& elem, const ObjectField& field, size_t fieldIdx) {
        writer.append(elem);
        writer.append(field);
        writer.appendField(fieldIdx);
    };

/**
 * Builds the result of an SBE plan from its root ResultInfo: a base object slot and a list of
 * top-level field changes, each read from its own slot. This lets the executor produce a result
 * without the plan materializing it.
 *
 * 'closed' means fields of the base that are not listed are dropped; otherwise they are kept.
 * Listed fields are processed in order: kKeep keeps a field of a closed result, kDrop drops a field
 * of an open result, kSet writes the field's value in place (appending it if the base does not have
 * the field), and kAdd removes the field from its position in the base and appends the value. A
 * Nothing value removes the field. The BSON output is byte-identical to makeBsonObj() with the
 * equivalent MakeObjSpec.
 *
 * For example, over the base {a: 1, b: 2, c: 3}, an open result keeps the unlisted fields:
 *
 *   []                      -> {a: 1, b: 2, c: 3}
 *   [a: kKeep]              -> {a: 1, b: 2, c: 3}  (kKeep has no effect)
 *   [b: kDrop]              -> {a: 1, c: 3}
 *   [b: kSet 20]            -> {a: 1, b: 20, c: 3}
 *   [d: kSet 4]             -> {a: 1, b: 2, c: 3, d: 4}
 *   [a: kAdd 10]            -> {b: 2, c: 3, a: 10}
 *   [b: kSet Nothing]       -> {a: 1, c: 3}
 *   [e: kAdd 5, d: kSet 4]  -> {a: 1, b: 2, c: 3, e: 5, d: 4}  (appended in list order)
 *
 * and a closed result drops them:
 *
 *   []                      -> {}
 *   [a: kDrop]              -> {}  (kDrop has no effect)
 *   [c: kKeep, a: kKeep]    -> {a: 1, c: 3}  (base order, not list order)
 *   [c: kKeep, a: kSet 10]  -> {a: 10, c: 3}
 *   [c: kKeep, a: kAdd 10]  -> {c: 3, a: 10}
 *   [d: kSet 4]             -> {d: 4}
 *
 * Object slots (the base when it is not walked, and object-valued fields) are copied at most once
 * per result:
 *  - A BSONObjValueAccessor (the record slot of a scan or fetch) shares one owned copy of the
 * record through getOwnedBSONObj(), so consecutive results over the same record don't copy it
 * again.
 *  - Any other accessor's value is copied once per result, even when the stage owns it. Moving it
 *    out would leave the stage viewing a buffer it no longer owns, and stages expose such values
 *    again without re-owning them.
 *  - An SBE Object value is encoded to BSON once.
 *
 * Every Document returned is owned.
 */
class ResultMaterializer {
public:
    ResultMaterializer(value::SlotAccessor* base, bool closed, std::vector<ResultField> fields);

    bool hasChanges() const {
        return _closed || !_fields.empty();
    }

    /**
     * Returns the result as BSON. Without changes this is the base object itself (a view when
     * 'mustOwn' is false); with changes the result is always a newly built, owned object. Can
     * return oversized BSON, the caller must check the size if needed.
     */
    BSONObj toBson(bool mustOwn);

    Document toDocument();

private:
    struct BsonWriter;
    struct DocumentWriter;

    Value fieldValue(size_t fieldIdx) const;

    /**
     * Mirrors the top-level part of vm::MakeObjImpl::produceObj() for the base and the changes,
     * passing each field of the result to 'writer' in order: kept base fields to append() and set
     * or added fields to appendField(). The result may exceed the default BSON size limit.
     */
    template <ResultWriter Writer>
    void produce(Writer& writer);

    template <ResultWriter Writer, ResultBaseCursor Cursor>
    void produce(Writer& writer, Cursor cursor);

    value::SlotAccessor* _base;
    value::BSONObjValueAccessor* _baseBsonAccessor;
    bool _closed;
    std::vector<ResultField> _fields;
    StringListSet _fieldNames;

    // The number of base fields whose action is not the default one.
    size_t _numFieldsToSearchFor = 0;

    bool _canShareBase = false;
};

}  // namespace mongo::sbe
