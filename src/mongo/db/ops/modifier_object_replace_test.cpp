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


#include "mongo/db/ops/modifier_object_replace.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::fromjson;
    using mongo::LogBuilder;
    using mongo::ModifierInterface;
    using mongo::NumberInt;
    using mongo::ModifierObjectReplace;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::countChildren;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $set mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj)
            : _mod() {
            _modObj = modObj;
            ASSERT_OK(_mod.init(BSON("" << modObj).firstElement(),
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

        ModifierObjectReplace& mod() { return _mod; }
        BSONObj& obj() { return _modObj; }

    private:
        ModifierObjectReplace _mod;
        BSONObj _modObj;
    };

    // Normal replacements below
    TEST(Normal, SingleFieldDoc){
        Document doc(fromjson("{_id:1, a:1}"));
        Mod mod(fromjson("{_id:1, b:12}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1, b:12}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(Normal, ComplexDoc){
        Document doc(fromjson("{_id:1, a:1}"));
        Mod mod(fromjson("{_id:1, b:[123], c: {r:true}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1, b:[123], c: {r:true}}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(Normal, OnlyIdField){
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{_id:1}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, mod.obj());

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    // These updates have to do with updates without an _id field
    // (the existing _id isn't removed)
    TEST(IdLeft, EmptyDocReplacement){
        Document doc(fromjson("{_id:1}"));
        Mod mod(fromjson("{}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(IdLeft, EmptyDoc){
        Document doc(fromjson("{_id:1}"));
        Mod mod(fromjson("{}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(IdLeft, SingleFieldAddition){
        Document doc(fromjson("{_id:1}"));
        Mod mod(fromjson("{a:1}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1, a:1}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(IdLeft, SingleFieldReplaced){
        Document doc(fromjson("{a: []}"));
        Mod mod(fromjson("{a:10}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, mod.obj());

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(mod.obj(), logDoc);
    }

    TEST(IdLeft, SwapFields){
        Document doc(fromjson("{_id:1, a:1}"));
        Mod mod(fromjson("{b:1}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(doc, fromjson("{_id:1, b:1}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{b:1}"), logDoc);
    }

    TEST(IdImmutable, ReplaceIdNumber){
        Document doc(fromjson("{_id:1, a:1}"));
        Mod mod(fromjson("{_id:2}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_NOT_OK(mod.apply());
    }

    TEST(IdImmutable, ReplaceEmbeddedId){
        Document doc(fromjson("{_id:{a:1, b:2}, a:1}"));
        Mod mod(fromjson("{_id:{b:2, a:1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_NOT_OK(mod.apply());
    }

    // Test for bad paths
    TEST(ValidatePath, FailDottedField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{'a.a':10}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, FailEmbeddedDottedField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{a:{'a.a':10}}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, FailDollarPrefixField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{$a:10}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, FailEmbeddedDollarPrefixField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{a:{$foo:1}}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, FailArrayIdField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{_id:[9]}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
        input = fromjson("{blah:1, _id:[9]}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, FailRegexIdField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{_id: /a/}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
        input = fromjson("{foo:1, _id: /a/}");
        ASSERT_NOT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    // These are similar to ones above but are allowed for embedded doc/elements
    TEST(ValidatePath, EmbeddedArrayIdField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{a: {_id:[10]}}");
        ASSERT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
        input = fromjson("{a: {blah:1, _id:[10]}}");
        ASSERT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

    TEST(ValidatePath, HasEmbeddedRegexIdField){
        ModifierObjectReplace mod;
        BSONObj input = fromjson("{a: {_id: /a/, hi:1}}");
        ASSERT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
        input = fromjson("{a: {hi:1, _id: /a/}}");
        ASSERT_OK(mod.init(BSON("" << input).firstElement(),
                               ModifierInterface::Options::normal()));
    }

} // unnamed namespace
