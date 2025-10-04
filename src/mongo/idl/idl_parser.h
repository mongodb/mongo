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

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace idl {
template <typename T>
using HasBSONSerializeOp = decltype(std::declval<T>().serialize(std::declval<BSONObjBuilder*>()));

template <typename T>
constexpr bool hasBSONSerialize = stdx::is_detected_v<HasBSONSerializeOp, T>;

template <typename T>
void idlSerialize(BSONObjBuilder* builder, StringData fieldName, T arg) {
    if constexpr (hasBSONSerialize<decltype(arg)>) {
        BSONObjBuilder subObj(builder->subobjStart(fieldName));
        arg.serialize(&subObj);
    } else {
        builder->append(fieldName, arg);
    }
}

template <typename T>
void idlSerialize(BSONObjBuilder* builder, StringData fieldName, std::vector<T> arg) {
    BSONArrayBuilder arrayBuilder(builder->subarrayStart(fieldName));
    for (const auto& item : arg) {
        if constexpr (hasBSONSerialize<decltype(item)>) {
            BSONObjBuilder subObj(arrayBuilder.subobjStart());
            item.serialize(&subObj);
        } else {
            arrayBuilder.append(item);
        }
    }
}

/**
 * A few overloads of `idlPreparsedValue` are built into IDL. See
 * `preparsedValue` below. They are placed into a tiny private
 * namespace which defines no types to isolate them.
 */
namespace preparsed_value_adl_barrier {

/**
 * This is the fallback for `idlPreparsedValue`. It value-initializes a `T`
 * with a forwarded argument list in the usual way.
 */
template <typename T, typename... A>
auto idlPreparsedValue(stdx::type_identity<T>, A&&... a) {
    return T(std::forward<A>(a)...);
}

/**
 * The value -1 is a conspicuous "uninitialized" value for integers.
 * The integral type `bool` is exempt from this convention, however.
 */
template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<bool, T>, int> = 0>
auto idlPreparsedValue(stdx::type_identity<T>) {
    return static_cast<T>(-1);
}

/**
 * Define a default Feature Compatibility Version enum value for use in parsed
 * ServerGlobalParams.
 * TODO(SERVER-50101): Remove 'FeatureCompatibility::Version' once IDL supports
 * a command cpp_type of C++ enum.
 */
inline auto idlPreparsedValue(stdx::type_identity<multiversion::FeatureCompatibilityVersion>) {
    return multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior;
}

}  // namespace preparsed_value_adl_barrier

/**
 * Constructs an instance of `T(args...)` for use in idl parsing. The way the
 * IDL generator currently writes C++ parse functions, it makes an instance of
 * a field of type `T` and then mutates it. `preparsedValue<T>()` is used to
 * construct those objects. This convention allows an extension hook whereby a
 * type can select a custom initializer for such pre-parsed objects,
 * particularly for types that shouldn't have a public default constructor.
 *
 * The extension hook is implemented via ADL on the name `idlPreparsedValue`.
 *
 * `idlPreparsedValue` takes a `type_identity<T>` and then some forwarded
 * constructor arguments optionally (the IDL generator doesn't currently
 * provide any such arguments but could conceivably do so in the future). A
 * type `T` is deduced from this `type_identity<T>` argument.
 *
 * There are other ways to implement this extension mechanism, but this
 * phrasing allows argument-dependent lookup to search the namespaces
 * associated with `T`, since `T` is a template parameter of the
 * `type_identity<T>` argument.
 */
template <typename T, typename... A>
T preparsedValue(A&&... args) {
    using preparsed_value_adl_barrier::idlPreparsedValue;
    return idlPreparsedValue(stdx::type_identity<T>{}, std::forward<A>(args)...);
}

enum DebugEnabled : bool {};

constexpr inline DebugEnabled kIsDebug{kDebugBuild};

/**
 * HasMembers tracks the presence of required fields in debug mode, and is a noop class in
 * production builds.
 */
template <size_t N, DebugEnabled = kIsDebug>
class HasMembers;

template <size_t N>
class HasMembers<N, DebugEnabled{false}> {
public:
    void required() const {}
    void markPresent(size_t pos) {}
};

template <size_t N>
class HasMembers<N, DebugEnabled{true}> {
public:
    void required() const {
        invariant(_hasField.all());
    }

    void markPresent(size_t pos) {
        _hasField.set(pos, true);
    }

private:
    std::bitset<N> _hasField;
};

/** Support routines for IDL-generated comparison operators */
namespace relop {

template <typename T>
struct BasicOrderOps;

template <typename T>
struct Ordering {
    friend bool operator==(const Ordering& a, const Ordering& b) {
        return BasicOrderOps<T>{}.equal(a._v, b._v);
    }
    friend bool operator<(const Ordering& a, const Ordering& b) {
        return BasicOrderOps<T>{}.less(a._v, b._v);
    }
    friend bool operator!=(const Ordering& a, const Ordering& b) {
        return !(a == b);
    }
    friend bool operator>(const Ordering& a, const Ordering& b) {
        return b < a;
    }
    friend bool operator<=(const Ordering& a, const Ordering& b) {
        return !(a > b);
    }
    friend bool operator>=(const Ordering& a, const Ordering& b) {
        return !(a < b);
    }

    const T& _v;
};
template <typename T>
Ordering(const T&) -> Ordering<T>;

/** fallback case */
template <typename T>
struct BasicOrderOps {
    bool equal(const T& a, const T& b) const {
        return a == b;
    }
    bool less(const T& a, const T& b) const {
        return a < b;
    }
};

template <>
struct BasicOrderOps<BSONObj> {
    bool equal(const BSONObj& a, const BSONObj& b) const {
        return _cmp(a, b) == 0;
    }

    bool less(const BSONObj& a, const BSONObj& b) const {
        return _cmp(a, b) < 0;
    }

private:
    int _cmp(const BSONObj& a, const BSONObj& b) const {
        return SimpleBSONObjComparator::kInstance.compare(a, b);
    }
};

/** Disengaged optionals precede engaged optionals. */
template <typename T>
struct BasicOrderOps<boost::optional<T>> {
    bool equal(const boost::optional<T>& a, const boost::optional<T>& b) const {
        return (!a || !b) ? (!!a == !!b) : (Ordering{*a} == Ordering{*b});
    }

    bool less(const boost::optional<T>& a, const boost::optional<T>& b) const {
        return (!a || !b) ? (!!a < !!b) : (Ordering{*a} < Ordering{*b});
    }
};

}  // namespace relop

}  // namespace idl

/**
 * IDLParserContext manages the current parser context for parsing BSON documents.
 *
 * The class stores the path to the current document to enable it provide more useful error
 * messages. The path is a dot delimited list of field names which is useful for nested struct
 * parsing.
 *
 * This class is responsible for throwing all error messages the IDL generated parsers throw,
 * and provide utility methods like checking a BSON type or set of BSON types.
 */
class IDLParserContext {
    IDLParserContext(const IDLParserContext&) = delete;
    IDLParserContext& operator=(const IDLParserContext&) = delete;

    template <typename T>
    friend void throwComparisonError(IDLParserContext& ctxt,
                                     StringData fieldName,
                                     StringData op,
                                     T actualValue,
                                     T expectedValue);

public:
    /**
     * String constants for well-known IDL fields.
     */
    static constexpr auto kOpMsgDollarDB = "$db"_sd;
    static constexpr auto kOpMsgDollarDBDefault = "admin"_sd;
    static constexpr auto kTenantIdField = "$tenant"_sd;

    explicit IDLParserContext(StringData fieldName)
        : IDLParserContext{fieldName,
                           boost::optional<auth::ValidatedTenancyScope>{boost::none},
                           boost::optional<TenantId>{boost::none},
                           SerializationContext::stateDefault()} {}

    IDLParserContext(StringData fieldName,
                     const boost::optional<auth::ValidatedTenancyScope>& vts,
                     boost::optional<TenantId> tenantId,
                     const SerializationContext& serializationContext)
        : _serializationContext(serializationContext),
          _currentField(fieldName),
          _tenantId(std::move(tenantId)),
          _predecessor(nullptr),
          _validatedTenancyScope(vts) {}

    IDLParserContext(StringData fieldName, const IDLParserContext* predecessor)
        : IDLParserContext(fieldName,
                           predecessor,
                           boost::optional<auth::ValidatedTenancyScope>{boost::none},
                           SerializationContext::stateDefault(),
                           boost::optional<TenantId>{boost::none}) {}

    IDLParserContext(StringData fieldName,
                     const IDLParserContext* predecessor,
                     const boost::optional<auth::ValidatedTenancyScope>& vts,
                     const SerializationContext& serializationContext,
                     boost::optional<TenantId> tenantId)
        : _serializationContext(serializationContext),
          _currentField(fieldName),
          _tenantId(std::move(tenantId)),
          _predecessor(predecessor),
          _validatedTenancyScope(vts) {
        assertTenantIdMatchesPredecessor(predecessor);
    }

    /**
     * Check that BSON element is a given type or whether the field should be skipped.
     *
     * Returns true if the BSON element is the correct type.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertType(const BSONElement& element, BSONType type) const {
        if (MONGO_likely(element.type() == type)) {
            return true;
        }

        return checkAndAssertTypeSlowPath(element, type);
    }

    /**
     * Check that BSON element is a bin data type, and has the specified bin data subtype, or
     * whether the field should be skipped.
     *
     * Returns true if the BSON element is the correct type.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertBinDataType(const BSONElement& element, BinDataType type) const {
        if (MONGO_likely(element.type() == BSONType::binData && element.binDataType() == type)) {
            return true;
        }

        return checkAndAssertBinDataTypeSlowPath(element, type);
    }

    /**
     * Check that BSON element is one of a given type or whether the field should be skipped.
     *
     * Returns true if the BSON element is one of the types.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertTypes(const BSONElement& element, std::span<const BSONType> types) const;

    /**
     * Throw an error message about the BSONElement being a duplicate field.
     */
    MONGO_COMPILER_NORETURN void throwDuplicateField(const BSONElement& element) const;

    /**
     * Throw an error message about the BSONElement being a duplicate field.
     */
    MONGO_COMPILER_NORETURN void throwDuplicateField(StringData fieldName) const;

    /**
     * Throw an error message about the required field missing from the document.
     */
    MONGO_COMPILER_NORETURN void throwMissingField(StringData fieldName) const;

    /**
     * Throw an error message about an unknown field in a document.
     */
    MONGO_COMPILER_NORETURN void throwUnknownField(StringData fieldName) const;

    /**
     * Throw an error message about the array field name not being the next number in the sequence.
     */
    MONGO_COMPILER_NORETURN void throwBadArrayFieldNumberSequence(StringData actualValue,
                                                                  StringData expectedValue) const;

    /**
     * Throw an error message about an unrecognized enum value.
     */
    MONGO_COMPILER_NORETURN void throwBadEnumValue(StringData enumValue) const;
    MONGO_COMPILER_NORETURN void throwBadEnumValue(int enumValue) const;

    /**
     * Throw an error about a field having the wrong type.
     */
    MONGO_COMPILER_NORETURN void throwBadType(const BSONElement& element,
                                              std::span<const BSONType> types) const;

    /**
     * Check that the collection name in 'element' is valid. Throws an exception if not valid.
     * Returns the collection name otherwise.
     */
    static StringData checkAndAssertCollectionName(const BSONElement& element,
                                                   bool allowGlobalCollectionName);

    /**
     * Check that the collection name or UUID in 'element' is valid. Throws an exception if not
     * valid. Returns either the collection name or UUID otherwise.
     */
    static std::variant<UUID, StringData> checkAndAssertCollectionNameOrUUID(
        const BSONElement& element);

    const boost::optional<TenantId>& getTenantId() const;

    const SerializationContext& getSerializationContext() const;

    const boost::optional<auth::ValidatedTenancyScope>& getValidatedTenancyScope() const;

private:
    /**
     * See comment on getElementPath below.
     */
    std::string getElementPath(const BSONElement& element) const;

    /**
     * Return a dot seperated path to the specified field. For instance, if the code is parsing a
     * grandchild field that has an error, this will return "grandparent.parent.child".
     */
    std::string getElementPath(StringData fieldName) const;

    /**
     * See comment on checkAndAssertType.
     */
    bool checkAndAssertTypeSlowPath(const BSONElement& element, BSONType type) const;

    /**
     * See comment on checkAndAssertBinDataType.
     */
    bool checkAndAssertBinDataTypeSlowPath(const BSONElement& element, BinDataType type) const;

    void assertTenantIdMatchesPredecessor(const IDLParserContext* predecessor) {
        if (!_tenantId || predecessor == nullptr) {
            return;
        }

        auto& parentTenantId = predecessor->getTenantId();
        iassert(8423379,
                str::stream() << "The IDLParserContext tenantId " << _tenantId->toString()
                              << " must match the predecessor's tenantId "
                              << parentTenantId->toString(),
                !parentTenantId || parentTenantId == _tenantId);
    }

private:
    // Modifies serialization behavior to match request format, only accessed by IDL generated code
    const SerializationContext _serializationContext;

    // Name of the current field that is being parsed.
    const StringData _currentField;

    const boost::optional<TenantId> _tenantId;

    // Pointer to a parent parser context.
    // This provides a singly linked list of parent pointers, and use to produce a full path to a
    // field with an error.
    const IDLParserContext* _predecessor;

    const boost::optional<auth::ValidatedTenancyScope> _validatedTenancyScope;
};

/**
 * This class is used to record information about deserialization which a caller can later use to
 * perform extra parsing validation.
 */
class DeserializationContext {
public:
    /**
     * Marks that an unstable struct field was parsed.
     *
     * This does not perform any validation whatsoever, including validating that this field is
     * unstable or that it had already been marked present.
     */
    void markUnstableFieldPresent(StringData unstableFieldName) {
        _unstableField = unstableFieldName;
    }

    /**
     * Throws `ErrorCodes::APIStrictError` if an unstable field was encountered during
     * deserialization.
     */
    void validateApiStrict() {
        if (_unstableField) {
            uasserted(ErrorCodes::APIStrictError,
                      str::stream() << "BSON field '" << *_unstableField
                                    << "' is not allowed with apiStrict:true.");
        }
    }

private:
    boost::optional<StringData> _unstableField;
};

/**
 * Throw an error when BSON validation fails during parse.
 */
template <typename T>
void throwComparisonError(
    IDLParserContext& ctxt, StringData fieldName, StringData op, T actualValue, T expectedValue) {
    std::string path = ctxt.getElementPath(fieldName);
    throwComparisonError(path, op, actualValue, expectedValue);
}

/**
 * Throw an error when a user calls a setter and it fails the comparison.
 */
template <typename T>
void throwComparisonError(StringData fieldName, StringData op, T actualValue, T expectedValue) {
    uasserted(ErrorCodes::BadValue,
              str::stream() << "BSON field '" << fieldName << "' value must be " << op << " "
                            << expectedValue << ", actual value '" << actualValue << "'");
}


/**
 * Transform a vector of input type to a vector of output type.
 *
 * Used by the IDL generated code to transform between vectors of view, and non-view types.
 */
std::vector<StringData> transformVector(const std::vector<std::string>& input);
std::vector<std::string> transformVector(const std::vector<StringData>& input);
std::vector<ConstDataRange> transformVector(const std::vector<std::vector<std::uint8_t>>& input);
std::vector<std::vector<std::uint8_t>> transformVector(const std::vector<ConstDataRange>& input);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void noOpSerializer(bool, StringData fieldName, BSONObjBuilder* bob);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeBSONWhenNotEmpty(BSONObj obj, StringData fieldName, BSONObjBuilder* bob);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
BSONObj parseOwnedBSON(BSONElement element);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
bool parseBoolean(BSONElement element);

/**
 * Generated enums specialize this with their element count.
 */
template <typename E>
constexpr inline size_t idlEnumCount = 0;

namespace idl {

/**
 * Parse an IDL-defined struct from a document, throwing an exception if any unstable fields are
 * encountered.
 */
template <typename T>
T parseApiStrict(const BSONObj& cmdObj, const IDLParserContext& ctx) {
    DeserializationContext dctx;
    auto cmd = T::parse(cmdObj, ctx, &dctx);
    dctx.validateApiStrict();
    return cmd;
}

/**
 * Parse an IDL-defined command from a command request document.
 * If the request includes apiStrict: true along with any unstable fields, an exception will be
 * thrown.
 */
template <typename T>
T parseCommandDocument(const BSONObj& cmdObj, const IDLParserContext& ctx) {
    DeserializationContext dctx;
    auto cmd = T::parse(cmdObj, ctx, &dctx);
    if (cmd.getGenericArguments().getApiStrict().value_or(false)) {
        dctx.validateApiStrict();
    }
    return cmd;
}

/**
 * Parse an IDL-defined command from a command request.
 * If the request includes apiStrict: true along with any unstable fields, an exception will be
 * thrown.
 */
template <typename T>
T parseCommandRequest(const OpMsgRequest& req, const IDLParserContext& ctx) {
    DeserializationContext dctx;
    auto cmd = T::parse(req, ctx, &dctx);
    if (cmd.getGenericArguments().getApiStrict().value_or(false)) {
        dctx.validateApiStrict();
    }
    return cmd;
}
}  // namespace idl

}  // namespace mongo
