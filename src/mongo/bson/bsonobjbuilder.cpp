// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"

#include "mongo/bson/timestamp.h"
#include "mongo/logv2/log.h"

#include <string>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendMinForType(std::string_view fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case stdx::to_underlying(BSONType::numberInt):
        case stdx::to_underlying(BSONType::numberDouble):
        case stdx::to_underlying(BSONType::numberLong):
        case stdx::to_underlying(BSONType::numberDecimal):
            append(fieldName, std::numeric_limits<double>::quiet_NaN());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::symbol):
        case stdx::to_underlying(BSONType::string):
            append(fieldName, "");
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::date):
            appendDate(fieldName, Date_t::min());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::timestamp):
            appendTimestamp(fieldName, 0);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::undefined):  // shared with EOO
            appendUndefined(fieldName);
            return static_cast<Derived&>(*this);

        // Separate canonical types
        case stdx::to_underlying(BSONType::minKey):
            appendMinKey(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::maxKey):
            appendMaxKey(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::oid): {
            OID o;
            appendOID(fieldName, &o);
            return static_cast<Derived&>(*this);
        }
        case stdx::to_underlying(BSONType::boolean):
            appendBool(fieldName, false);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::null):
            appendNull(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::object):
            append(fieldName, BSONObj());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::array):
            appendArray(fieldName, BSONObj());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::binData):
            appendBinData(fieldName, 0, BinDataGeneral, (const char*)nullptr);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::regEx):
            appendRegex(fieldName, "");
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::dbRef): {
            OID o;
            appendDBRef(fieldName, "", o);
            return static_cast<Derived&>(*this);
        }
        case stdx::to_underlying(BSONType::code):
            appendCode(fieldName, "");
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::codeWScope):
            appendCodeWScope(fieldName, "", BSONObj());
            return static_cast<Derived&>(*this);
    }
    LOGV2(20101, "type not supported for appendMinElementForType: {t}", "t"_attr = t);
    uassert(10061, "type not supported for appendMinElementForType", false);
}

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendMaxForType(std::string_view fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case stdx::to_underlying(BSONType::numberInt):
        case stdx::to_underlying(BSONType::numberDouble):
        case stdx::to_underlying(BSONType::numberLong):
        case stdx::to_underlying(BSONType::numberDecimal):
            append(fieldName, std::numeric_limits<double>::infinity());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::symbol):
        case stdx::to_underlying(BSONType::string):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::object));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::date):
            appendDate(fieldName, Date_t::max());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::timestamp):
            append(fieldName, Timestamp::max());
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::undefined):  // shared with EOO
            appendUndefined(fieldName);
            return static_cast<Derived&>(*this);

        // Separate canonical types
        case stdx::to_underlying(BSONType::minKey):
            appendMinKey(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::maxKey):
            appendMaxKey(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::oid): {
            OID o = OID::max();
            appendOID(fieldName, &o);
            return static_cast<Derived&>(*this);
        }
        case stdx::to_underlying(BSONType::boolean):
            appendBool(fieldName, true);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::null):
            appendNull(fieldName);
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::object):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::array));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::array):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::binData));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::binData):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::oid));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::regEx):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::dbRef));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::dbRef):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::code));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::code):
            appendMinForType(fieldName, stdx::to_underlying(BSONType::codeWScope));
            return static_cast<Derived&>(*this);
        case stdx::to_underlying(BSONType::codeWScope):
            // This upper bound may change if a new bson type is added.
            appendMinForType(fieldName, stdx::to_underlying(BSONType::maxKey));
            return static_cast<Derived&>(*this);
    }
    LOGV2(20102, "type not supported for appendMaxElementForType: {t}", "t"_attr = t);
    uassert(14853, "type not supported for appendMaxElementForType", false);
}

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendDate(std::string_view fieldName, Date_t dt) {
    _b.appendNum((char)BSONType::date);
    _b.appendCStr(fieldName);
    _b.appendNum(dt.toMillisSinceEpoch());
    return static_cast<Derived&>(*this);
}

/* add all the fields from the object specified to this object */
template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendElements(const BSONObj& x) {
    if (!x.isEmpty())
        _b.appendBuf(x.objdata() + 4,   // skip over leading length
                     x.objsize() - 5);  // ignore leading length and trailing \0
    return static_cast<Derived&>(*this);
}

/* add all the fields from the object specified to this object, replacing each
 * field name with the corresponding field name in the 'newNames' object.
 */
template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendElementsRenamed(const BSONObj& x,
                                                               const BSONObj& newNames,
                                                               bool keepTail) {
    BSONObjIterator it(x);
    for (BSONObjIterator nIter(newNames); it.more() && nIter.more();) {
        appendAs(it.next(), nIter.next().fieldName());
    }
    if (keepTail) {
        while (it.more()) {
            append(it.next());
        }
    }
    return static_cast<Derived&>(*this);
}

/* add all the fields from the object specified to this object if they don't exist */
template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendElementsUnique(const BSONObj& x) {
    std::set<std::string> have;
    {
        BSONObjIterator i = iterator();
        while (i.more())
            have.insert(i.next().fieldName());
    }

    BSONObjIterator it(x);
    while (it.more()) {
        BSONElement e = it.next();
        if (have.count(e.fieldName()))
            continue;
        append(e);
    }
    return static_cast<Derived&>(*this);
}

template <class Derived, class B>
BSONObjIterator BSONObjBuilderBase<Derived, B>::iterator() const {
    const char* s = _b.buf() + _offset;
    const char* e = _b.buf() + _b.len();
    return BSONObjIterator(s, e);
}

template <class Derived, class B>
bool BSONObjBuilderBase<Derived, B>::hasField(std::string_view name) const {
    BSONObjIterator i = iterator();
    while (i.more())
        if (name == i.next().fieldName())
            return true;
    return false;
}

// Explicit instantiations
template class BSONObjBuilderBase<BSONObjBuilder, BufBuilder>;
template class BSONObjBuilderBase<UniqueBSONObjBuilder, UniqueBufBuilder>;
template class BSONObjBuilderBase<allocator_aware::BSONObjBuilder<std::allocator<void>>,
                                  allocator_aware::BufBuilder<std::allocator<void>>>;
template class BSONObjBuilderBase<allocator_aware::BSONObjBuilder<tracking::Allocator<void>>,
                                  allocator_aware::BufBuilder<tracking::Allocator<void>>>;
template class BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder>;
template class BSONArrayBuilderBase<allocator_aware::BSONArrayBuilder<std::allocator<void>>,
                                    allocator_aware::BSONObjBuilder<std::allocator<void>>>;
template class BSONArrayBuilderBase<allocator_aware::BSONArrayBuilder<tracking::Allocator<void>>,
                                    allocator_aware::BSONObjBuilder<tracking::Allocator<void>>>;
template class BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder>;

}  // namespace mongo
