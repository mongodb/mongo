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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/ops/modifier_bit.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::LogBuilder;
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
            ASSERT_OK(_mod.init(_modObj["$bit"].embeddedObject().firstElement(),
                                ModifierInterface::Options::normal()));

        }

        Status prepare(Element root,
                       const StringData& matchedField,
                       ModifierInterface::ExecInfo* execInfo) {
            return _mod.prepare(root, matchedField, execInfo);
        }

        Status apply() const {
            return _mod.apply();
        }

        Status log(LogBuilder* logBuilder) const {
            return _mod.log(logBuilder);
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
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));

        // Array is an invalid $bit argument
        modObj = fromjson("{ $bit : { a : [] } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));

        // An object with value not in ('and', 'or') is an invalid $bit argument
        modObj = fromjson("{ $bit : { a : { foo : 4 } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));

        // The argument to the sub-operator must be numeric
        modObj = fromjson("{ $bit : { a : { or : [] } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));

        modObj = fromjson("{ $bit : { a : { or : 'foo' } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));

        // The argument to the sub-operator must be integral
        modObj = fromjson("{ $bit : { a : { or : 1.0 } } }");
        ASSERT_NOT_OK(mod.init(modObj["$bit"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(Init, ParsesAndInt) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1)))));
    }

    TEST(Init, ParsesOrInt) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));
    }

    TEST(Init, ParsesXorInt) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<int>(1)))));
    }

    TEST(Init, ParsesAndLong) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));
    }

    TEST(Init, ParsesOrLong) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));
    }

    TEST(Init, ParsesXorLong) {
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<long long>(1)))));
    }

    TEST(SimpleMod, PrepareOKTargetNotFound) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;

        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareOKTargetFound) {
        Document doc(fromjson("{ a : 1 }"));
        Mod mod(fromjson("{ $bit : { a : { and : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;

        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
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
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 0 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 0 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocumentOr) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { or : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocumentXor) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { xor : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentAnd) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { and : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 4 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 4 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentOr) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { or : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 7 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 7 } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentXor) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { xor : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 3 }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : 3 } }"), logDoc);
    }

    TEST(InPlace, IntToIntAndIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(InPlace, IntToIntOrIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(InPlace, IntToIntXorIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0))), logDoc);
    }

    TEST(InPlace, LongToLongAndIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<long long>(1))), logDoc);
    }

    TEST(InPlace, LongToLongOrIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<long long>(1))), logDoc);
    }

    TEST(InPlace, LongToLongXorIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<long long>(0))), logDoc);
    }

    TEST(InPlace, IntToLongAndIsNotInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(InPlace, IntToLongOrIsNotInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(InPlace, IntToLongXorIsNotInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(NoOp, IntAnd) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(0xFFFFFFFFU)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0xABCD1234U))), logDoc);
    }

    TEST(NoOp, IntOr) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(0x0U)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0xABCD1234U))), logDoc);
    }

    TEST(NoOp, IntXor) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<int>(0x0U)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(0xABCD1234U))), logDoc);
    }

    TEST(NoOp, LongAnd) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" <<
                                                static_cast<long long>(0xFFFFFFFFFFFFFFFFULL)))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" <<
                                          static_cast<long long>(0xABCD1234EF981234ULL))), logDoc);
    }

    TEST(NoOp, LongOr) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(0x0ULL)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" <<
                                          static_cast<long long>(0xABCD1234EF981234ULL))), logDoc);
    }

    TEST(NoOp, LongXor) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<long long>(0x0ULL)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" <<
                                          static_cast<long long>(0xABCD1234EF981234ULL))), logDoc);
    }

    TEST(Upcasting, UpcastIntToLongAnd) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, UpcastIntToLongOr) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, UpcastIntToLongXor) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<long long>(0)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, LongsStayLongsAnd) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(2)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 0 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, LongsStayLongsOr) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(2)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 3 }"), doc);
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

    TEST(Upcasting, LongsStayLongsXor) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("xor" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : 0 }"), doc);
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

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(BSON("a" << static_cast<int>(1)), doc);
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("a" << static_cast<int>(1))), logDoc);
    }

    TEST(DbUpdateTests, BitRewriteNonExistingField) {
        Document doc(BSON("a" << static_cast<int>(0)));
        Mod mod(BSON("$bit" << BSON("b" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(BSON("a" << static_cast<int>(0) << "b" << static_cast<int>(1)), doc);
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
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

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("x" << (3 & 2))), logDoc);
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

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("x" << (1 | 4))), logDoc);
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

    TEST(DbUpdateTests, Bit1_3_Combined) {
        Document doc(BSON("_id" << 1 << "x" << 3));
        Mod mod(BSON("$bit" << BSON("x" << BSON("and" << 2 << "or" << 8))));
        const BSONObj result(BSON("_id" << 1 << "x" << ((3 & 2) | 8)));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        if (!execInfo.noOp)
            ASSERT_OK(mod.apply());

        ASSERT_EQUALS(result, doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("x" << ((3 & 2) | 8))), logDoc);
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

    TEST(DbUpdateTests, Bit1_4_Combined) {
        Document doc(BSON("_id" << 1 << "x" << 3));
        Mod mod(BSON("$bit" << BSON("x" << BSON("or" << 2 << "and" << 8))));
        const BSONObj result(BSON("_id" << 1 << "x" << ((3 | 2) & 8)));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        if (!execInfo.noOp)
            ASSERT_OK(mod.apply());

        ASSERT_EQUALS(result, doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(BSON("$set" << BSON("x" << ((3 | 2) & 8))), logDoc);
    }

} // namespace
