// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelementvalue.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"

#include <array>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

/**
 * Sink that prevents the compiler from eliminating dead reads in optimized builds.
 */
template <typename... Args>
[[clang::optnone]] void doNotOptimize(const Args&...) {}

/**
 * Utility to be used on the BSONObj and BSONElement API to ignore `DBException`s, which are
 * not buffer overreads.
 */
template <typename Fn, typename... Args>
void invokeAndIgnoreDBExceptions(Fn&& fn, Args&&... args) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Fn, Args...>>) {
            std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...);
        } else {
            doNotOptimize(std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...));
        }
    } catch (const DBException&) {
    }
}

/**
 * Multiple call variant of `invokeAndIgnoreDBExceptions`.
 */
template <typename Target, typename... Calls>
void exerciseCalls(Target&& target, Calls&&... calls) {
    (invokeAndIgnoreDBExceptions(std::forward<Calls>(calls), std::forward<Target>(target)), ...);
}

void exerciseObject(const BSONObj& obj, int depth);

void exerciseElement(const BSONElement& el, int depth) {
    int binLen = 0;
    int coercedInt = 0;
    long long coercedLong = 0;
    double coercedDouble = 0.0;
    bool coercedBool = false;
    std::string coercedStr;
    Decimal128 coercedDec;
    std::vector<std::string> coercedStrVec;
    Date_t valDate{};
    BSONObj valObj;
    mongo::OID valOid;
    exerciseCalls(
        el,
        &BSONElement::size,
        &BSONElement::valuesize,
        &BSONElement::fieldNameStringData,
        [](const BSONElement& e) {
            return std::string_view(e.fieldName(), e.fieldNameSize() ? e.fieldNameSize() - 1 : 0);
        },
        &BSONElement::isABSONObj,
        &BSONElement::isNumber,
        &BSONElement::isNull,
        [](const BSONElement& e) { return e.isBinData(BinDataType::BinDataGeneral); },
        &BSONElement::trueValue,
        &BSONElement::isNaN,
        &BSONElement::valueStringData,
        &BSONElement::valueStringDataSafe,
        &BSONElement::valuestrsize,
        &BSONElement::objsize,
        &BSONElement::str,
        &BSONElement::String,
        &BSONElement::checkAndGetStringData,
        &BSONElement::_asCode,
        &BSONElement::_binDataVector,
        &BSONElement::uuid,
        &BSONElement::md5,
        &BSONElement::regex,
        &BSONElement::regexFlags,
        &BSONElement::dbrefNS,
        &BSONElement::dbrefOID,
        &BSONElement::codeWScopeCode,
        &BSONElement::codeWScopeCodeLen,
        &BSONElement::codeWScopeScopeData,
        &BSONElement::_numberDouble,
        &BSONElement::_numberInt,
        &BSONElement::_numberDecimal,
        &BSONElement::_numberLong,
        &BSONElement::numberDouble,
        &BSONElement::number,
        &BSONElement::safeNumberDouble,
        &BSONElement::numberInt,
        &BSONElement::safeNumberInt,
        &BSONElement::numberLong,
        &BSONElement::safeNumberLong,
        &BSONElement::safeNumberLongForHash,
        &BSONElement::exactNumberLong,
        &BSONElement::parseIntegerElementToLong,
        &BSONElement::parseIntegerElementToNonNegativeLong,
        &BSONElement::parseIntegerElementToInt,
        &BSONElement::parseIntegerElementToNonNegativeInt,
        &BSONElement::numberDecimal,
        &BSONElement::Number,
        &BSONElement::Double,
        &BSONElement::Int,
        &BSONElement::Long,
        &BSONElement::Decimal,
        &BSONElement::boolean,
        &BSONElement::booleanSafe,
        &BSONElement::Bool,
        &BSONElement::date,
        &BSONElement::Date,
        &BSONElement::timestamp,
        &BSONElement::timestampTime,
        &BSONElement::timestampInc,
        &BSONElement::timestampValue,
        &BSONElement::OID,
        &BSONElement::__oid,
        &BSONElement::Obj,
        &BSONElement::Array,
        &BSONElement::embeddedObjectUserCheck,
        [&](const BSONElement& e) {
            e.Val(valDate);
            doNotOptimize(valDate);
        },
        [&](const BSONElement& e) {
            e.Val(coercedLong);
            doNotOptimize(coercedLong);
        },
        [&](const BSONElement& e) {
            e.Val(coercedDec);
            doNotOptimize(coercedDec);
        },
        [&](const BSONElement& e) {
            e.Val(coercedBool);
            doNotOptimize(coercedBool);
        },
        [&](const BSONElement& e) {
            e.Val(valObj);
            doNotOptimize(valObj);
        },
        [&](const BSONElement& e) {
            e.Val(valOid);
            doNotOptimize(valOid);
        },
        [&](const BSONElement& e) {
            e.Val(coercedInt);
            doNotOptimize(coercedInt);
        },
        [&](const BSONElement& e) {
            e.Val(coercedDouble);
            doNotOptimize(coercedDouble);
        },
        [&](const BSONElement& e) {
            e.Val(coercedStr);
            doNotOptimize(coercedStr);
        },
        [&](const BSONElement& e) { doNotOptimize(e.binData(binLen), binLen); },
        [&](const BSONElement& e) { doNotOptimize(e.binDataClean(binLen), binLen); },
        [](const BSONElement& e) { return e.binDataType(); },
        [](const BSONElement& e) { return e["a"]; },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedInt), coercedInt); },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedLong), coercedLong); },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedDouble), coercedDouble); },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedBool), coercedBool); },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedStr), coercedStr); },
        [&](const BSONElement& e) { doNotOptimize(e.tryCoerce(&coercedDec), coercedDec); },
        [&](const BSONElement& e) { doNotOptimize(e.coerce(&coercedStrVec), coercedStrVec); },
        [](const BSONElement& e) { return e.toString(); },
        [](const BSONElement& e) {
            // TODO: For this case, jsonString on an eoo (or an invalid type) would fail.
            // It should be discussed if the MONGO_UNREACHABLE internal to jsonString should
            // be changed to a tassert. Or if the failure should be silenced in fuzz tests.
            if (!e.eoo()) {
                doNotOptimize(e.jsonString(ExtendedCanonicalV2_0_0, false));
            }
        },
        [](const BSONElement& e) {
            if (!e.eoo()) {
                doNotOptimize(e.jsonString(ExtendedRelaxedV2_0_0, false));
            }
        },
        [](const BSONElement& e) {
            if (!e.eoo()) {
                doNotOptimize(e.jsonString(LegacyStrict, false));
            }
        },
        [](const BSONElement& e) {
            // Calling `wrap()` on an eoo element is an invariant.
            if (!e.eoo()) {
                doNotOptimize(e.wrap());
            }
        },
        [](const BSONElement& e) {
            if (!e.eoo()) {
                doNotOptimize(e.wrap("x"));
            }
        },
        [&](const BSONElement& e) { exerciseObject(e.embeddedObject(), depth + 1); },
        [&](const BSONElement& e) { exerciseObject(e.codeWScopeObject(), depth + 1); },
        [](const BSONElement& e) {
            if (e.type() == BSONType::object) {
                doNotOptimize(BSONElementValue(e.value()).Obj());
            } else if (e.type() == BSONType::array) {
                doNotOptimize(BSONElementValue(e.value()).Array());
            } else if (e.type() == BSONType::codeWScope) {
                doNotOptimize(BSONElementValue(e.value()).CodeWScopeObj());
            }
        });
}

void exerciseObject(const BSONObj& obj, int depth) {
    // Constants limiting the fuzz test from infinitely iterating through BSONObj.
    static constexpr int kMaxDepth = 8;
    static constexpr int kMaxElemsIterated = 32;
    if (depth > kMaxDepth) {
        return;
    }

    exerciseCalls(
        obj,
        &BSONObj::objsize,
        &BSONObj::isEmpty,
        &BSONObj::firstElementType,
        &BSONObj::firstElementFieldName,
        &BSONObj::firstElementFieldNameStringData,
        [&](const BSONObj& o) { exerciseElement(o.firstElement(), depth); },
        [&](const BSONObj& o) {
            BSONObjIterator it(o);
            for (int n = 0; it.more() && n < kMaxElemsIterated; ++n) {
                invokeAndIgnoreDBExceptions([&] { exerciseElement(*it, depth); });
                exerciseElement(it.next(), depth);
            }

            // Iterator has possibly passed the end iterator.
            exerciseCalls(
                it, &BSONObjIterator::next, &BSONObjIterator::operator*, &BSONObjIterator::next);
        },
        [&](const BSONObj& o) {
            auto it = o.begin();
            const auto end = o.end();
            for (int n = 0; it != end && n < kMaxElemsIterated; ++n, ++it) {
                exerciseElement(*it, depth);
            }

            // Iterator has possibly passed the end iterator.
            invokeAndIgnoreDBExceptions([&] { ++it; });
        },
        [](const BSONObj& o) { return o[0]; },
        &BSONObj::nFields,
        &BSONObj::couldBeArray,
        &BSONObj::hasFieldNames,
        [](const BSONObj& o) { return o.getFieldNames<std::set<std::string>>(); },
        [](const BSONObj& o) {
            std::array<BSONElement, 2> fields;
            o.getFields(std::array<std::string_view, 2>{"a", "s"}, &fields);
            doNotOptimize(fields);
        },
        &BSONObj::storageValidEmbedded,
        [](const BSONObj& o) { return o.isPrefixOf(o, SimpleBSONElementComparator::kInstance); },
        [](const BSONObj& o) { return o.isFieldNamePrefixOf(o); },
        [](const BSONObj& o) { return o.redact(); },
        &BSONObj::clientReadable,
        &BSONObj::hexDump,
        &BSONObj::stripFieldNames,
        [](const BSONObj& o) { return o.toString(); },
        [](const BSONObj& o) { return o.jsonString(ExtendedCanonicalV2_0_0, 0); },
        [](const BSONObj& o) { return o.jsonString(ExtendedRelaxedV2_0_0, 0); },
        [](const BSONObj& o) { return o.jsonString(LegacyStrict, 0); },
        [](const BSONObj& o) { return o.woCompare(o); },
        [](const BSONObj& o) { return o.binaryEqual(o); },
        &BSONObj::copy,
        [](const BSONObj& o) { return o.getOwned(); },
        [](const BSONObj& o) { return BSONObj(o.objdata()).getOwned(); },
        [](const BSONObj& o) {
            if (o.isOwned()) {
                doNotOptimize(BSONObj(o.objdata()).shareOwnershipWith(o));
                doNotOptimize(BSONObj(o.objdata()).shareOwnershipWith(o.sharedBuffer()));
            }
        });
}

class BSONSpatialSafetyFuzzTestBase {
public:
    BSONSpatialSafetyFuzzTestBase() {
        // Ignore the debugger trap (which fails the fuzz test) used by some assertions.
        std::signal(SIGTRAP, SIG_IGN);
    }

    // Copies raw bytes into a SharedBuffer.
    static SharedBuffer createBuffer(const std::string& rawBytes) {
        const size_t size = rawBytes.size();
        auto storage = SharedBuffer::allocate(size);
        std::memcpy(storage.get(), rawBytes.data(), size);
        return storage;
    }
};

class BSONObjSpatialSafetyFuzzTest : public BSONSpatialSafetyFuzzTestBase {
public:
    // Seed inputs as "hints" to the fuzz test. The many different types means the fuzz test
    // has immediate coverage of various type specific methods. This is not required, but helps
    // with the fuzz test coverage "ramp up".
    static inline const std::string kRichDocStr = [] {
        const char bin[] = {'x', 'y'};
        // Used for coverage for BinDataType::ByteArrayDeprecated.
        const char deprecatedBin[] = {2, 0, 0, 0, 'x', 'y'};
        BSONObjBuilder bob;
        bob.append("a", 1);
        bob.append("s", "hello");
        bob.append("sym", BSONSymbol("sym"));
        bob.append("code", BSONCode("f()"));
        bob.append("b", BSONBinData(bin, sizeof(bin), BinDataType::BinDataGeneral));
        bob.append(
            "b2",
            BSONBinData(deprecatedBin, sizeof(deprecatedBin), BinDataType::ByteArrayDeprecated));
        bob.append("o", BSON("n" << 1));
        bob.append("arr", BSON_ARRAY(1 << "two" << BSON("k" << true)));
        bob.append("r", BSONRegEx("pat", "im"));
        bob.append("d", BSONDBRef("db.coll", OID()));
        bob.append("c", BSONCodeWScope("function(){}", BSON("x" << 42)));
        bob.append("dbl", 3.14);
        bob.append("lng", 99LL);
        bob.append("dec", Decimal128(1));
        bob.append("bool", true);
        bob.append("dt", Date_t::fromMillisSinceEpoch(1000));
        bob.append("oid", OID());
        bob.appendNull("null");
        bob.appendUndefined("undef");
        bob.append("ts", Timestamp(Seconds(1), 2));
        const BSONObj doc = bob.obj();
        return std::string(doc.objdata(), static_cast<size_t>(doc.objsize()));
    }();
    static inline const std::string kEmptyDocStr = [] {
        const BSONObj doc;
        return std::string(doc.objdata(), static_cast<size_t>(doc.objsize()));
    }();

    void FuzzBSONObjBufferOverread(const std::string& rawBytes) {
        auto storage = createBuffer(rawBytes);

        std::optional<BSONObj> obj;

        // Test BSONObj construction with a ConstSharedBuffer.
        invokeAndIgnoreDBExceptions([&] { obj.emplace(storage); });
        if (!obj) {
            return;
        }

        exerciseObject(*obj, /*depth=*/0);
    }

    void FuzzBSONObjFieldAccess(const std::string& rawBytes, const std::string& fieldName) {
        auto storage = createBuffer(rawBytes);

        std::optional<BSONObj> obj;
        invokeAndIgnoreDBExceptions([&] { obj.emplace(storage); });
        if (!obj) {
            return;
        }

        exerciseCalls(
            *obj,
            [&](const BSONObj& target) {
                exerciseElement(target.getField(fieldName), /*depth=*/0);
            },
            [&](const BSONObj& target) { return target[fieldName]; },
            [&](const BSONObj& target) { return target.hasField(fieldName); },
            [&](const BSONObj& target) { return target.hasElement(fieldName); },
            [&](const BSONObj& target) { return target.getStringField(fieldName); },
            [&](const BSONObj& target) { return target.getIntField(fieldName); },
            [&](const BSONObj& target) { return target.getBoolField(fieldName); },
            [&](const BSONObj& target) { return target.getObjectField(fieldName); });
    }
};

class BSONElementSpatialSafetyFuzzTest : public BSONSpatialSafetyFuzzTestBase {
public:
    // Seed with each individual element from `kRichDocStr` so all BSON types are immediately
    // represented in the element corpus.
    static inline const std::vector<std::tuple<std::string>> kElementSeeds = [] {
        std::vector<std::tuple<std::string>> seeds;
        const BSONObj richDoc(BSONObjSpatialSafetyFuzzTest::kRichDocStr.data());
        for (const auto& elem : richDoc) {
            seeds.emplace_back(std::string(elem.rawdata(), static_cast<size_t>(elem.size())));
        }
        seeds.emplace_back("");
        return seeds;
    }();

    // Seeds for comparison of two elements should be two of the same element. Here, we represent
    // that with the tuple (`baseBytes` : std::string, `mutationOffset` : uint32_t, and
    // `mutationBytes` : std::string). `mutationOffset` and `mutationBytes` determine how much of
    // `baseBytes` should be changed for the comparison element, and start at 0 and "" so that
    // fuzz test starts at 2 of the same element before comparison.
    static inline const std::vector<std::tuple<std::string, uint32_t, std::string>>
        kComparisonSeeds = [] {
            std::vector<std::tuple<std::string, uint32_t, std::string>> seeds;
            for (const auto& [bytes] : kElementSeeds) {
                seeds.emplace_back(bytes, 0u, std::string());
            }
            return seeds;
        }();

    void FuzzBSONElementBufferOverread(const std::string& rawBytes) {
        auto storage = createBuffer(rawBytes);

        std::optional<BSONElement> elem;

        // Test the "untrusted" span constructor for BSONElement, which is not covered by the
        // BSONObj fuzz test.
        invokeAndIgnoreDBExceptions(
            [&] { elem.emplace(std::span<const char>(storage.get(), rawBytes.size())); });
        if (!elem) {
            return;
        }

        exerciseElement(*elem, /*depth=*/0);
    }

    void FuzzBSONElementComparison(const std::string& baseBytes,
                                   uint32_t mutationOffset,
                                   const std::string& mutationBytes) {
        // Construct `rhsBytes` as a mutated copy of `baseBytes` rather than an independent
        // buffer. Comparing two unrelated random buffers rarely produces two valid elements with
        // matching types. Mutating a part of `baseBytes` has a higher chance of preserving the
        // element's type and size while varying its field name or value.
        std::string rhsBytes = baseBytes;
        if (!rhsBytes.empty() && !mutationBytes.empty()) {
            const size_t offset = static_cast<size_t>(mutationOffset) % rhsBytes.size();
            const size_t len = std::min(mutationBytes.size(), rhsBytes.size() - offset);
            std::memcpy(rhsBytes.data() + offset, mutationBytes.data(), len);
        }

        auto lhsStorage = createBuffer(baseBytes);
        auto rhsStorage = createBuffer(rhsBytes);

        std::optional<BSONElement> lhs;
        std::optional<BSONElement> rhs;
        invokeAndIgnoreDBExceptions([&] {
            lhs.emplace(std::span<const char>(lhsStorage.get(), baseBytes.size()));
            rhs.emplace(std::span<const char>(rhsStorage.get(), rhsBytes.size()));
        });
        if (!lhs || !rhs) {
            return;
        }

        exerciseCalls(
            *lhs,
            [&](const BSONElement& l) { return l.binaryEqual(*rhs); },
            [&](const BSONElement& l) { return l.binaryEqualValues(*rhs); },
            [&](const BSONElement& l) {
                return l.woCompare(*rhs, BSONElement::ComparisonRules::kConsiderFieldName);
            },
            [&](const BSONElement& l) { return l.woCompare(*rhs, 0); },
            [&](const BSONElement& l) {
                return BSONElement::compareElements(l, *rhs, 0, nullptr);
            });
    }
};

FUZZ_TEST_F(BSONObjSpatialSafetyFuzzTest, FuzzBSONObjBufferOverread)
    .WithSeeds({{BSONObjSpatialSafetyFuzzTest::kRichDocStr},
                {BSONObjSpatialSafetyFuzzTest::kEmptyDocStr}});

FUZZ_TEST_F(BSONObjSpatialSafetyFuzzTest, FuzzBSONObjFieldAccess)
    .WithSeeds({{BSONObjSpatialSafetyFuzzTest::kRichDocStr, "a"},
                {BSONObjSpatialSafetyFuzzTest::kEmptyDocStr, ""}});

FUZZ_TEST_F(BSONElementSpatialSafetyFuzzTest, FuzzBSONElementBufferOverread)
    .WithSeeds(BSONElementSpatialSafetyFuzzTest::kElementSeeds);

FUZZ_TEST_F(BSONElementSpatialSafetyFuzzTest, FuzzBSONElementComparison)
    .WithSeeds(BSONElementSpatialSafetyFuzzTest::kComparisonSeeds);

// Sanity check for seed inputs.
TEST(BSONSpatialSafetyFuzzTest, HarnessAcceptsValidObject) {
    BSONObjSpatialSafetyFuzzTest objFixture;
    for (const auto& bytes :
         {BSONObjSpatialSafetyFuzzTest::kRichDocStr, BSONObjSpatialSafetyFuzzTest::kEmptyDocStr}) {
        objFixture.FuzzBSONObjBufferOverread(bytes);
    }
}

}  // namespace
}  // namespace mongo
