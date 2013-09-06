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


#include "mongo/db/ops/modifier_pull.h"

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
    using mongo::ModifierPull;
    using mongo::ModifierInterface;
    using mongo::Status;
    using mongo::StringData;
    using mongo::fromjson;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $pull mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj)
            : _modObj(modObj)
            , _mod() {
            ASSERT_OK(_mod.init(_modObj["$pull"].embeddedObject().firstElement(),
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

        ModifierPull& mod() {
            return _mod;
        }

    private:
        BSONObj _modObj;
        ModifierPull _mod;
    };

    TEST(SimpleMod, PrepareOKTargetNotFound) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $unset : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, PrepareOKTargetFound) {
        Document doc(fromjson("{ a : [ 0, 1, 2, 3 ] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareInvalidTargetString) {
        Document doc(fromjson("{ a : 'foo' }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareInvalidTargetObject) {
        Document doc(fromjson("{ a : { 'foo' : 'bar' } }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
    }

    TEST(SimpleMod, PrepareAndLogEmptyDocument) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $unset : { a : 1 } }"), logDoc);
    }

    TEST(SimpleMod, PrepareAndLogMissingElementAfterFoundPath) {
        Document doc(fromjson("{ a : { b : { c : {} } } }"));
        Mod mod(fromjson("{ $pull : { 'a.b.c.d' : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b.c.d");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $unset : { 'a.b.c.d' : 1 } }"), logDoc);
    }

    TEST(SimpleMod, PrepareAndLogEmptyArray) {
        Document doc(fromjson("{ a : [] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
    }

    TEST(SimpleMod, PullMatchesNone) {
        Document doc(fromjson("{ a : [2, 3, 4, 5] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : [2, 3, 4, 5] } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogPullMatchesOne) {
        Document doc(fromjson("{ a : [0, 1, 2, 3, 4, 5] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : [ 1, 2, 3, 4, 5 ] }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : [1, 2, 3, 4, 5] } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogPullMatchesSeveral) {
        Document doc(fromjson("{ a : [0, 1, 0, 2, 0, 3, 0, 4, 0, 5] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : [ 1, 2, 3, 4, 5 ] }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : [1, 2, 3, 4, 5] } }"), logDoc);
    }

    TEST(SimpleMod, ApplyAndLogPullMatchesAll) {
        Document doc(fromjson("{ a : [0, -1, -2, -3, -4, -5] }"));
        Mod mod(fromjson("{ $pull : { a : { $lt : 1 } } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
    }

    TEST(ComplexMod, ApplyAndLogComplexDocAndMatching1) {

        const char* const strings[] = {
            // Document:
            "{ a : { b : [ { x : 1 }, { y : 'y' }, { x : 2 }, { z : 'z' } ] } }",

            // Modifier:
            "{ $pull : { 'a.b' : { $or : [ "
            "  { 'y' : { $exists : true } }, "
            "  { 'z' : { $exists : true } } "
            "] } } }",

            // Document result:
            "{ a : { b : [ { x : 1 }, { x : 2 } ] } }",

            // Log result:
            "{ $set : { 'a.b' : [ { x : 1 }, { x : 2 } ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(ComplexMod, ApplyAndLogComplexDocAndMatching2) {

        const char* const strings[] = {
            // Document:
            "{ a : { b : [ { x : 1 }, { y : 'y' }, { x : 2 }, { z : 'z' } ] } }",

            // Modifier:
            "{ $pull : { 'a.b' : { 'y' : { $exists : true } } } }",

            // Document result:
            "{ a : { b : [ { x : 1 }, { x : 2 }, { z : 'z' } ] } }",

            // Log result:
            "{ $set : { 'a.b' : [ { x : 1 }, { x : 2 }, { z : 'z' } ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(ComplexMod, ApplyAndLogComplexDocAndMatching3) {

        const char* const strings[] = {
            // Document:
            "{ a : { b : [ { x : 1 }, { y : 'y' }, { x : 2 }, { z : 'z' } ] } }",

            // Modifier:
            "{ $pull : { 'a.b' : { $in : [ { x : 1 }, { y : 'y' } ] } } }",

            // Document result:
            "{ a : { b : [ { x : 2 }, { z : 'z' } ] } }",

            // Log result:
            "{ $set : { 'a.b' : [ { x : 2 }, { z : 'z' } ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(ValueMod, ApplyAndLogScalarValueMod) {

        const char* const strings[] = {
            // Document:
            "{ a : [1, 2, 1, 2, 1, 2] }",

            // Modifier:
            "{ $pull : { a : 1 } }",

            // Document result:
            "{ a : [ 2, 2, 2] }",

            // Log result:
            "{ $set : { a : [ 2, 2, 2 ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(ValueMod, ApplyAndLogObjectValueMod) {

        const char* const strings[] = {
            // Document:
            "{ a : [ { x : 1 }, { y : 2 }, { x : 1 }, { y : 2 } ] }",

            // Modifier:
            "{ $pull : { a : { y : 2 } } }",

            // Document result:
            "{ a : [ { x : 1 }, { x : 1 }] }",

            // Log result:
            "{ $set : { a : [ { x : 1 }, { x : 1 } ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(DocumentationTests, Example1) {
        const char* const strings[] = {
            // Document:
            "{ flags: ['vme', 'de', 'pse', 'tsc', 'msr', 'pae', 'mce' ] }",

            // Modifier:
            "{ $pull: { flags: 'msr' } }",

            // Document result:
            "{ flags: ['vme', 'de', 'pse', 'tsc', 'pae', 'mce' ] }",

            // Log result:
            "{ $set : { flags: ['vme', 'de', 'pse', 'tsc', 'pae', 'mce' ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "flags");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(DocumentationTests, Example2a) {
        const char* const strings[] = {
            // Document:
            "{ votes: [ 3, 5, 6, 7, 7, 8 ] }",

            // Modifier:
            "{ $pull: { votes: 7 } }",

            // Document result:
            "{ votes: [ 3, 5, 6, 8 ] }",

            // Log result:
            "{ $set : { votes: [ 3, 5, 6, 8 ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "votes");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(DocumentationTests, Example2b) {
        const char* const strings[] = {
            // Document:
            "{ votes: [ 3, 5, 6, 7, 7, 8 ] }",

            // Modifier:
            "{ $pull: { votes: { $gt: 6 } } }",

            // Document result:
            "{ votes: [ 3, 5, 6 ] }",

            // Log result:
            "{ $set : { votes: [ 3, 5, 6 ] } }"
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "votes");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

    TEST(MatchingEdgeCases, NonObjectShortCircuit) {
        const char* const strings[] = {
            "{ a: [ { x: 1 }, 2 ] }",

            "{ $pull: { a: { x: 1 } } }",

            "{ a: [ 2 ] }",

            "{ $set : { a: [ 2 ] } }",
        };

        Document doc(fromjson(strings[0]));
        Mod mod(fromjson(strings[1]));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(strings[2]), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson(strings[3]), logDoc);
    }

} // namespace
