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
    using mongo::mutablebson::checkDoc;
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
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 0 }")));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 0 } }")));
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocumentOr) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $bit : { a : { or : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1 }")));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 1 } }")));
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentAnd) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { and : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 4 }")));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 4 } }")));
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocumentOr) {
        Document doc(fromjson("{ a : 5 }"));
        Mod mod(fromjson("{ $bit : { a : { or : 6 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 7 }")));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 7 } }")));
    }

    TEST(InPlace, IntToIntAndIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(InPlace, IntToIntOrIsInPlace) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(InPlace, LongToLongAndIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(InPlace, LongToLongOrIsInPlace) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
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
    }

    TEST(NoOp, IntOr) {
        Document doc(BSON("a" << static_cast<int>(0xABCD1234U)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<int>(0x0U)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(NoOp, LongAnd) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" <<
                                                static_cast<long long>(0xFFFFFFFFFFFFFFFFULL)))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(NoOp, LongOr) {
        Document doc(BSON("a" << static_cast<long long>(0xABCD1234EF981234ULL)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("or" << static_cast<long long>(0x0ULL)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(Upcasting, UpcastIntToLongAnd) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod mod(BSON("$bit" << BSON("a" << BSON("and" << static_cast<long long>(1)))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1 }")));
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
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1 }")));
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
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 0 }")));
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
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 3 }")));
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    }

} // namespace
