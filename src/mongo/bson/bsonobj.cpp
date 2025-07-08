/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/bson/bsonobj.h"

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/generator_extended_canonical_2_0_0.h"
#include "mongo/bson/generator_extended_relaxed_2_0_0.h"
#include "mongo/bson/generator_legacy_strict.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

template <class ObjectIterator>
int compareObjects(const BSONObj& firstObj,
                   const BSONObj& secondObj,
                   const BSONObj& idxKey,
                   BSONObj::ComparisonRulesSet rules,
                   const StringDataComparator* comparator) {
    if (firstObj.isEmpty())
        return secondObj.isEmpty() ? 0 : -1;
    if (secondObj.isEmpty())
        return 1;

    ObjectIterator firstIter(firstObj);
    ObjectIterator secondIter(secondObj);
    ObjectIterator idxKeyIter(idxKey);

    while (true) {
        BSONElement l = firstIter.next();
        BSONElement r = secondIter.next();

        if (l.eoo())
            return r.eoo() ? 0 : -1;
        if (r.eoo())
            return 1;

        auto x = l.woCompare(r, rules, comparator);

        if (idxKeyIter.more() && idxKeyIter.next().number() < 0)
            x = -x;

        if (x != 0)
            return x;
    }

    MONGO_UNREACHABLE;
}

}  // namespace

/* BSONObj ------------------------------------------------------------*/

const BSONObj BSONObj::kEmptyObject;

void BSONObj::_assertInvalid(int maxSize) const {
    StringBuilder ss;
    int os = objsize();
    ss << "BSONObj size: " << os << " (0x" << unsignedHex(os) << ") is invalid. "
       << "Size must be between 0 and " << maxSize << "(" << (maxSize / (1024 * 1024)) << "MB)";
    try {
        BSONElement e = firstElement();
        ss << " First element: " << e.toString();
    } catch (...) {
    }
    massert(ErrorCodes::BSONObjectTooLarge, ss.str(), 0);
}

BSONObj BSONObj::copy() const {
    // The undefined behavior checks in this function are best-effort and attempt to detect
    // undefined behavior as early as possible. We cannot make any guarantees about detection
    // because we are observing undefined state, and we assume the compiler does not make either
    // of the following optimizations: a) optimizing away the call to objsize() on freed memory, and
    // b) optimizing away the two sequential calls to objsize() as one.
    // The behavior of this function must degrade as gracefully as possible under violation of
    // those assumptions, and preserving any currently observed behavior does not form an argument
    // against the later application of such optimizations.
    int size = objsize();
    _validateUnownedSize(size);
    auto storage = SharedBuffer::allocate(size);

    // If the call to objsize() changes between this call and the previous one, this indicates that
    // that the memory we are reading has changed, and we must exit immediately to avoid further
    // undefined behavior.
    if (int sizeAfter = objsize(); sizeAfter != size) {
        LOGV2_FATAL(
            31323,
            "BSONObj::copy() - size {sizeAfter} differs from previously observed size {size}",
            "sizeAfter"_attr = sizeAfter,
            "size"_attr = size);
    }
    memcpy(storage.get(), objdata(), size);
    return BSONObj(std::move(storage));
}

void BSONObj::makeOwned() {
    if (!isOwned()) {
        *this = copy();
    }
}

BSONObj BSONObj::getOwned() const {
    if (isOwned())
        return *this;
    return copy();
}

BSONObj BSONObj::getOwned(const BSONObj& obj) {
    return obj.getOwned();
}

BSONObj BSONObj::redact(RedactLevel level,
                        std::function<std::string(const BSONElement&)> fieldNameRedactor) const {
    _validateUnownedSize(objsize());

    // Helper to get an "internal function" to be able to do recursion
    struct redactor {
        void appendRedactedElem(BSONObjBuilder& builder,
                                StringData fieldNameString,
                                bool appendMask) {
            if (appendMask) {
                builder.append(fieldNameString, "###"_sd);
            } else {
                builder.appendNull(fieldNameString);
            }
        }

        void operator()(BSONObjBuilder& builder,
                        const BSONObj& obj,
                        bool appendMask,
                        RedactLevel level,
                        std::function<std::string(const BSONElement&)> fieldNameRedactor) {
            for (BSONElement e : obj) {
                StringData fieldNameString;
                // Temporarily allocated string that must live long enough to be copied by builder.
                std::string tempString;
                if (!fieldNameRedactor) {
                    fieldNameString = e.fieldNameStringData();
                } else {
                    tempString = fieldNameRedactor(e);
                    fieldNameString = {tempString};
                }
                if (e.type() == BSONType::object) {
                    BSONObjBuilder subBuilder = builder.subobjStart(fieldNameString);
                    operator()(subBuilder, e.Obj(), appendMask, level, fieldNameRedactor);
                    subBuilder.done();
                } else if (e.type() == BSONType::array) {
                    BSONObjBuilder subBuilder = builder.subarrayStart(fieldNameString);
                    operator()(subBuilder, e.Obj(), appendMask, level, fieldNameRedactor);
                    subBuilder.done();
                } else {
                    // SERVER-79068 Templatizing this could be a good opportunity for performance
                    // improvements.
                    switch (level) {
                        case RedactLevel::all: {
                            appendRedactedElem(builder, fieldNameString, appendMask);
                            break;
                        }
                        case RedactLevel::encryptedAndSensitive: {
                            if (e.type() == BSONType::binData &&
                                (e.binDataType() == BinDataType::Encrypt ||
                                 e.binDataType() == BinDataType::Sensitive)) {
                                appendRedactedElem(builder, fieldNameString, appendMask);
                            } else {
                                builder.append(e);
                            }
                            break;
                        }
                        case RedactLevel::sensitiveOnly: {
                            if (e.type() == BSONType::binData &&
                                e.binDataType() == BinDataType::Sensitive) {
                                appendRedactedElem(builder, fieldNameString, appendMask);
                            } else {
                                builder.append(e);
                            }
                            break;
                        }
                    }
                }
            }
        }
    };

    try {
        BSONObjBuilder builder;
        redactor()(builder, *this, /*appendMask=*/true, level, fieldNameRedactor);
        BSONObj obj = builder.obj();
        uassertStatusOK(obj.validateBSONObjSize().addContext("Redacted object exceeds size limit"));
        return obj;
    } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
    }

    // For some BSONObj with lots of small fields, replacing each element's value with the default
    // redaction mask "###" may cause us to exceed the maximum allowed BSON size. In this case,
    // we use BSONType::jstNull, which ensures the redacted object will not be larger than the
    // original.
    BSONObjBuilder builder;
    redactor()(builder, *this, /*appendMask=*/false, level, fieldNameRedactor);
    return builder.obj();
}

void BSONObj::_validateUnownedSize(int size) const {
    // Only for unowned objects, the size is validated in the constructor, so it is an error for
    // the size to ever be invalid. This means that the unowned memory we are reading has
    // changed, and we must exit immediately to avoid further undefined behavior.
    if (!isOwned() && (size < kMinBSONLength || size > BufferMaxSize)) {
        LOGV2_FATAL(31322,
                    "BSONObj::_validateUnownedSize() - size {size} of unowned BSONObj is invalid "
                    "and differs from previously validated size.",
                    "size"_attr = size);
    }
}

template <typename Generator>
BSONObj BSONObj::_jsonStringGenerator(const Generator& g,
                                      int pretty,
                                      bool isArray,
                                      fmt::memory_buffer& buffer,
                                      size_t writeLimit) const {
    if (isEmpty()) {
        const auto empty = isArray ? "[]"_sd : "{}"_sd;
        buffer.append(empty.data(), empty.data() + empty.size());
        return BSONObj();
    }
    buffer.push_back(isArray ? '[' : '{');

    BSONObjIterator i(*this);
    BSONElement e = i.next();
    BSONObj truncation;
    if (!e.eoo()) {
        bool writeSeparator = false;
        while (1) {
            truncation = e.jsonStringGenerator(
                g, writeSeparator, !isArray, pretty ? (pretty + 1) : 0, buffer, writeLimit);

            if (!truncation.isEmpty()) {
                g.writePadding(buffer);

                BSONObjBuilder bob;
                int omitted = 0;

                bob.append("truncated", truncation);
                if (!e.isABSONObj()) {
                    // element is a leaf and will be omitted
                    omitted++;
                }
                // subsequent elements are omitted
                while (!(e = i.next()).eoo())
                    ++omitted;

                if (omitted) {
                    bob.append("omitted", omitted);
                }
                truncation = bob.obj();
                break;
            }

            e = i.next();

            if (e.eoo()) {
                g.writePadding(buffer);
                break;
            }
            writeSeparator = true;
        }
    }

    if (pretty)
        fmt::format_to(std::back_inserter(buffer), "\n{:<{}}", "", (pretty - 1) * 4);
    buffer.push_back(isArray ? ']' : '}');
    return truncation;
}

BSONObj BSONObj::jsonStringGenerator(ExtendedCanonicalV200Generator const& generator,
                                     int pretty,
                                     bool isArray,
                                     fmt::memory_buffer& buffer,
                                     size_t writeLimit) const {
    return _jsonStringGenerator(generator, pretty, isArray, buffer, writeLimit);
}
BSONObj BSONObj::jsonStringGenerator(ExtendedRelaxedV200Generator const& generator,
                                     int pretty,
                                     bool isArray,
                                     fmt::memory_buffer& buffer,
                                     size_t writeLimit) const {
    return _jsonStringGenerator(generator, pretty, isArray, buffer, writeLimit);
}
BSONObj BSONObj::jsonStringGenerator(LegacyStrictGenerator const& generator,
                                     int pretty,
                                     bool isArray,
                                     fmt::memory_buffer& buffer,
                                     size_t writeLimit) const {
    return _jsonStringGenerator(generator, pretty, isArray, buffer, writeLimit);
}

std::string BSONObj::jsonString(JsonStringFormat format,
                                int pretty,
                                bool isArray,
                                size_t writeLimit,
                                BSONObj* outTruncationResult) const {
    fmt::memory_buffer buffer;
    BSONObj truncation = jsonStringBuffer(format, pretty, isArray, buffer);
    if (outTruncationResult) {
        *outTruncationResult = truncation;
    }
    return fmt::to_string(buffer);
}

BSONObj BSONObj::jsonStringBuffer(JsonStringFormat format,
                                  int pretty,
                                  bool isArray,
                                  fmt::memory_buffer& buffer,
                                  size_t writeLimit) const {
    auto withGenerator = [&](auto&& gen) {
        return jsonStringGenerator(gen, pretty, isArray, buffer, writeLimit);
    };

    if (format == ExtendedCanonicalV2_0_0) {
        return withGenerator(ExtendedCanonicalV200Generator());
    } else if (format == ExtendedRelaxedV2_0_0) {
        return withGenerator(ExtendedRelaxedV200Generator(dateFormatIsLocalTimezone()));
    } else if (format == LegacyStrict) {
        return withGenerator(LegacyStrictGenerator());
    } else {
        MONGO_UNREACHABLE;
    }
}


int BSONObj::woCompare(const BSONObj& r,
                       const Ordering& o,
                       ComparisonRulesSet rules,
                       const StringDataComparator* comparator) const {
    BSONObjIterator i(*this);
    BSONObjIterator j(r);
    for (int pos = 0;; ++pos) {
        bool im = i.more();
        bool jm = j.more();
        if (!im || !jm)
            return im - jm;
        if (int x = i.next().woCompare(j.next(), rules, comparator))
            return x * o.get(pos);
    }
}

/* well ordered compare */
int BSONObj::woCompare(const BSONObj& r,
                       const BSONObj& idxKey,
                       ComparisonRulesSet rules,
                       const StringDataComparator* comparator) const {
    return (rules & ComparisonRules::kIgnoreFieldOrder)
        ? compareObjects<BSONObjIteratorSorted>(*this, r, idxKey, rules, comparator)
        : compareObjects<BSONObjIterator>(*this, r, idxKey, rules, comparator);
}

bool BSONObj::isPrefixOf(const BSONObj& otherObj,
                         const BSONElement::ComparatorInterface& eltCmp) const {
    BSONObjIterator a(*this);
    BSONObjIterator b(otherObj);

    while (a.more() && b.more()) {
        BSONElement x = a.next();
        BSONElement y = b.next();
        if (eltCmp.evaluate(x != y))
            return false;
    }

    return !a.more();
}

bool BSONObj::isFieldNamePrefixOf(const BSONObj& otherObj) const {
    BSONObjIterator a(*this);
    BSONObjIterator b(otherObj);

    while (a.more() && b.more()) {
        BSONElement x = a.next();
        BSONElement y = b.next();
        if (x.fieldNameStringData() != y.fieldNameStringData()) {
            return false;
        }
    }

    return !a.more();
}

void BSONObj::extractFieldsUndotted(BSONObjBuilder* b, const BSONObj& pattern) const {
    BSONObjIterator i(pattern);
    while (i.more()) {
        BSONElement e = i.next();
        BSONElement x = getField(e.fieldName());
        if (!x.eoo())
            b->appendAs(x, "");
    }
}

BSONObj BSONObj::extractFieldsUndotted(const BSONObj& pattern) const {
    BSONObjBuilder b;
    extractFieldsUndotted(&b, pattern);
    return b.obj();
}

void BSONObj::filterFieldsUndotted(BSONObjBuilder* b, const BSONObj& filter, bool inFilter) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        BSONElement x = filter.getField(e.fieldName());
        if ((x.eoo() && !inFilter) || (!x.eoo() && inFilter))
            b->append(e);
    }
}

BSONObj BSONObj::filterFieldsUndotted(const BSONObj& filter, bool inFilter) const {
    BSONObjBuilder b;
    filterFieldsUndotted(&b, filter, inFilter);
    return b.obj();
}

bool BSONObj::couldBeArray() const {
    BSONObjIterator i(*this);
    int index = 0;
    while (i.more()) {
        BSONElement e = i.next();

        // TODO:  If actually important, may be able to do int->char* much faster
        if (strcmp(e.fieldName(), static_cast<std::string>(str::stream() << index).c_str()) != 0)
            return false;
        index++;
    }
    return true;
}

BSONObj BSONObj::clientReadable() const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        switch (e.type()) {
            case BSONType::minKey: {
                BSONObjBuilder m;
                m.append("$minElement", 1);
                b.append(e.fieldName(), m.done());
                break;
            }
            case BSONType::maxKey: {
                BSONObjBuilder m;
                m.append("$maxElement", 1);
                b.append(e.fieldName(), m.done());
                break;
            }
            default:
                b.append(e);
        }
    }
    return b.obj();
}

BSONObj BSONObj::replaceFieldNames(const BSONObj& names) const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    BSONObjIterator j(names);
    BSONElement f = j.next();
    while (i.more()) {
        BSONElement e = i.next();
        if (!f.eoo()) {
            b.appendAs(e, f.fieldName());
            f = j.next();
        } else {
            b.append(e);
        }
    }
    return b.obj();
}

BSONObj BSONObj::stripFieldNames(const BSONObj& obj) {
    if (!obj.hasFieldNames())
        return obj;

    BSONObjBuilder bb;
    for (auto e : obj) {
        bb.appendAs(e, StringData());
    }
    return bb.obj();
}

bool BSONObj::hasFieldNames() const {
    for (auto e : *this) {
        if (!e.fieldNameStringData().empty())
            return true;
    }
    return false;
}

Status BSONObj::storageValidEmbedded() const {
    BSONObjIterator i(*this);

    // The first field is special in the case of a DBRef where the first field must be $ref
    bool first = true;
    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        // Cannot start with "$", unless dbref which must start with ($ref, $id)
        if (name.starts_with("$")) {
            if (first &&
                // $ref is a collection name and must be a String
                (name == "$ref") && e.type() == BSONType::string &&
                (i.next().fieldNameStringData() == "$id")) {
                first = false;
                // keep inspecting fields for optional "$db"
                e = i.next();
                name = e.fieldNameStringData();  // "" if eoo()

                // optional $db field must be a String
                if ((name == "$db") && e.type() == BSONType::string) {
                    continue;  // this element is fine, so continue on to siblings (if any more)
                }

                // Can't start with a "$", all other checks are done below (outside if blocks)
                if (name.starts_with("$")) {
                    return Status(ErrorCodes::DollarPrefixedFieldName,
                                  str::stream() << name << " is not valid for storage.");
                }
            } else {
                // not an okay, $ prefixed field name.
                return Status(ErrorCodes::DollarPrefixedFieldName,
                              str::stream() << name << " is not valid for storage.");
            }
        }

        if (e.mayEncapsulate()) {
            switch (e.type()) {
                case BSONType::object:
                case BSONType::array: {
                    Status s = e.embeddedObject().storageValidEmbedded();
                    // TODO: combine field names for better error messages
                    if (!s.isOK())
                        return s;
                } break;
                case BSONType::codeWScope: {
                    Status s = e.codeWScopeObject().storageValidEmbedded();
                    // TODO: combine field names for better error messages
                    if (!s.isOK())
                        return s;
                } break;
                default:
                    uassert(12579, "unhandled cases in BSONObj storageValidEmbedded", 0);
            }
        }

        // After we have processed one field, we are no longer on the first field
        first = false;
    }
    return Status::OK();
}

BSONElement BSONObj::getField(StringData name) const {
    const char* elem = objdata() + sizeof(int);
    while (*elem) {
        auto ptr = elem;
        // Use the name comparison while computing the name length: this avoids having to look at
        // the same name bytes twice.
        for (auto c : name)
            if (*++ptr != c || c == '\0')
                goto next;  // *ptr is the first non-matching byte, possibly the 0 terminator

        // If the field name is found and complete, return the element.
        if (!*++ptr)
            return BSONElement(elem, ptr - elem, BSONElement::TrustedInitTag{});

        // The name is found, but the field name is not yet complete, so there's no match.
        ++ptr;
next:
        // Skip any remaining part of the field name.
        while (*ptr)
            ++ptr;

        elem += BSONElement(elem, ptr - elem, BSONElement::TrustedInitTag{}).size();
    }
    return BSONElement();
}

int BSONObj::getIntField(StringData name) const {
    BSONElement e = getField(name);
    return e.isNumber() ? (int)e.number() : std::numeric_limits<int>::min();
}

bool BSONObj::getBoolField(StringData name) const {
    BSONElement e = getField(name);
    return e.type() == BSONType::boolean ? e.boolean() : false;
}

StringData BSONObj::getStringField(StringData name) const {
    BSONElement e = getField(name);
    return e.valueStringDataSafe();
}

BSONObj BSONObj::addField(const BSONElement& field) const {
    if (!field.ok())
        return copy();
    BSONObjBuilder b;
    StringData name = field.fieldNameStringData();
    bool added = false;
    for (auto e : *this) {
        if (e.fieldNameStringData() == name) {
            if (!added)
                b.append(field);
            added = true;
        } else {
            b.append(e);
        }
    }
    if (!added)
        b.append(field);
    return b.obj();
}

BSONObj BSONObj::addFields(const BSONObj& from,
                           const boost::optional<StringDataSet>& fields) const {
    BSONObjBuilder bob;
    for (auto&& originalField : *this) {
        auto commonField = from[originalField.fieldNameStringData()];
        // If there is a common field, add the value from 'from' object.
        if (commonField && (!fields || fields->count(originalField.fieldNameStringData()))) {
            bob.append(commonField);
        } else {
            bob.append(originalField);
        }
    }

    for (auto&& fromField : from) {
        // Ignore the common fields as they are already added earlier.
        if (!hasField(fromField.fieldNameStringData()) &&
            (!fields || fields->count(fromField.fieldNameStringData()))) {
            bob.append(fromField);
        }
    }
    return bob.obj();
}

BSONObj BSONObj::removeField(StringData name) const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        auto fname = e.fieldNameStringData();
        if (name != fname)
            b.append(e);
    }
    return b.obj();
}

BSONObj BSONObj::removeFields(const StringDataSet& fields) const {
    BSONObjBuilder bob;
    for (auto&& field : *this) {
        if (fields.count(field.fieldNameStringData())) {
            continue;
        }
        bob.append(field);
    }
    return bob.obj();
}

std::string BSONObj::hexDump() const {
    std::stringstream ss;
    const char* d = objdata();
    int size = objsize();
    for (int i = 0; i < size; ++i) {
        ss.width(2);
        ss.fill('0');
        ss << std::hex << (unsigned)(unsigned char)(d[i]) << std::dec;
        if ((d[i] >= '0' && d[i] <= '9') || (d[i] >= 'A' && d[i] <= 'z'))
            ss << '\'' << d[i] << '\'';
        if (i != size - 1)
            ss << ' ';
    }
    return ss.str();
}


void BSONObj::elems(std::vector<BSONElement>& v) const {
    BSONObjIterator i(*this);
    while (i.more())
        v.push_back(i.next());
}

void BSONObj::elems(std::list<BSONElement>& v) const {
    BSONObjIterator i(*this);
    while (i.more())
        v.push_back(i.next());
}

BSONObj BSONObj::getObjectField(StringData name) const {
    BSONElement e = getField(name);
    BSONType t = e.type();
    return t == BSONType::object || t == BSONType::array ? e.embeddedObject() : BSONObj();
}

int BSONObj::nFields() const {
    int n = 0;
    for (BSONObjIterator i(*this); i.more(); i.next())
        ++n;
    return n;
}

std::string BSONObj::toString(bool redactValues) const {
    if (isEmpty())
        return "{}";
    StringBuilder s;
    toString(s, false, false, redactValues);
    return s.str();
}
void BSONObj::toString(
    StringBuilder& s, bool isArray, bool full, bool redactValues, int depth) const {
    if (isEmpty()) {
        s << (isArray ? "[]" : "{}");
        return;
    }

    s << (isArray ? "[ " : "{ ");
    bool first = true;
    for (auto i = begin();; ++i) {
        BSONElement e = *i;
        massert(10327, "Object does not end with EOO", i != end() || e.eoo());
        massert(10328, "Invalid element size", e.size() > 0);
        massert(10329, "Element too large", e.size() < (1 << 30));
        int offset = (int)(e.rawdata() - this->objdata());
        massert(10330, "Element extends past end of object", e.size() + offset <= this->objsize());
        bool end = (e.size() + offset == this->objsize());
        if (e.eoo()) {
            massert(10331, "EOO Before end of object", end);
            break;
        }
        if (first)
            first = false;
        else
            s << ", ";
        e.toString(s, !isArray, full, redactValues, depth);
    }
    s << (isArray ? " ]" : " }");
}

Status DataType::Handler<BSONObj>::store(const BSONObj& bson,
                                         char* ptr,
                                         size_t length,
                                         size_t* advanced,
                                         std::ptrdiff_t debug_offset) noexcept try {
    if (bson.objsize() > static_cast<int>(length)) {
        str::stream ss;
        ss << "buffer too small to write bson of size (" << bson.objsize()
           << ") at offset: " << debug_offset;
        return Status(ErrorCodes::Overflow, ss);
    }

    if (ptr) {
        std::memcpy(ptr, bson.objdata(), bson.objsize());
    }

    if (advanced) {
        *advanced = bson.objsize();
    }

    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

std::ostream& operator<<(std::ostream& s, const BSONObj& o) {
    return s << o.toString();
}

StringBuilder& operator<<(StringBuilder& s, const BSONObj& o) {
    o.toString(s);
    return s;
}

/** Compare two bson elements, provided as const char *'s, by field name. */
class BSONIteratorSorted::FieldNameCmp {
public:
    FieldNameCmp(bool isArray);
    bool operator()(StringData lhs, StringData rhs) const;

private:
    str::LexNumCmp _cmp;
};

BSONIteratorSorted::FieldNameCmp::FieldNameCmp(bool isArray) : _cmp(!isArray) {}

bool BSONIteratorSorted::FieldNameCmp::operator()(StringData lhs, StringData rhs) const {
    // Just compare field names.
    return _cmp(lhs, rhs);
}

BSONIteratorSorted::BSONIteratorSorted(const BSONObj& o, const FieldNameCmp& cmp)
    : _fields(o.nFields()) {
    int x = 0;
    BSONObjIterator i(o);
    while (i.more()) {
        auto elem = i.next();
        _fields[x++] = elem.fieldNameStringData();
    }
    std::sort(_fields.begin(), _fields.end(), cmp);
    _cur = 0;
}

BSONObjIteratorSorted::BSONObjIteratorSorted(const BSONObj& object)
    : BSONIteratorSorted(object, FieldNameCmp(false)) {}

BSONArrayIteratorSorted::BSONArrayIteratorSorted(const BSONArray& array)
    : BSONIteratorSorted(array, FieldNameCmp(true)) {}

/**
 * Types used to represent BSONObj and BSONArray memory in the Visual Studio debugger
 */
#if defined(_MSC_VER)
struct BSONObjData {
    int32_t size;
} bsonObjDataInstance;

struct BSONArrayData {
    int32_t size;
} bsonObjArrayInstance;
#endif  // defined(_MSC_VER)

}  // namespace mongo
