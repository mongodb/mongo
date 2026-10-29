// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"

#include "mongo/bson/timestamp.h"
#include "mongo/logv2/log.h"

#include <array>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

/**
 * Refers to a field name stored inside a BSONObjBuilder's buffer, by the field name's offset into
 * that buffer plus its length. Appending more data to the builder can reallocate the buffer and
 * thus invalidate any pointers into it, but the offsets remain valid.
 */
struct FieldNameRef {
    size_t offset;
    size_t length;
};

/**
 * Resolves a lookup key into the field name it refers to. A key is either a FieldNameRef, which has
 * to be resolved against the builder's current buffer, or already a field name.
 */
template <class B>
std::string_view toView(const B& b, FieldNameRef key) {
    return std::string_view(b.buf() + key.offset, key.length);
}

template <class B>
std::string_view toView(const B&, std::string_view key) {
    return key;
}

/**
 * Transparent hasher for a hash set of FieldNameRefs, allowing lookups by field name. 'b' is the
 * builder owning the buffer the FieldNameRefs point into, and must outlive the hash set.
 */
template <class B>
struct FieldNameHash {
    using is_transparent = void;

    template <class T>
    size_t operator()(const T& key) const {
        return absl::Hash<std::string_view>{}(toView(*b, key));
    }

    const B* b;
};

/**
 * Transparent equality comparator matching FieldNameHash. Either argument may be a FieldNameRef
 * or a field name.
 */
template <class B>
struct FieldNameEq {
    using is_transparent = void;

    template <class T, class U>
    bool operator()(const T& lhs, const U& rhs) const {
        return toView(*b, lhs) == toView(*b, rhs);
    }

    const B* b;
};

}  // namespace

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
    // Objects normally have few fields, so serve the hash set's allocations from a small stack
    // buffer. Once the buffer is exhausted, the monotonic resource falls back to its upstream
    // resource, which allocates on the heap.
    static constexpr size_t kStackBufferSize = 4 * 1024;
    alignas(std::max_align_t) std::array<std::byte, kStackBufferSize> stackBuffer;
    std::pmr::monotonic_buffer_resource resource(stackBuffer.data(), stackBuffer.size());

    using Allocator = std::pmr::polymorphic_allocator<FieldNameRef>;
    absl::flat_hash_set<FieldNameRef, FieldNameHash<B>, FieldNameEq<B>, Allocator> have(
        0, FieldNameHash<B>{&_b}, FieldNameEq<B>{&_b}, Allocator{&resource});

    for (BSONObjIterator i = iterator(); i.more();) {
        BSONElement e = i.next();
        std::string_view fieldName = e.fieldNameStringData();
        have.emplace(static_cast<size_t>(fieldName.data() - _b.buf()), fieldName.size());
    }

    BSONObjIterator it(x);
    while (it.more()) {
        BSONElement e = it.next();
        if (have.contains(e.fieldNameStringData()))
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
        if (name == i.next().fieldNameStringData())
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
