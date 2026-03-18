/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bson_mutator/bson_mutator_owned_obj.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/util/shared_buffer.h"

#include <iostream>
#include <string>

#include <absl/container/node_hash_map.h>
#include <absl/random/bit_gen_ref.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_format.h>
#include <fuzztest/domain_core.h>
#include <fuzztest/fuzztest.h>


namespace fuzztest::internal {

/**
 * wrapper for BSONObjImpl to allow use of fuzztest::Arbitrary<ConstSharedBuffer>
 * see BSONObjImpl for more information
 */
template <>
class ArbitraryImpl<mongo::ConstSharedBuffer>;

}  // namespace fuzztest::internal

namespace mongo::bson_mutator {

// (CamelCaseName, CPPType, BSONType, DefaultDomain)
#define BSON_MUTATOR_EXPAND_FIELD_TYPES(FUNC)                                                    \
    FUNC(Double, double, BSONType::numberDouble, fuzztest::Arbitrary<double>())                  \
    FUNC(String, std::string, BSONType::string, fuzztest::Arbitrary<std::string>())              \
    FUNC(BSONObj, ConstSharedBuffer, BSONType::object, fuzztest::Arbitrary<ConstSharedBuffer>()) \
    FUNC(BSONArray, BSONArray, BSONType::array, ArbitraryBSONArray())                            \
    FUNC(BinData, BSONBinDataOwned, BSONType::binData, ArbitraryBinData())                       \
    FUNC(Undefined, std::optional<char>, BSONType::undefined, fuzztest::NullOpt<char>())         \
    FUNC(OID, OID, BSONType::oid, ArbitraryOID())                                                \
    FUNC(Bool, bool, BSONType::boolean, fuzztest::Arbitrary<bool>())                             \
    FUNC(Date, Date_t, BSONType::date, ArbitraryDate_T())                                        \
    FUNC(Null, std::optional<int>, BSONType::null, fuzztest::NullOpt<int>())                     \
    FUNC(RegEx, BSONRegExOwned, BSONType::regEx, ArbitraryBSONRegEx())                           \
    FUNC(DBRef, BSONDBRefOwned, BSONType::dbRef, ArbitraryBSONDBRef())                           \
    FUNC(Code, BSONCodeOwned, BSONType::code, ArbitraryBSONCode())                               \
    FUNC(Symbol, BSONSymbolOwned, BSONType::symbol, ArbitraryBSONSymbol())                       \
    FUNC(CodeWScope, BSONCodeWScopeOwned, BSONType::codeWScope, ArbitraryBSONCodeWScope())       \
    FUNC(Int, int, BSONType::numberInt, fuzztest::Arbitrary<int>())                              \
    FUNC(Timestamp, Timestamp, BSONType::timestamp, ArbitraryTimestamp())                        \
    FUNC(Long, long long, BSONType::numberLong, fuzztest::Arbitrary<long long>())                \
    FUNC(Decimal, Decimal128, BSONType::numberDecimal, ArbitraryDecimal128())

#define BSON_MUTATOR_TAIL_(A, ...) __VA_ARGS__
#define BSON_MUTATOR_TAIL(...) BSON_MUTATOR_TAIL_(__VA_ARGS__)
#define BSON_MUTATOR_VARIANT_TYPES(Camel, cpptype, bsontype, defaultdomain) \
    , fuzztest::Domain<cpptype>
using BSONElementDomainVariant =
    std::variant<BSON_MUTATOR_TAIL(BSON_MUTATOR_EXPAND_FIELD_TYPES(BSON_MUTATOR_VARIANT_TYPES))>;

using BSONDomainMap = absl::flat_hash_map<BSONType, BSONElementDomainVariant>;
using BSONDomainVariant = std::variant<BSONElementDomainVariant, BSONDomainMap>;


fuzztest::Domain<Date_t> ArbitraryDate_T();
fuzztest::Domain<Decimal128> ArbitraryDecimal128();
fuzztest::Domain<Timestamp> ArbitraryTimestamp();
fuzztest::Domain<OID> ArbitraryOID();
fuzztest::Domain<BSONRegExOwned> ArbitraryBSONRegEx();
fuzztest::Domain<BSONCodeOwned> ArbitraryBSONCode();
fuzztest::Domain<BSONSymbolOwned> ArbitraryBSONSymbol();
fuzztest::Domain<BSONCodeWScopeOwned> ArbitraryBSONCodeWScope();
fuzztest::Domain<BSONDBRefOwned> ArbitraryBSONDBRef();
fuzztest::Domain<BSONArray> ArbitraryBSONArray();
fuzztest::Domain<BSONBinDataOwned> ArbitraryBinData();  // collection of all BinData subtypes


fuzztest::Domain<BSONBinDataOwned> ArbitraryBinDataGeneral();
fuzztest::Domain<BSONBinDataOwned> ArbitraryFunction();
fuzztest::Domain<BSONBinDataOwned> ArbitraryByteArrayDeprecated();
fuzztest::Domain<BSONBinDataOwned> ArbitrarybdtUUID();
fuzztest::Domain<BSONBinDataOwned> ArbitrarynewUUID();
fuzztest::Domain<BSONBinDataOwned> ArbitraryMD5Type();
fuzztest::Domain<BSONBinDataOwned> ArbitraryColumn();
fuzztest::Domain<BSONBinDataOwned> ArbitraryEncrypted();
fuzztest::Domain<BSONBinDataOwned> ArbitrarySensitive();
fuzztest::Domain<BSONBinDataOwned> ArbitraryVector();
fuzztest::Domain<BSONBinDataOwned> ArbitraryUserData();

struct BSONPrinter;

/**
 * BSONObjImpl is a FuzzTest domain allowing for custom BSON mutation.
 *
 * BSONObjImpl supports all possible BSON types, including deprecated ones.
 * The BSON objects generated by this domain are guaranteed to pass validateBSON() in
 * BSONValidateModeEnum::kDefault mode, but may contain values that fail with stricter validation
 * levels.
 *
 * BSONObjImpl provides a ConstSharedBuffer as it's value type to the fuzzer test using it.
 * This allows the fuzzer test either treat it as a raw buffer or convert it directly to a BSONObj
 * as needed.
 *
 * Example:
 * void TestFuzzer(ConstSharedBuffer input) {
 *      BSONObj obj = BSONObj(input);
 *      ...
 * }
 *
 * By default, BSON objects are randomly generated with arbitrary elements. Specific element
 * names and types can be specified using the appropriate .With<type>() function. This ensures that
 * an element will always have the same name and type when the object is generated.
 * However, an element added this way is not always guaranteed to exist within the object.
 * This allows for testing error handling code paths.
 *
 * Example:
 *   BSONObjImpl()
 *     .WithInt("foo")
 *     .WithLong("bar", fuzztest::InRange(0, 50))
 *     .WithString("baz");
 *
 * An element can support multiple BSON element types by using the .WithVariant() function.
 * By providing a BSONDomainMap, a BSONType can be mapped to arbitrary fuzztest domains.
 *
 * Example:
 *   BSONObjImpl().WithVariant("foo", {
 *     {BSONType::numberInt, fuzztest::InRange(0, 100)},
 *     {BSONType::numberLong, fuzztest::InRange(0, 10000)}
 *   });
 *
 * The .WithAny() function adds a specific BSON element to the object without restricting the
 * associated type. This is useful when a key is required, but explicit type information is not
 * easily identifiable or for use in validation functions.
 *
 * Example:
 *   BSONObjImpl().WithAny("foo");
 *
 */
class BSONObjImpl
    : public fuzztest::domain_implementor::DomainBase<BSONObjImpl, ConstSharedBuffer, BSONObj> {
public:
    using corpus_type = BSONObj;
    using value_type = ConstSharedBuffer;
    static constexpr bool has_custom_corpus_type = true;

    BSONObjImpl();

    corpus_type Init(absl::BitGenRef prng);

    void Mutate(corpus_type& val,
                absl::BitGenRef prng,
                const fuzztest::domain_implementor::MutationMetadata& metadata,
                bool only_shrink);

    value_type GetValue(const corpus_type& corpus_value) const;

    std::optional<corpus_type> FromValue(const value_type& user_value) const;

    std::optional<corpus_type> ParseCorpus(const fuzztest::internal::IRObject& obj) const;

    BSONPrinter GetPrinter() const;

    absl::Status ValidateCorpusValue(const corpus_type& corpus_value) const;

    fuzztest::internal::IRObject SerializeCorpus(const corpus_type& corpus) const;


#define BSON_MUTATOR_WITH_FIELD(Camel, cpptype, bsontype, defaultDomain)                       \
    BSONObjImpl& With##Camel(const std::string& name, const fuzztest::Domain<cpptype> domain); \
    BSONObjImpl& With##Camel(const std::string& name);

    BSON_MUTATOR_EXPAND_FIELD_TYPES(BSON_MUTATOR_WITH_FIELD)

    /**
     * Adds a BSONElement which can be one of the types defined by domain.
     */
    BSONObjImpl& WithVariant(const std::string& name, BSONDomainMap domain) {
        BSONDomainVariant x = domain;
        _inputElements.try_emplace(name, x);
        return *this;
    }

    /**
     * Adds a BSONElement without specifying type information
     */
    BSONObjImpl& WithAny(const std::string& name);

private:
    void ShrinkRandomElement(BSONObjBuilder& bob,
                             std::vector<BSONElement>& elements,
                             absl::BitGenRef prng,
                             const fuzztest::domain_implementor::MutationMetadata& metadata);

    void RemoveRandomElement(BSONObjBuilder& bob,
                             std::vector<BSONElement>& elements,
                             absl::BitGenRef prng);

    void CreateRandomObj(BSONObjBuilder& bob, absl::BitGenRef prng);

    void ModifyRandomElement(BSONObjBuilder& bob,
                             std::vector<BSONElement>& elements,
                             absl::BitGenRef prng,
                             const fuzztest::domain_implementor::MutationMetadata& metadata);

    void addAllToBuilder(BSONObjBuilder& bob, std::vector<BSONElement>& elements);

    template <typename T>
    T getElementValue(const BSONElement& elem);

    template <typename T>
    void addToBuilder(BSONObjBuilder& bob, const std::string& key, const T& value);

    template <typename F>
    void visitDomain(F visitor, std::string fieldName, BSONType domainType);

    void printElement(BSONElement element);

    absl::flat_hash_map<std::string, BSONDomainVariant> _inputElements;
    fuzztest::Domain<std::string> _fieldNameDomain;
};

struct BSONPrinter {
    using CorpusType = BSONObjImpl::corpus_type;

    void PrintCorpusValue(const CorpusType& val,
                          fuzztest::domain_implementor::RawSink out,
                          fuzztest::domain_implementor::PrintMode mode) const;
};

}  // namespace mongo::bson_mutator

namespace fuzztest::internal {

template <>
class ArbitraryImpl<mongo::ConstSharedBuffer> : public mongo::bson_mutator::BSONObjImpl {};

}  // namespace fuzztest::internal
