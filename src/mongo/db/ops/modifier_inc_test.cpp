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


#include "mongo/db/ops/modifier_inc.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::ModifierInc;
    using mongo::ModifierInterface;
    using mongo::NumberInt;
    using mongo::Status;
    using mongo::StringData;
    using mongo::fromjson;
    using mongo::mutablebson::checkDoc;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;
    using mongo::mutablebson::countChildren;

    /** Helper to build and manipulate a $inc mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj)
            : _modObj(modObj)
            , _mod() {
            ASSERT_OK(_mod.init(_modObj["$inc"].embeddedObject().firstElement()));
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

        ModifierInc& mod() { return _mod; }

    private:
        BSONObj _modObj;
        ModifierInc _mod;
    };

    TEST(Init, FailToInitWithInvalidValue) {
        BSONObj modObj;
        ModifierInc mod;

        // String is an invalid increment argument
        modObj = fromjson("{ $inc : { a : '' } }");
        ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement()));

        // Object is an invalid increment argument
        modObj = fromjson("{ $inc : { a : {} } }");
        ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement()));

        // Array is an invalid increment argument
        modObj = fromjson("{ $inc : { a : [] } }");
        ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement()));
    }

    TEST(Init, InitParsesNumberInt) {
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(1))));
    }

    TEST(Init, InitParsesNumberLong) {
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(1))));
    }

    TEST(Init, InitParsesNumberDouble) {
        Mod incMod(BSON("$inc" << BSON("a" << 1.0)));
    }

    TEST(SimpleMod, PrepareSimpleOK) {
        Document doc(fromjson("{ a : 1 }"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;

        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareSimpleNonNumericObject) {
        Document doc(fromjson("{ a : {} }"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareSimpleNonNumericArray) {

        Document doc(fromjson("{ a : [] }"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareSimpleNonNumericString) {
        Document doc(fromjson("{ a : '' }"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, ApplyAndLogEmptyDocument) {
        Document doc(fromjson("{}"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1 }")));

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 1 } }")));
    }

    TEST(SimpleMod, ApplyAndLogSimpleDocument) {
        Document doc(fromjson("{ a : 2 }"));
        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 3 }")));

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 3 } }")));
    }

    TEST(DottedMod, ApplyAndLogSimpleDocument) {
        Document doc(fromjson("{ a : { b : 2 } }"));
        Mod incMod(fromjson("{ $inc: { 'a.b' : 1 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : { b : 3 } }")));

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { 'a.b' : 3 } }")));
    }

    TEST(InPlace, IntToInt) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(1))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(InPlace, LongToLong) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(1))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(InPlace, DoubleToDouble) {
        Document doc(BSON("a" << 1.0));
        Mod incMod(BSON("$inc" << BSON("a" << 1.0 )));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(NoOp, Int) {
        Document doc(BSON("a" << static_cast<int>(1)));
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(0))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(NoOp, Long) {
        Document doc(BSON("a" << static_cast<long long>(1)));
        Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(0))));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(NoOp, Double) {
        Document doc(BSON("a" << 1.0));
        Mod incMod(BSON("$inc" << BSON("a" << 0.0)));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);
    }

    TEST(Upcasting, UpcastIntToLong) {
        // Checks that $inc : NumberLong(0) turns a NumberInt into a NumberLong and logs it
        // correctly.
        Document doc(BSON("a" << static_cast<int>(1)));
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(0))));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1 }")));
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 1 } }")));
        ASSERT_EQUALS(mongo::NumberLong, logDoc.root()["$set"]["a"].getType());
    }

    TEST(Upcasting, UpcastIntToDouble) {
        // Checks that $inc : 0.0 turns a NumberInt into a NumberDouble and logs it
        // correctly.
        Document doc(BSON("a" << static_cast<int>(1)));
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Mod incMod(fromjson("{ $inc : { a : 0.0 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1.0 }")));
        ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 1.0 } }")));
        ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
    }

    TEST(Upcasting, UpcastLongToDouble) {
        // Checks that $inc : 0.0 turns a NumberLong into a NumberDouble and logs it
        // correctly.
        Document doc(BSON("a" << static_cast<long long>(1)));
        ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());

        Mod incMod(fromjson("{ $inc : { a : 0.0 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 1.0 }")));
        ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 1.0 } }")));
        ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
    }

    TEST(Upcasting, DoublesStayDoubles) {
        // Checks that $inc : 0 doesn't change a NumberDouble away from double
        Document doc(fromjson("{ a : 1.0 }"));
        ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

        Mod incMod(fromjson("{ $inc : { a : 1 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{ a : 2.0 }")));
        ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

        Document logDoc;
        ASSERT_OK(incMod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $set : { a : 2.0 } }")));
        ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
    }

    // The only interesting overflow cases are int->long via increment: we never overflow to
    // double, and we never decrease precision on decrement.

    TEST(Spilling, OverflowIntToLong) {
        const int initial_value = std::numeric_limits<int32_t>::max();

        Document doc(BSON("a" << static_cast<int>(initial_value)));
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Mod incMod(fromjson("{ $inc : { a : 1 } }"));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        const long long target_value = static_cast<long long>(initial_value) + 1;

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, BSON("a" << target_value)));
    }

    TEST(Spilling, UnderflowIntToLong) {
        const int initial_value = std::numeric_limits<int32_t>::min();

        Document doc(BSON("a" << static_cast<int>(initial_value)));
        ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

        Mod incMod(fromjson("{ $inc : { a : -1 } }"));
        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        const long long target_value = static_cast<long long>(initial_value) - 1;

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc, BSON("a" << target_value)));
    }

    TEST(Lifecycle, IncModCanBeReused) {
        Document doc1(fromjson("{ a : 1 }"));
        Document doc2(fromjson("{ a : 1 }"));

        Mod incMod(fromjson("{ $inc: { a : 1 }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(incMod.prepare(doc1.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc1, fromjson("{ a : 2 }")));

        ASSERT_OK(incMod.prepare(doc2.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(incMod.apply());
        ASSERT_TRUE(checkDoc(doc2, fromjson("{ a : 2 }")));
    }

} // namespace
