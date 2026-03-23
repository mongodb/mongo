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


#include "mongo/bson/bson_mutator/bson_mutator.h"

#include "mongo/bson/bson_validate.h"
#include "mongo/util/overloaded_visitor.h"

#include <string>

#include <gtest/gtest.h>

namespace mongo::bson_mutator {

#define DOMAIN_MAP_INIT(Camel, cpptype, bsontype, defaultdomain) \
    , {                                                          \
        bsontype, BSONElementDomainVariant(defaultdomain)        \
    }

static BSONDomainMap default_domains_{
    BSON_MUTATOR_TAIL(BSON_MUTATOR_EXPAND_FIELD_TYPES(DOMAIN_MAP_INIT))};

void BSONPrinter::PrintCorpusValue(const CorpusType& val,
                                   fuzztest::domain_implementor::RawSink out,
                                   fuzztest::domain_implementor::PrintMode mode) const {

    if (mode == fuzztest::domain_implementor::PrintMode::kHumanReadable) {
        absl::Format(out, "BSONObj(%s)", val.toString());
    } else {
        absl::Format(out, "BSONObj(%s)", val.hexDump());
    }
}


BSONObjImpl::BSONObjImpl() : _fieldNameDomain(fuzztest::StringOf(fuzztest::NonZeroChar())) {}


BSONObjImpl::corpus_type BSONObjImpl::Init(absl::BitGenRef prng) {
    if (auto seed = MaybeGetRandomSeed(prng)) {
        return *seed;
    }

    BSONObjBuilder bob;
    for (auto& elem : _inputElements) {
        BSONDomainVariant value = elem.second;

        /**
         * handler will generate a new initial value for a domain and add it to the
         * Object builder. There is special handling for Null, Undefined, and raw bson buffers.
         */
        auto handler = OverloadedVisitor{
            [&](fuzztest::Domain<ConstSharedBuffer>& arg) {
                auto val = arg.GetValue(arg.Init(prng));
                bob.append(elem.first, BSONObj(val));
            },
            [&](fuzztest::Domain<std::optional<char>>) { bob.appendUndefined(elem.first); },
            [&](fuzztest::Domain<std::optional<int>>) { bob.appendNull(elem.first); },
            [&](auto& arg) {
                auto val = arg.GetValue(arg.Init(prng));
                bob.append(elem.first, val);
            },
        };

        /**
         * switcher decides if the domain needs to be chosen from a set of possible domains
         * or if it has a single possible domain type
         */
        auto switcher = OverloadedVisitor{
            [&](BSONElementDomainVariant& arg) { std::visit(handler, arg); },
            [&](BSONDomainMap& arg) {
                auto b = arg.begin();
                std::advance(b, absl::Uniform(prng, 0UL, arg.size()));
                auto domain = b->second;
                std::visit(handler, domain);
            },
        };

        std::visit(switcher, value);
    }

    return bob.obj();
}

void BSONObjImpl::Mutate(corpus_type& val,
                         absl::BitGenRef prng,
                         const fuzztest::domain_implementor::MutationMetadata& metadata,
                         bool only_shrink) {
    // We have to collect the elements since there is no way to know the size without a full
    // walk of the object
    std::vector<BSONElement> elements;
    for (auto&& elem : val) {
        elements.push_back(elem);
    }

    const bool canShrink = elements.size() > 0;
    const bool canRemove = elements.size() > 1;
    const bool canGrow = !only_shrink;
    const bool canChange = elements.size() != 0 && !only_shrink;

    int possible_actions = canShrink + canRemove + canGrow + canChange;
    if (possible_actions == 0) {
        // There are no actions we can perform, so just return the initial object
        val = Init(prng);
        return;
    }

    int action = absl::Uniform(prng, 0, possible_actions);
    if (canShrink) {
        if (action-- == 0) {
            // We need to shrink the object
            BSONObjBuilder bob = BSONObjBuilder();
            ShrinkRandomElement(bob, elements, prng, metadata);
            val = bob.obj();
            return;
        }
    }
    if (canRemove) {
        if (action-- == 0) {
            // We need to remove an element
            BSONObjBuilder bob = BSONObjBuilder();
            RemoveRandomElement(bob, elements, prng);
            val = bob.obj();
            return;
        }
    }
    if (canGrow) {
        if (action-- == 0) {
            // We need to grow the object
            BSONObjBuilder bob = BSONObjBuilder();
            addAllToBuilder(bob, elements);
            CreateRandomObj(bob, prng);
            val = bob.obj();
            return;
        }
    }
    if (canChange) {
        if (action-- == 0) {
            BSONObjBuilder bob = BSONObjBuilder();
            ModifyRandomElement(bob, elements, prng, metadata);
            val = bob.obj();
            return;
        }
    }
    MONGO_UNREACHABLE;
}

BSONObjImpl::value_type BSONObjImpl::GetValue(const corpus_type& corpus_value) const {
    return corpus_value.getOwned().sharedBuffer();
    // return corpus_value.sharedBuffer();
}

std::optional<BSONObjImpl::corpus_type> BSONObjImpl::FromValue(const value_type& user_value) const {
    return BSONObj(user_value);
}

std::optional<BSONObjImpl::corpus_type> BSONObjImpl::ParseCorpus(
    const fuzztest::internal::IRObject& obj) const {

    auto stringData = obj.GetScalar<std::string>();
    if (stringData.has_value()) {
        auto data = stringData.value();
        auto newbuff = allocator_aware::SharedBuffer<>(data.length());
        memcpy(newbuff.get(), data.data(), data.length());

        return BSONObj(newbuff);
    }
    return std::nullopt;
}

BSONPrinter BSONObjImpl::GetPrinter() const {
    return BSONPrinter();
}

absl::Status BSONObjImpl::ValidateCorpusValue(const BSONObjImpl::corpus_type& corpus_value) const {
    auto sharedBuf = corpus_value.sharedBuffer();

    auto isValid = validateBSON(sharedBuf.get(), sharedBuf.capacity());
    if (!isValid.isOK()) {
        return absl::InvalidArgumentError(isValid.reason());
    } else if (!corpus_value.isValid()) {
        return absl::InvalidArgumentError("Invalid BSONObj");
    }
    return absl::OkStatus();
}

fuzztest::internal::IRObject BSONObjImpl::SerializeCorpus(
    const BSONObjImpl::corpus_type& corpus) const {
    auto sharedBuf = corpus.sharedBuffer();
    return fuzztest::internal::IRObject(std::string(sharedBuf.get(), corpus.objsize()));
}

void BSONObjImpl::printElement(BSONElement element) {
    std::cout << fmt::format("BSONElement: Name: [{0}], Type: [{1}], Value: ",
                             absl::CHexEscape(element.fieldName()),
                             typeName(element.type()));
    try {
        visitDomain(
            OverloadedVisitor{
                [&](fuzztest::Domain<ConstSharedBuffer>) {
                    auto val = getElementValue<ConstSharedBuffer>(element);
                    std::cout << "[" << BSONObj(val).hexDump() << "]" << std::endl;
                },
                [&](fuzztest::Domain<std::optional<char>>) { std::cout << "[undef]" << std::endl; },
                [&](fuzztest::Domain<std::optional<int>>) { std::cout << "[null]" << std::endl; },
                [&](fuzztest::Domain<BSONRegExOwned>) {
                    auto val = getElementValue<BSONRegExOwned>(element);
                    std::cout << fmt::format("[pattern: [{0}], flags: [{1}]]",
                                             absl::CHexEscape(val.patternOwned),
                                             val.flagsOwned)
                              << std::endl;
                },
                [&](fuzztest::Domain<BSONCodeOwned>) {
                    auto val = getElementValue<BSONCodeOwned>(element);
                    std::cout << "[code: " << val.codeOwned << "]" << std::endl;
                },
                [&](fuzztest::Domain<BSONSymbolOwned>) {
                    auto val = getElementValue<BSONSymbolOwned>(element);
                    std::cout << "[symbol: " << val.symbolOwned << "]" << std::endl;
                },
                [&](fuzztest::Domain<BSONCodeWScopeOwned>) {
                    auto val = getElementValue<BSONCodeWScopeOwned>(element);
                    std::cout << fmt::format("[code: [{0}], obj: [{1}]]",
                                             absl::CHexEscape(val.codeOwned),
                                             val.scope.hexDump())
                              << std::endl;
                },
                [&](fuzztest::Domain<BSONDBRefOwned>) {
                    auto val = getElementValue<BSONDBRefOwned>(element);
                    std::cout << "[ns: [" << absl::CHexEscape(val.nsOwned) << "], oid: [" << val.oid
                              << "]]" << std::endl;
                },
                [&](fuzztest::Domain<Decimal128>) {
                    auto val = getElementValue<Decimal128>(element);
                    std::cout << "[Dec128: " << val.toString() << "]" << std::endl;
                },
                [&](fuzztest::Domain<BSONBinDataOwned>) {
                    auto val = getElementValue<BSONBinDataOwned>(element);
                    std::cout << fmt::format("[BinData SubType: {0}, Hex: {1}]",
                                             val.type,
                                             absl::CHexEscape(std::string(
                                                 reinterpret_cast<char*>(val.dataOwned.data()),
                                                 val.dataOwned.size())))
                              << std::endl;
                },
                [&](auto&& arg) {
                    auto val =
                        getElementValue<typename std::decay_t<decltype(arg)>::value_type>(element);
                    std::cout << val << std::endl;
                },
            },
            element.fieldName(),
            element.type());
    } catch (AssertionException&) {
        // if the output type doesn't match what is in the bson
        // and this is a valid bson type
        visitDomain(
            [&](auto&& arg) {
                std::cout << std::endl
                          << fmt::format(
                                 "Field does not match expected type: field: [{0}], Type received: "
                                 "[{1}], Type expected: [{2}]",
                                 element.fieldName(),
                                 typeName(element.type()),
                                 typeid(typename std::decay_t<decltype(arg)>::value_type).name())
                          << std::endl;
            },
            element.fieldName(),
            element.type());
        return;
    } catch (...) {
        std::cout << "Caught unkown exception while printing BSONElement" << std::endl;
        throw;
    }
}

void BSONObjImpl::ShrinkRandomElement(
    BSONObjBuilder& bob,
    std::vector<BSONElement>& elements,
    absl::BitGenRef prng,
    const fuzztest::domain_implementor::MutationMetadata& metadata) {

    size_t index = absl::Uniform(prng, 0UL, elements.size());
    // we want to actually just call mutate on the element setting shrink
    for (size_t i = 0; i < elements.size(); i++) {
        if (i != index) {
            bob.append(elements[i]);
        } else {

            visitDomain(
                [&, i](auto&& domain) {
                    typename std::decay_t<decltype(domain)>::corpus_type corpus;

                    // Try getting the corpus value to mutate
                    try {
                        auto optional_corpus = domain.FromValue(
                            getElementValue<typename std::decay_t<decltype(domain)>::value_type>(
                                elements[i]));

                        // check if we got a valid corpus value, if not, get a default value
                        if (optional_corpus.has_value()) {
                            corpus = optional_corpus.value();
                            // mutate with shrink_only == true
                            domain.Mutate(corpus, prng, metadata, true);

                        } else {
                            corpus = domain.Init(prng);
                        }
                    } catch (AssertionException&) {
                        // if the output type doesn't match what is in the bson
                        // and this is a valid bson type, then assume we changed types
                        // and get a default value for this new type
                        corpus = domain.Init(prng);
                    } catch (...) {
                        throw;
                    }

                    // convert the corpus back to the value type and insert it into bob
                    if constexpr (std::is_same_v<
                                      typename std::decay_t<decltype(domain)>::value_type,
                                      allocator_aware::ConstSharedBuffer<>>) {
                        bob.appendObject(elements[i].fieldName(), domain.GetValue(corpus).get());
                    } else if constexpr (std::is_same_v<
                                             typename std::decay_t<decltype(domain)>::value_type,
                                             std::optional<char>>) {
                        bob.appendUndefined(elements[i].fieldName());
                    } else if constexpr (std::is_same_v<
                                             typename std::decay_t<decltype(domain)>::value_type,
                                             std::optional<int>>) {
                        bob.appendNull(elements[i].fieldName());
                    } else {
                        bob.append(elements[i].fieldName(), domain.GetValue(corpus));
                    }
                },
                elements[i].fieldName(),
                elements[i].type());
        }
    }
}

void BSONObjImpl::RemoveRandomElement(BSONObjBuilder& bob,
                                      std::vector<BSONElement>& elements,
                                      absl::BitGenRef prng) {
    size_t index = absl::Uniform(prng, 0UL, elements.size());
    elements.erase(elements.begin() + index);
    addAllToBuilder(bob, elements);
}

void BSONObjImpl::CreateRandomObj(BSONObjBuilder& bob, absl::BitGenRef prng) {
    // select a random type from the full list of default bson element domains
    auto b = default_domains_.begin();
    std::advance(b, absl::Uniform(prng, 0UL, default_domains_.size()));
    auto type = b->first;

    // get a random field name. This may collide with a preconfigured field name, but that is
    // desired here to test validation code paths and invariants
    std::string fieldname = _fieldNameDomain.GetValue(_fieldNameDomain.Init(prng));

    // Create and append a new bson element of the randomly chosen type/name
    visitDomain(
        OverloadedVisitor{
            [&](fuzztest::Domain<ConstSharedBuffer>& arg) {
                bob.append(fieldname, BSONObj(arg.GetValue(arg.Init(prng))));
            },
            [&](fuzztest::Domain<std::optional<char>>) { bob.appendUndefined(fieldname); },
            [&](fuzztest::Domain<std::optional<int>>) { bob.appendNull(fieldname); },
            [&](auto& arg) { bob.append(fieldname, arg.GetValue(arg.Init(prng))); },
        },

        "\0",  // This should be an invalid field name, so it shouldn't appear in _inputElements
        type);
}

void BSONObjImpl::ModifyRandomElement(
    BSONObjBuilder& bob,
    std::vector<BSONElement>& elements,
    absl::BitGenRef prng,
    const fuzztest::domain_implementor::MutationMetadata& metadata) {

    // choose which element to modify
    size_t index = absl::Uniform(prng, 0UL, elements.size());

    for (size_t i = 0; i < elements.size(); i++) {
        if (i == index) {
            // choose what action to perform
            size_t availableActions = 1;
            if (auto e = _inputElements.find(elements[i].fieldName()); e == _inputElements.end()) {
                // element isn't in input elements map, so we can modify the type or name
                availableActions += 2;
            }
            size_t action = absl::Uniform(prng, 0UL, availableActions);

            if (action == 0) {
                // Change the value
                visitDomain(
                    [&, i](auto&& domain) {
                        typename std::decay_t<decltype(domain)>::corpus_type corpus;

                        // Try getting the corpus value to mutate
                        try {
                            auto optional_corpus = domain.FromValue(
                                getElementValue<
                                    typename std::decay_t<decltype(domain)>::value_type>(
                                    elements[i]));

                            // check if we got a valid corpus value, if not, get a default value
                            if (optional_corpus.has_value()) {
                                corpus = optional_corpus.value();
                                // mutate forcing the corpus value to shrink
                                domain.Mutate(corpus, prng, metadata, false);

                            } else {
                                corpus = domain.Init(prng);
                            }
                        } catch (AssertionException&) {
                            // if the output type doesn't match what is in the bson
                            // and this is a valid bson type, then assume we changed types
                            // and get a default value for this new type
                            corpus = domain.Init(prng);
                        } catch (...) {
                            throw;
                        }

                        // convert the corpus back to the value type and insert it into bob
                        if constexpr (std::is_same_v<
                                          typename std::decay_t<decltype(domain)>::value_type,
                                          allocator_aware::ConstSharedBuffer<>>) {
                            bob.appendObject(elements[i].fieldName(),
                                             domain.GetValue(corpus).get());
                        } else if constexpr (std::is_same_v<typename std::decay_t<
                                                                decltype(domain)>::value_type,
                                                            std::optional<char>>) {
                            bob.appendUndefined(elements[i].fieldName());
                        } else if constexpr (std::is_same_v<typename std::decay_t<
                                                                decltype(domain)>::value_type,
                                                            std::optional<int>>) {
                            bob.appendNull(elements[i].fieldName());
                        } else {
                            bob.append(elements[i].fieldName(), domain.GetValue(corpus));
                        }
                    },
                    elements[i].fieldName(),
                    elements[i].type());

            } else if (action == 1) {
                // change the field name
                auto originalFieldName =
                    _fieldNameDomain.FromValue(std::string(elements[i].fieldName()));
                if (originalFieldName.has_value()) {
                    auto newFieldName = originalFieldName.value();
                    _fieldNameDomain.Mutate(newFieldName, prng, metadata, false);
                    bob.appendAs(elements[i], _fieldNameDomain.GetValue(newFieldName));
                } else {
                    auto newFieldName = _fieldNameDomain.GetValue(_fieldNameDomain.Init(prng));
                    bob.appendAs(elements[i], newFieldName);
                }

            } else {
                // change the type
                // This won't change the type for fields defined in _inputElements
                auto b = default_domains_.begin();
                std::advance(b, absl::Uniform(prng, 0UL, default_domains_.size()));
                auto new_type = b->first;

                visitDomain(OverloadedVisitor{
                                [&](fuzztest::Domain<ConstSharedBuffer>& arg) {
                                    auto val = arg.GetValue(arg.Init(prng));
                                    bob.appendObject(elements[i].fieldName(), val.get());
                                },
                                [&](fuzztest::Domain<std::optional<char>>) {
                                    bob.appendUndefined(elements[i].fieldName());
                                },
                                [&](fuzztest::Domain<std::optional<int>>) {
                                    bob.appendNull(elements[i].fieldName());
                                },
                                [&](auto& arg) {
                                    auto val = arg.GetValue(arg.Init(prng));
                                    bob.append(elements[i].fieldName(), val);
                                },
                            },
                            elements[i].fieldName(),
                            new_type);
            }
        } else {
            bob.append(elements[i]);
        }
    }
}

BSONObjImpl& BSONObjImpl::WithAny(const std::string& name) {
    _inputElements.try_emplace(name, default_domains_);
    return *this;
}

template <typename F>
void BSONObjImpl::visitDomain(F visitor, std::string fieldName, BSONType domainType) {

    if (auto domain = _inputElements.find(fieldName); domain != _inputElements.end()) {
        if (std::holds_alternative<BSONDomainMap>(domain->second)) {
            auto domainSet = std::get<BSONDomainMap>(domain->second);
            if (auto inner = domainSet.find(domainType); inner != domainSet.end()) {
                std::visit(visitor, inner->second);
            } else {
                GTEST_FAIL() << "invalid BSON type requested from variant: fieldname: " << fieldName
                             << " type: " << typeName(domainType);
            }
        } else {
            std::visit(visitor, std::get<BSONElementDomainVariant>(domain->second));
        }

    } else if (auto domain = default_domains_.find(domainType); domain != default_domains_.end()) {
        std::visit(visitor, domain->second);
    } else {
        GTEST_FAIL() << "invalid BSON type: fieldname: " << fieldName
                     << " type: " << typeName(domainType);
    }
}

void BSONObjImpl::addAllToBuilder(BSONObjBuilder& builder, std::vector<BSONElement>& elements) {

    for (auto&& elem : elements) {
        builder.append(elem);
    }
}


// Assertions will be thrown automatically if the elements type does not match T
template <typename T>
T BSONObjImpl::getElementValue(const BSONElement& elem) {
    if constexpr (std::is_same_v<T, double>) {
        return elem.Double();
    } else if constexpr (std::is_same_v<T, std::basic_string<char>>) {
        return elem.String();
    } else if constexpr (std::is_same_v<T, BSONObj>) {
        return elem.Obj();
    } else if constexpr (std::is_same_v<T, BSONArray>) {
        return BSONArray(elem.Obj().getOwned());
    } else if constexpr (std::is_same_v<T, BSONBinDataOwned>) {
        int len = 0;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(elem.binDataClean(len));
        return BSONBinDataOwned(std::vector<uint8_t>(data, data + len), elem.binDataType());
    } else if constexpr (std::is_same_v<T, std::optional<char>>) {
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, OID>) {
        return elem.OID();
    } else if constexpr (std::is_same_v<T, bool>) {
        return elem.Bool();
    } else if constexpr (std::is_same_v<T, Date_t>) {
        return elem.Date();
    } else if constexpr (std::is_same_v<T, std::optional<int>>) {
        return std::nullopt;
    } else if constexpr (std::is_same_v<T, BSONRegExOwned>) {
        return BSONRegExOwned(std::string(elem.regex()), std::string(elem.regexFlags()));
    } else if constexpr (std::is_same_v<T, BSONDBRefOwned>) {
        return BSONDBRefOwned(elem.dbrefNS(), elem.dbrefOID());
    } else if constexpr (std::is_same_v<T, BSONCodeOwned>) {
        return BSONCodeOwned(elem._asCode());
    } else if constexpr (std::is_same_v<T, BSONSymbolOwned>) {
        return BSONSymbolOwned(elem.str());
    } else if constexpr (std::is_same_v<T, BSONCodeWScopeOwned>) {
        return BSONCodeWScopeOwned(std::string(elem.codeWScopeCode(), elem.codeWScopeCodeLen()),
                                   BSONObj(elem.codeWScopeScopeData()));
    } else if constexpr (std::is_same_v<T, int>) {
        return elem.Int();
    } else if constexpr (std::is_same_v<T, Timestamp>) {
        return elem.timestamp();
    } else if constexpr (std::is_same_v<T, long long>) {
        return elem.Long();
    } else if constexpr (std::is_same_v<T, Decimal128>) {
        return elem.Decimal();
    } else if constexpr (std::is_same_v<T, allocator_aware::ConstSharedBuffer<>>) {
        return elem.Obj().getOwned().sharedBuffer();
    } else {
        GTEST_FAIL() << "Type not supported";
    }
}

#define BSON_MUTATOR_WITH_FIELD_IMPL(Camel, cpptype, bsontype, defaultDomain)                   \
    BSONObjImpl& BSONObjImpl::With##Camel(const std::string& name,                              \
                                          const fuzztest::Domain<cpptype> domain) {             \
        _inputElements.try_emplace(name, BSONDomainVariant(BSONElementDomainVariant(domain)));  \
        return *this;                                                                           \
    }                                                                                           \
    BSONObjImpl& BSONObjImpl::With##Camel(const std::string& name) {                            \
        _inputElements.try_emplace(name,                                                        \
                                   BSONDomainVariant(BSONElementDomainVariant(defaultDomain))); \
        return *this;                                                                           \
    }

BSON_MUTATOR_EXPAND_FIELD_TYPES(BSON_MUTATOR_WITH_FIELD_IMPL)


}  // namespace mongo::bson_mutator
