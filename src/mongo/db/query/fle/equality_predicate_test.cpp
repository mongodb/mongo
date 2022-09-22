/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/equality_predicate.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::fle {
namespace {
class MockEqualityPredicate : public EqualityPredicate {
public:
    MockEqualityPredicate(const QueryRewriterInterface* rewriter) : EqualityPredicate(rewriter) {}
    MockEqualityPredicate(const QueryRewriterInterface* rewriter,
                          TagMap tags,
                          std::set<StringData> encryptedFields)
        : EqualityPredicate(rewriter), _tags(tags), _encryptedFields(encryptedFields) {}

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, std::vector<PrfBlock> tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
    }

    void addEncryptedField(StringData field) {
        _encryptedFields.insert(field);
    }


protected:
    bool isPayload(const BSONElement& elt) const override {
        return _encryptedFields.find(elt.fieldNameStringData()) != _encryptedFields.end();
    }

    bool isPayload(const Value& v) const override {
        return true;
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const {
        return stdx::visit(
            OverloadedVisitor{
                [&](BSONElement p) {
                    ASSERT(p.isNumber());  // Only accept numbers as mock FFPs.
                    ASSERT(_tags.find({p.fieldNameStringData(), p.Int()}) != _tags.end());
                    return _tags.find({p.fieldNameStringData(), p.Int()})->second;
                },
                [&](std::reference_wrapper<Value> v) { return std::vector<PrfBlock>{}; }},
            payload);
    }

private:
    TagMap _tags;
    std::set<StringData> _encryptedFields;
};

class EqualityPredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    EqualityPredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockEqualityPredicate _predicate;
};

TEST_F(EqualityPredicateRewriteTest, Equality_NoFFP) {
    std::unique_ptr<MatchExpression> input =
        std::make_unique<EqualityMatchExpression>("ssn", Value(5));
    auto expected = EqualityMatchExpression("ssn", Value(5));

    auto result = _predicate.rewrite(input.get());
    ASSERT(result == nullptr);
    ASSERT(input->equivalent(&expected));
}

TEST_F(EqualityPredicateRewriteTest, In_NoFFP) {
    auto input = makeInExpr("name",
                            BSON_ARRAY("harry"
                                       << "ron"
                                       << "hermione"));
    auto expected = makeInExpr("name",
                               BSON_ARRAY("harry"
                                          << "ron"
                                          << "hermione"));

    auto result = _predicate.rewrite(input.get());
    ASSERT(result == nullptr);
    ASSERT(input->equivalent(expected.get()));
}

TEST_F(EqualityPredicateRewriteTest, Equality_Basic) {
    auto input = EqualityMatchExpression("ssn", Value(5));
    std::vector<PrfBlock> tags = {{1}, {2}, {3}};

    _predicate.setEncryptedTags({"ssn", 5}, tags);

    assertRewriteToTags(_predicate, &input, toBSONArray(std::move(tags)));
}

TEST_F(EqualityPredicateRewriteTest, In_Basic) {
    auto input = makeInExpr("ssn", BSON_ARRAY(2 << 4 << 6));

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});
    _predicate.setEncryptedTags({"2", 6}, {{99}, {100}});

    assertRewriteToTags(_predicate, input.get(), toBSONArray({{1}, {2}, {3}, {5}, {99}, {100}}));
}

TEST_F(EqualityPredicateRewriteTest, In_NotAllFFPs) {
    auto input = makeInExpr("ssn", BSON_ARRAY(2 << 4 << 6));

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});

    ASSERT_THROWS_CODE(
        assertRewriteToTags(_predicate, input.get(), toBSONArray({{1}, {2}, {3}, {5}})),
        AssertionException,
        6329400);
}

template <typename T>
std::vector<uint8_t> toEncryptedVector(EncryptedBinDataType dt, T t) {
    BSONObj obj = t.toBSON();

    std::vector<uint8_t> buf(obj.objsize() + 1);
    buf[0] = static_cast<uint8_t>(dt);

    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), buf.data() + 1);

    return buf;
}

template <typename T>
void toEncryptedBinData(StringData field, EncryptedBinDataType dt, T t, BSONObjBuilder* builder) {
    auto buf = toEncryptedVector(dt, t);

    builder->appendBinData(field, buf.size(), BinDataType::Encrypt, buf.data());
}

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

const FLEIndexKey& getIndexKey() {
    static std::string indexVec = hexblob::decode(
        "7dbfebc619aa68a659f64b8e23ccd21644ac326cb74a26840c3d2420176c40ae088294d00ad6cae9684237b21b754cf503f085c25cd320bf035c3417416e1e6fe3d9219f79586582112740b2add88e1030d91926ae8afc13ee575cfb8bb965b7"_sd);
    static FLEIndexKey indexKey(KeyMaterial(indexVec.begin(), indexVec.end()));
    return indexKey;
}

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "a7ddbc4c8be00d51f68d9d8e485f351c8edc8d2206b24d8e0e1816d005fbe520e489125047d647b0d8684bfbdbf09c304085ed086aba6c2b2b1677ccc91ced8847a733bf5e5682c84b3ee7969e4a5fe0e0c21e5e3ee190595a55f83147d8de2a"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}


BSONObj generateFFP(StringData path, int value) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    BSONObj doc = BSON("value" << value);
    auto element = doc.firstElement();
    auto fpp = FLEClientCrypto::serializeFindPayload(indexKeyAndId, userKeyAndId, element, 0);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindEqualityPayload, fpp, &builder);
    return builder.obj();
}

std::unique_ptr<MatchExpression> generateEqualityWithFFP(StringData path, int value) {
    auto ffp = generateFFP(path, value);
    return std::make_unique<EqualityMatchExpression>(path, ffp.firstElement());
}

std::unique_ptr<Expression> generateEqualityWithFFP(ExpressionContext* const expCtx,
                                                    StringData path,
                                                    int value) {
    auto ffp = Value(generateFFP(path, value).firstElement());
    auto ffpExpr = make_intrusive<ExpressionConstant>(expCtx, ffp);
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children = {std::move(fieldpath),
                                                              std::move(ffpExpr)};
    return std::make_unique<ExpressionCompare>(expCtx, ExpressionCompare::EQ, std::move(children));
}

std::unique_ptr<MatchExpression> generateDisjunctionWithFFP(StringData path,
                                                            std::initializer_list<int> vals) {
    BSONArrayBuilder bab;
    for (auto& value : vals) {
        bab.append(generateFFP(path, value).firstElement());
    }
    auto arr = bab.arr();
    return EncryptedPredicateRewriteTest::makeInExpr(path, arr);
}

std::unique_ptr<Expression> generateDisjunctionWithFFP(ExpressionContext* const expCtx,
                                                       StringData path,
                                                       std::initializer_list<int> values) {
    std::vector<boost::intrusive_ptr<Expression>> ffps;
    for (auto& value : values) {
        auto ffp = make_intrusive<ExpressionConstant>(
            expCtx, Value(generateFFP(path, value).firstElement()));
        ffps.emplace_back(std::move(ffp));
    }
    auto ffpArray = make_intrusive<ExpressionArray>(expCtx, std::move(ffps));
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children{std::move(fieldpath),
                                                           std::move(ffpArray)};
    return std::make_unique<ExpressionIn>(expCtx, std::move(children));
}

class EqualityPredicateCollScanRewriteTest : public EncryptedPredicateRewriteTest {
public:
    EqualityPredicateCollScanRewriteTest() : _predicate(&_mock) {
        _mock.setForceEncryptedCollScanForTest();
    }

protected:
    MockEqualityPredicate _predicate;
};

TEST_F(EqualityPredicateCollScanRewriteTest, Eq_Match) {
    auto input = generateEqualityWithFFP("ssn", 1);
    _predicate.addEncryptedField("ssn");

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    ASSERT_EQ(result->matchType(), MatchExpression::EXPRESSION);
    auto* expr = static_cast<ExprMatchExpression*>(result.get());
    auto aggExpr = expr->getExpression();

    auto expected = fromjson(R"({
        "$_internalFleEq": {
            "field": "$ssn",
            "edc": {
                "$binary": {
                    "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                    "subType": "6"
                }
            },
            "counter": {
                "$numberLong": "0"
            },
            "server": {
                "$binary": {
                    "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT_BSONOBJ_EQ(aggExpr->serialize(false).getDocument().toBson(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, Eq_Expr) {
    auto expCtx = _mock.getExpressionContext();
    auto input = generateEqualityWithFFP(expCtx, "ssn", 1);
    _predicate.addEncryptedField("ssn");

    auto result = _predicate.rewrite(input.get());

    auto expected = fromjson(R"({
        "$_internalFleEq": {
            "field": "$ssn",
            "edc": {
                "$binary": {
                    "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                    "subType": "6"
                }
            },
            "counter": {
                "$numberLong": "0"
            },
            "server": {
                "$binary": {
                    "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result->serialize(false).getDocument().toBson(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, In_Match) {
    auto input = generateDisjunctionWithFFP("ssn", {1, 2, 3});
    _predicate.addEncryptedField("0");
    _predicate.addEncryptedField("1");
    _predicate.addEncryptedField("2");

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    ASSERT_EQ(result->matchType(), MatchExpression::OR);
    auto expected = fromjson(R"({
    "$or": [
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CLpCo6rNuYMVT+6n1HCX15MNrVYDNqf6udO46ayo43Sw",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CPi44oCQHnNDeRqHsNLzbdCeHt2DK/wCly0g2dxU5fqN",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }
            }
        }
    ]
})");
    ASSERT_BSONOBJ_EQ(result->serialize(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, In_Expr) {
    auto input = generateDisjunctionWithFFP(_mock.getExpressionContext(), "ssn", {1, 1});
    _predicate.addEncryptedField("0");
    _predicate.addEncryptedField("1");
    _predicate.addEncryptedField("2");

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    auto expected = fromjson(R"({ "$or" : [ {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }},
                {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "edc": {
                        "$binary": {
                            "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                            "subType": "6"
                        }
                    },
                    "counter": {
                        "$numberLong": "0"
                    },
                    "server": {
                        "$binary": {
                            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
                            "subType": "6"
                        }
                    }
                }}
            ]})");
    ASSERT_BSONOBJ_EQ(result->serialize(false).getDocument().toBson(), expected);
}

}  // namespace
}  // namespace mongo::fle
