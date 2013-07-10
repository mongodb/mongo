/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "mongo/db/ops/modifier_bit.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::ModifierBit;
    using mongo::ModifierInterface;
    using mongo::Status;
    using mongo::StringData;
    using mongo::fromjson;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $bit mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj)
            : _modObj(modObj)
            , _mod() {
            ASSERT_OK(_mod.init(_modObj["$bit"].embeddedObject().firstElement()));
        }

        Status prepare(Element root,
                       const StringData& matchedField,
                       ModifierInterface::ExecInfo* execInfo) {
            return _mod.prepare(root, matchedField, execInfo);
        }

        Status apply() const {
            return _mod.apply();
        }

        Status log(Element logRoot) const {
            return _mod.log(logRoot);
        }

        ModifierBit& mod() { return _mod; }

    private:
        BSONObj _modObj;
        ModifierBit _mod;
    };


    TEST(Init, FailToInitWithInvalidValue) {
        BSONObj modObj;
        ModifierBit mod;

        // String is an invalid $bit argument
        modObj = fromjson("{ $bit : { a : '' } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));

        // Array is an invalid $bit argument
        modObj = fromjson("{ $bit : { a : [] } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));

        // An object with value not in ('and', 'or') is an invalid $bit argument
        modObj = fromjson("{ $bit : { a : { foo : 4 } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));

        // The argument to the sub-operator must be numeric
        modObj = fromjson("{ $bit : { a : { or : [] } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));

        modObj = fromjson("{ $bit : { a : { or : 'foo' } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));

        // The argument to the sub-operator must be integral
        modObj = fromjson("{ $bit : { a : { or : 1.0 } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement()));
    }

    TEST(Init, ParsesAndInt) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1)))));
    }

    TEST(Init, ParsesOrInt) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));
    }

    TEST(Init, ParsesAndLong) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));
    }

    TEST(Init, ParsesOrLong) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));
    }

    TEST(SimpleMod, PrepareOKTargetNotFound) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;

        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareOKTargetFound) {
        Document doc(fromjson("{ a : 1 }"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;

        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, PrepareSimpleNonNumericObject) {
        Document doc(fromjson("{ a : {} }"));
        Mod mod(fromjson("{ $bit : { a : { or : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareSimpleNonNumericArray) {

        Document doc(fromjson("{ a : [] }"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareSimpleNonNumericString) {
        Document doc(fromjson("{ a : '' }"));
        Mod mod(fromjson("{ $bit : { a : { or : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocumentAnd) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 0 }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : 0 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocumentOr) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { or : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentAnd) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { and : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 4 }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : 4 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentOr) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { or : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 7 }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : 7 } }"), logDoc);
    }

    TEST(InPlace, IntToIntAndIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(InPlace, IntToIntOrIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(InPlace, LongToLongAndIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<long long>(1))), logDoc);
    }

    TEST(InPlace, LongToLongOrIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<long long>(1))), logDoc);
    }

    TEST(InPlace, IntToLongAndIsNotInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);
    }

    TEST(InPlace, IntToLongOrIsNotInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);
    }

    TEST(NoOp, IntAnd) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(0xFFFFFFFFU)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0xABCD1234U))), logDoc);
    }

    TEST(NoOp, IntOr) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(0x0U)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0xABCD1234U))), logDoc);
    }

    TEST(NoOp, LongAnd) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" <<
                                                static_cast<long long>(0xFFFFFFFFFFFFFFFFULL)))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" <<
                                          static_cast<long long>(0xABCD1234EF981234ULL))), logDoc);
    }

    TEST(NoOp, LongOr) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(0x0ULL)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" <<
                                          static_cast<long long>(0xABCD1234EF981234ULL))), logDoc);
    }

    TEST(Upcasting, UpcastIntToLongAnd) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, UpcastIntToLongOr) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, LongsStayLongsAnd) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(2)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 0 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, LongsStayLongsOr) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(2)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : 3 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    // The following tests are re-created from the previous $bit tests in updatetests.cpp. They
    // are probably redundant with the tests above in various ways.

    TEST(DbUpdateTests, BitRewriteExistingField) {
        Document doc(BSON("a" << static_cast<int>(0)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(BSON("a" << static_cast<int>(1)), doc);
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(DbUpdateTests, BitRewriteNonExistingField) {
        Document doc(BSON("a" << static_cast<int>(0)));
        Mod mod(BSON("$bit" << BSON("b" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(BSON("a" << static_cast<int>(0) << "b" << static_cast<int>(1)), doc);
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(BSON("$set" << BSON("b" << static_cast<int>(1))), logDoc);
    }

    TEST(DbUpdateTests, Bit1_1) {
        Document doc(BSON("_id" << 1 << "x" << 3));
        Mod mod(BSON("$bit" << BSON("x" << BSON("and" << 2))));
        const BSONObj result(BSON("_id" << 1 << "x" << (3 & 2)));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        if (!execInfo.noOp)
            ASSERT_OK(mod.apply());

        ASSERT_EQUALS(result, doc);
    }

    TEST(DbUpdateTests, Bit1_2) {
        Document doc(BSON("_id" << 1 << "x" << 1));
        Mod mod(BSON("$bit" << BSON("x" << BSON("or" << 4))));
        const BSONObj result(BSON("_id" << 1 << "x" << (1 | 4)));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        if (!execInfo.noOp)
            ASSERT_OK(mod.apply());

        ASSERT_EQUALS(result, doc);
    }

    TEST(DbUpdateTests, Bit1_3) {
        Document doc(BSON("_id" << 1 << "x" << 3));
        Mod mod1(BSON("$bit" << BSON("x" << BSON("and" << 2))));
        Mod mod2(BSON("$bit" << BSON("x" << BSON("or" << 8))));
        const BSONObj result(BSON("_id" << 1 << "x" << ((3 & 2) | 8)));

        ModifierInterface::ExecInfo execInfo1;
        ASSERT_OK(mod1.prepare(doc.root(), "", &execInfo1));
        if (!execInfo1.noOp)
            ASSERT_OK(mod1.apply());

        ModifierInterface::ExecInfo execInfo2;
        ASSERT_OK(mod2.prepare(doc.root(), "", &execInfo2));
        if (!execInfo2.noOp)
            ASSERT_OK(mod2.apply());

        ASSERT_EQUALS(result, doc);
    }

    TEST(DbUpdateTests, Bit1_4) {
        Document doc(BSON("_id" << 1 << "x" << 3));
        Mod mod1(BSON("$bit" << BSON("x" << BSON("or" << 2))));
        Mod mod2(BSON("$bit" << BSON("x" << BSON("and" << 8))));
        const BSONObj result(BSON("_id" << 1 << "x" << ((3 | 2) & 8)));

        ModifierInterface::ExecInfo execInfo1;
        ASSERT_OK(mod1.prepare(doc.root(), "", &execInfo1));
        if (!execInfo1.noOp)
            ASSERT_OK(mod1.apply());

        ModifierInterface::ExecInfo execInfo2;
        ASSERT_OK(mod2.prepare(doc.root(), "", &execInfo2));
        if (!execInfo2.noOp)
            ASSERT_OK(mod2.apply());

        ASSERT_EQUALS(result, doc);
    }

} // namespace
