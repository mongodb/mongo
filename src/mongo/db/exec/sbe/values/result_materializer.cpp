// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/result_materializer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

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
            // TODO SERVER-138113 add reserve call.
            std::vector<Value> vals;
            auto enumerator = value::ArrayEnumerator{tag, val};
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
    MutableDocument doc;
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
 * The object 'obj' as BSON: a view of the value's memory when it already holds BSON, which is only
 * valid until the slot's value changes. Nothing is the empty object.
 */
BSONObj asBson(value::TagValueView obj) {
    switch (obj.tag) {
        case value::TypeTags::bsonObject:
            return BSONObj(value::bitcastTo<const char*>(obj.value), BSONObj::LargeSizeTrait{});
        case value::TypeTags::Object:
            return encodeObject(value::getObjectView(obj.value));
        case value::TypeTags::Nothing:
            return BSONObj();
        default:
            tasserted(13773200, "Expected an object or Nothing");
    }
}

BSONObj ownObject(value::SlotAccessor* accessor, value::BSONObjValueAccessor* bsonAccessor) {
    if (bsonAccessor) {
        return bsonAccessor->getOwnedBSONObj();
    }
    // Copied even if the stage owns it: a stage that gave its value away by a move can still expose
    // it again later.
    return asBson(accessor->getViewOfValue()).getOwned();
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
    for (const auto& field : _fields) {
        if (field.action == ResultFieldAction::kSet || field.action == ResultFieldAction::kAdd) {
            tassert(13773201, "Expected a slot for a set or added result field", field.accessor);
        }
    }
}

Value ResultMaterializer::fieldValue(size_t fieldIdx) const {
    const auto& field = _fields[fieldIdx];
    auto view = field.accessor->getViewOfValue();
    if (view.tagIn(value::TypeTags::bsonObject, value::TypeTags::Object)) {
        return Value(Document{ownObject(field.accessor, field.bsonAccessor)});
    }
    return convertToValue(view);
}

/**
 * Appends the fields of the result to a BSONObjBuilder.
 */
struct ResultMaterializer::BsonWriter {
    explicit BsonWriter(const ResultMaterializer& materializer) : materializer(materializer) {}

    void appendElem(const BSONElement& elem) {
        bob.append(elem);
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

    void appendElem(const BSONElement& elem) {
        md.addField(elem.fieldNameStringData(), Value(elem));
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
void ResultMaterializer::produce(Writer& writer, const BSONObj& base) {
    // 'visitedSetFields' tracks which kSet fields were found in the base, so the remaining ones can
    // be appended at the end.
    absl::InlinedVector<char, 32> visitedSetFields(_fields.size(), 0);

    const auto defaultAction = _closed ? ResultFieldAction::kDrop : ResultFieldAction::kKeep;
    BSONObjIterator it(base);
    size_t fieldsLeft = _numFieldsToSearchFor;
    while (it.more() && fieldsLeft > 0) {
        auto elem = it.next();
        const size_t fieldIdx = _fieldNames.findPos(elem.fieldNameStringData());
        const auto action =
            fieldIdx != StringListSet::npos ? _fields[fieldIdx].action : defaultAction;

        switch (action) {
            case ResultFieldAction::kDrop:
            case ResultFieldAction::kAdd:
                fieldsLeft -= !_closed;
                break;
            case ResultFieldAction::kKeep:
                fieldsLeft -= _closed;
                writer.appendElem(elem);
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
        while (it.more()) {
            writer.appendElem(it.next());
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
    auto base = _base->getViewOfValue();
    if (!hasChanges()) {
        return mustOwn ? ownObject(_base, _baseBsonAccessor) : asBson(base);
    }

    BsonWriter writer(*this);
    // TODO SERVER-138113 avoid sbe::Object -> BSON translation in this case. We can instead
    // iterate the sbe::Object directly.
    produce(writer, asBson(base));
    return writer.bob.obj<BSONObj::LargeSizeTrait>();
}

Document ResultMaterializer::toDocument() {
    if (!hasChanges()) {
        return Document{ownObject(_base, _baseBsonAccessor)};
    }

    // TODO SERVER-138113 improve result materializer efficiency by detecting cases when
    // we can reuse the bson as the base of the document and build fields in the field cache.
    DocumentWriter writer(*this);
    // TODO SERVER-138113 avoid sbe::Object -> BSON translation in this case. We can instead
    // iterate the sbe::Object directly.
    produce(writer, asBson(_base->getViewOfValue()));
    return writer.md.freeze();
}

}  // namespace mongo::sbe
