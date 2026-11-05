// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/result_materializer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/makeobj_cursors.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

#include <algorithm>

#include <absl/container/inlined_vector.h>

namespace mongo::sbe {
namespace {
Document convertToDocument(const value::Object& obj);
}  // namespace

Value convertToValue(value::TagValueView view) {
    auto [tag, val] = view;
    switch (tag) {
        case value::TypeTags::Nothing:
            return Value();

        case value::TypeTags::NumberInt32:
            return Value(value::bitcastTo<int32_t>(val));

        case value::TypeTags::NumberInt64:
            return Value(value::bitcastTo<long long>(val));

        case value::TypeTags::NumberDouble:
            return Value(value::bitcastTo<double>(val));

        case value::TypeTags::NumberDecimal:
            return Value(value::bitcastTo<Decimal128>(val));

        case value::TypeTags::Date:
            return Value(Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(val)));

        case value::TypeTags::Timestamp:
            return Value(Timestamp(value::bitcastTo<uint64_t>(val)));

        case value::TypeTags::Boolean:
            return Value(value::bitcastTo<bool>(val));

        case value::TypeTags::Null:
            return Value(BSONNULL);

        case value::TypeTags::StringSmall:
        case value::TypeTags::StringBig:
        case value::TypeTags::bsonString:
            return Value(value::getStringOrSymbolView(tag, val));

        case value::TypeTags::bsonSymbol:
            return Value(BSONSymbol(value::getStringOrSymbolView(tag, val)));

        case value::TypeTags::Array:
        case value::TypeTags::ArraySet:
        case value::TypeTags::ArrayMultiSet: {
            std::vector<Value> vals;
            auto enumerator = value::ArrayEnumerator{tag, val};
            if (auto size = enumerator.trySize()) {
                vals.reserve(*size);
            }
            while (!enumerator.atEnd()) {
                vals.push_back(convertToValue(enumerator.getViewOfValue()));
                enumerator.advance();
            }
            return Value(std::move(vals));
        }

        case value::TypeTags::Object:
            return Value(convertToDocument(*value::getObjectView(val)));

        case value::TypeTags::ObjectId:
            return Value(OID::from(value::getObjectIdView(val)->data()));

        case value::TypeTags::MinKey:
            return Value(kMinBSONKey);

        case value::TypeTags::MaxKey:
            return Value(kMaxBSONKey);

        case value::TypeTags::bsonObject:
            return Value(BSONObj{value::bitcastTo<const char*>(val)});

        case value::TypeTags::bsonArray:
            return Value(BSONArray{BSONObj{value::bitcastTo<const char*>(val)}});

        case value::TypeTags::bsonObjectId:
            return Value(OID::from(value::bitcastTo<const char*>(val)));

        case value::TypeTags::bsonBinData:
            return Value(BSONBinData(value::getBSONBinData(tag, val),
                                     value::getBSONBinDataSize(tag, val),
                                     value::getBSONBinDataSubtype(tag, val)));

        case value::TypeTags::bsonUndefined:
            return Value(BSONUndefined);

        case value::TypeTags::bsonRegex: {
            auto regex = value::getBsonRegexView(val);
            return Value(BSONRegEx(regex.pattern, regex.flags));
        }

        case value::TypeTags::bsonJavascript:
            return Value(BSONCode(value::getBsonJavascriptView(val)));

        case value::TypeTags::bsonDBPointer: {
            auto dbptr = value::getBsonDBPointerView(val);
            return Value(BSONDBRef(dbptr.ns, OID::from(dbptr.id)));
        }

        case value::TypeTags::bsonCodeWScope: {
            auto bcws = value::getBsonCodeWScopeView(val);
            return Value(BSONCodeWScope(bcws.code, BSONObj(bcws.scope)));
        }

        default:
            MONGO_UNREACHABLE;
    }
}

namespace {
Document convertToDocument(const value::Object& obj) {
    MutableDocument doc(obj.size());
    for (size_t idx = 0; idx < obj.size(); ++idx) {
        doc.addField(obj.field(idx), convertToValue(obj.getAt(idx)));
    }
    return doc.freeze();
}

BSONObj encodeObject(value::Object* obj) {
    BSONObjBuilder bob;
    bson::convertToBsonObj(bob, obj);
    return bob.obj<BSONObj::LargeSizeTrait>();
}

/**
 * The object 'obj' as BSON, without copies.
 */
BSONObj coerceToBson(value::TagValueView obj) {
    switch (obj.tag) {
        case value::TypeTags::bsonObject:
            return BSONObj(value::bitcastTo<const char*>(obj.value), BSONObj::LargeSizeTrait{});
        case value::TypeTags::Nothing:
            return BSONObj();
        default:
            tasserted(13773200, "Expected a bsonObject or Nothing");
    }
}

/**
 * The object 'obj' as a view if possible, or a copy otherwise.
 */
BSONObj bsonViewOrCopy(value::TagValueView obj) {
    // If we have sbe::Object, we'll have to translate this to BSON (copy).
    if (obj.tag == value::TypeTags::Object) {
        return encodeObject(value::getObjectView(obj.value));
    }
    // Other cases are easily coercible.
    return coerceToBson(obj);
}

/**
 * The object held in the accessor as owned BSON, which may require copies.
 */
BSONObj asOwnedBson(value::SlotAccessor* accessor, value::BSONObjValueAccessor* bsonAccessor) {
    // For a BSON accessor, getting owned BSON is cheap (reference count bump).
    if (bsonAccessor) {
        return bsonAccessor->getOwnedBSONObj();
    }
    value::TagValueView obj = accessor->getViewOfValue();
    // For an sbe::Object, we'll have to iterate through the object and copy it to
    // BSON.
    if (obj.tag == value::TypeTags::Object) {
        return encodeObject(value::getObjectView(obj.value)).getOwned();
    }
    // Otherwise, the type must be bsonObject or Nothing, which are easily owned
    // through coercing to BSON plus .getOwned()
    return coerceToBson(obj).getOwned();
}

StringListSet createFieldNamesListSet(const std::vector<ResultField>& fields) {
    std::vector<std::string> names;
    names.reserve(fields.size());
    for (const auto& field : fields) {
        names.push_back(field.name);
    }
    return StringListSet(std::move(names));
}

size_t computeNumFieldsToSearchFor(bool closed, const std::vector<ResultField>& fields) {
    size_t numFieldsToSearchFor = 0;
    for (const auto& field : fields) {
        if (closed) {
            // For closed, drop and add have no effect on how many fields we search for in the base
            // object.
            switch (field.action) {
                case ResultFieldAction::kKeep:
                case ResultFieldAction::kSet:
                    ++numFieldsToSearchFor;
                    break;
                case ResultFieldAction::kDrop:
                case ResultFieldAction::kAdd:
                    break;
            }
        } else {
            // For open, keep has no effect on how many fields we search for in the base object.
            switch (field.action) {
                case ResultFieldAction::kDrop:
                case ResultFieldAction::kSet:
                case ResultFieldAction::kAdd:
                    ++numFieldsToSearchFor;
                    break;
                case ResultFieldAction::kKeep:
                    break;
            }
        }
    }
    return numFieldsToSearchFor;
}
}  // namespace

ResultMaterializer::ResultMaterializer(value::SlotAccessor* base,
                                       bool closed,
                                       std::vector<ResultField> fields)
    : _base(base),
      _baseBsonAccessor(base ? base->as<value::BSONObjValueAccessor>() : nullptr),
      _closed(closed),
      _fields(std::move(fields)),
      _fieldNames(createFieldNamesListSet(_fields)) {
    tassert(13773202, "ResultMaterializer requires a base slot", _base);

    _numFieldsToSearchFor = computeNumFieldsToSearchFor(closed, _fields);
    // When building a document with `_closed=true`, we're forced to iterate through the base
    // so we only keep the requested fields. However if we have open results, we're keeping
    // fields by default and can potentially use a BSON _base as the underlying BSON for the
    // Document, and apply our changes on top:
    //    kSet  -> set the field to the given value in the Document's field cache
    //    kDrop -> set the field to `Value()` (missing) in the field cache, same effect as actually
    //             deleting the field.
    // However, this does not work if we have any kAdd fields, which requires the field to be moved
    // to the end of the object. This means we cannot just add our changes to the field cache, since
    // that would not affect the final field order. Document reads it's base BSON to determine the
    // field order to output, so the base needs to be changed.
    _canShareBase = !closed && std::ranges::none_of(_fields, [](const ResultField& field) {
        return field.action == ResultFieldAction::kAdd;
    });
    for (const auto& field : _fields) {
        if (field.action == ResultFieldAction::kSet || field.action == ResultFieldAction::kAdd) {
            tassert(13773201, "Expected a slot for a set or added result field", field.accessor);
        }
    }
}

Value ResultMaterializer::fieldValue(size_t fieldIdx) const {
    const auto& field = _fields[fieldIdx];
    auto view = field.accessor->getViewOfValue();
    if (view.tag == value::TypeTags::bsonObject) {
        return Value(Document{asOwnedBson(field.accessor, field.bsonAccessor)});
    }
    return convertToValue(view);
}

/**
 * Appends the fields of the result to a BSONObjBuilder.
 */
struct ResultMaterializer::BsonWriter {
    explicit BsonWriter(const ResultMaterializer& materializer) : materializer(materializer) {}

    void append(const BSONElement& elem) {
        bob.append(elem);
    }

    void append(const ObjectField& field) {
        const auto& [name, view] = field;
        bson::appendValueToBsonObj(bob, name, view.tag, view.value);
    }

    void appendField(size_t fieldIdx) {
        const auto& field = materializer._fields[fieldIdx];
        auto [tag, val] = field.accessor->getViewOfValue();
        bson::appendValueToBsonObj(bob, field.name, tag, val);
    }

    const ResultMaterializer& materializer;
    BSONObjBuilder bob;
};

/**
 * Appends the fields of the result to a MutableDocument. Every value added is owned, so the base
 * may be a view.
 */
struct ResultMaterializer::DocumentWriter {
    explicit DocumentWriter(const ResultMaterializer& materializer)
        : materializer(materializer), md(materializer._fields.size()) {}

    void append(const BSONElement& elem) {
        md.addField(elem.fieldNameStringData(), Value(elem));
    }

    void append(const ObjectField& field) {
        const auto& [name, view] = field;
        md.addField(name, convertToValue(view));
    }

    void appendField(size_t fieldIdx) {
        auto value = materializer.fieldValue(fieldIdx);
        if (!value.missing()) {
            md.addField(materializer._fields[fieldIdx].name, std::move(value));
        }
    }

    const ResultMaterializer& materializer;
    MutableDocument md;
};

template <ResultWriter Writer>
void ResultMaterializer::produce(Writer& writer) {
    auto base = _base->getViewOfValue();
    if (base.tag == value::TypeTags::Object) {
        produce(writer, vm::ObjectCursor(value::getObjectView(base.value)));
    } else {
        // `Nothing` will return an empty BSON here, and will iterate correctly.
        produce(writer, vm::BsonObjCursor(coerceToBson(base).objdata()));
    }
}

template <ResultWriter Writer, ResultBaseCursor Cursor>
void ResultMaterializer::produce(Writer& writer, Cursor cursor) {
    // 'visitedSetFields' tracks which kSet fields were found in the base, so the remaining ones can
    // be appended at the end.
    absl::InlinedVector<char, 32> visitedSetFields(_fields.size(), 0);

    const auto defaultAction = _closed ? ResultFieldAction::kDrop : ResultFieldAction::kKeep;
    size_t fieldsLeft = _numFieldsToSearchFor;
    for (; !cursor.atEnd() && fieldsLeft > 0; cursor.moveNext()) {
        const size_t fieldIdx = _fieldNames.findPos(cursor.fieldName());
        const auto action =
            fieldIdx != StringListSet::npos ? _fields[fieldIdx].action : defaultAction;

        switch (action) {
            case ResultFieldAction::kDrop:
            case ResultFieldAction::kAdd:
                fieldsLeft -= !_closed;
                break;
            case ResultFieldAction::kKeep:
                fieldsLeft -= _closed;
                writer.append(cursor.item());
                break;
            case ResultFieldAction::kSet:
                --fieldsLeft;
                visitedSetFields[fieldIdx] = 1;
                writer.appendField(fieldIdx);
                break;
        }
    }

    // If the result is open, copy over the base fields that were not searched.
    if (!_closed) {
        for (; !cursor.atEnd(); cursor.moveNext()) {
            writer.append(cursor.item());
        }
    }

    // Append the kSet fields that were not found in the base and all the kAdd fields.
    for (size_t fieldIdx = 0; fieldIdx < _fields.size(); ++fieldIdx) {
        auto action = _fields[fieldIdx].action;
        if ((action == ResultFieldAction::kSet && !visitedSetFields[fieldIdx]) ||
            action == ResultFieldAction::kAdd) {
            writer.appendField(fieldIdx);
        }
    }
}

BSONObj ResultMaterializer::toBson(bool mustOwn) {
    if (!hasChanges()) {
        return mustOwn ? asOwnedBson(_base, _baseBsonAccessor)
                       : bsonViewOrCopy(_base->getViewOfValue());
    }

    BsonWriter writer(*this);
    produce(writer);
    return writer.bob.obj<BSONObj::LargeSizeTrait>();
}

Document ResultMaterializer::toDocument() {
    if (!hasChanges()) {
        auto base = _base->getViewOfValue();
        // We have a base with no changes to apply. If we have an Object, we convert to a Document
        // and return it. If we have BSON, we can create an owned BSON and use that as the base of
        // the document.
        if (base.tag == value::TypeTags::Object) {
            return convertToValue(base).getDocument();
        }
        return Document{asOwnedBson(_base, _baseBsonAccessor)};
    }

    // If we have no kAdd changes and a BSON base, we can create a Document based on the BSON
    // and cheaply add our changes on top in the field cache.
    if (_canShareBase && _base->getViewOfValue().tag == value::TypeTags::bsonObject) {
        MutableDocument md{Document{asOwnedBson(_base, _baseBsonAccessor)}};
        for (size_t fieldIdx = 0; fieldIdx < _fields.size(); ++fieldIdx) {
            const auto& field = _fields[fieldIdx];
            if (field.action == ResultFieldAction::kDrop) {
                md.remove(field.name);
            } else if (field.action == ResultFieldAction::kSet) {
                // A missing value removes the field.
                // TODO SERVER-138519 Check for double copies in `fieldValue`
                md.setField(field.name, fieldValue(fieldIdx));
            }
        }
        return md.freeze();
    }

    DocumentWriter writer(*this);
    produce(writer);
    return writer.md.freeze();
}

}  // namespace mongo::sbe
