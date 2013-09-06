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

#include "mongo/db/commands/write_commands/batch.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST(InsertItemParsing, Normal) {
        // Tests that the document {foo: "bar"} is valid to be inserted.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_INSERT,
                                 fromjson("{foo:\"bar\"}"));
        ASSERT(w1.isValid());
    }

    TEST(UpdateItemParsing, Normal) {
        WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:{}, multi:true, upsert:true}"));
        ASSERT(w1.isValid());
    }

    TEST(UpdateItemParsing, MissingOptionalFields) {
        // Omitting fields "multi" and "upsert" is acceptable.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:{}, multi:true}"));
        ASSERT(w1.isValid());

        WriteBatch::WriteItem w2(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:{}, upsert:true}"));
        ASSERT(w2.isValid());
    }

    TEST(UpdateItemParsing, MissingRequiredFields) {
        // Omitting fields "q" or "u" is not acceptable.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}}"));
        ASSERT_FALSE(w1.isValid());

        WriteBatch::WriteItem w2(WriteBatch::WRITE_UPDATE,
                                 fromjson("{u:{}}"));
        ASSERT_FALSE(w2.isValid());
    }

    TEST(UpdateItemParsing, InvalidTypes) {
        // Fields "q" and "u" must be subdocuments; fields "multi" and "upsert" must be bools.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:1}"));
        ASSERT_FALSE(w1.isValid());

        WriteBatch::WriteItem w2(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:[]}"));
        ASSERT_FALSE(w2.isValid());

        WriteBatch::WriteItem w3(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:1, u:{}}"));
        ASSERT_FALSE(w3.isValid());

        WriteBatch::WriteItem w4(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:[], u:{}}"));
        ASSERT_FALSE(w4.isValid());

        WriteBatch::WriteItem w5(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:{}, multi:1}"));
        ASSERT_FALSE(w5.isValid());

        WriteBatch::WriteItem w6(WriteBatch::WRITE_UPDATE,
                                 fromjson("{q:{}, u:{}, upsert:1}"));
        ASSERT_FALSE(w6.isValid());
    }

    //
    // TODO: Reject ambiguous/invalid input.
    //

    // TEST(UpdateItemParsing, ExtraFields) {
    //     WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
    //                              fromjson("{q:{}, u:{}, extraField:1}"));
    //     ASSERT_FALSE(w1.isValid());
    // }

    // TEST(UpdateItemParsing, RepeatedFields) {
    //     WriteBatch::WriteItem w1(WriteBatch::WRITE_UPDATE,
    //                              fromjson("{q:{}, u:{}, q:1}"));
    //     ASSERT_FALSE(w1.isValid());
    //
    //     WriteBatch::WriteItem w2(WriteBatch::WRITE_UPDATE,
    //                              fromjson("{q:{}, u:{}, multi:true, multi:1}"));
    //     ASSERT_FALSE(w2.isValid());
    // }

    TEST(DeleteItemParsing, Normal) {
        // Field "q" will accept a subdocument.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_DELETE,
                                 fromjson("{q:{}}"));
        ASSERT(w1.isValid());
    }

    TEST(DeleteItemParsing, MissingRequiredFields) {
        // Omitting required field "q" is not acceptable.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_DELETE,
                                 fromjson("{}"));
        ASSERT_FALSE(w1.isValid());
    }

    TEST(DeleteItemParsing, InvalidTypes) {
        // Field "q" will accept a subdocument.

        WriteBatch::WriteItem w1(WriteBatch::WRITE_DELETE,
                                 fromjson("{q:1}"));
        ASSERT_FALSE(w1.isValid());
    }

    //
    // TODO: Reject ambiguous/invalid input.
    //

    // TEST(DeleteItemParsing, ExtraFields) {
    //     WriteBatch::WriteItem w1(WriteBatch::WRITE_DELETE,
    //                              fromjson("{q:{}, extraField:1}"));
    //     ASSERT_FALSE(w1.isValid());
    // }

    // TEST(DeleteItemParsing, RepeatedFields) {
    //     WriteBatch::WriteItem w1(WriteBatch::WRITE_DELETE,
    //                              fromjson("{q:{}, q:1}"));
    //     ASSERT_FALSE(w1.isValid());
    // }

    TEST(BatchParsing, Normal) {
        string errMsg;

        WriteBatch wb1("test.c", WriteBatch::WRITE_INSERT);
        ASSERT(wb1.parse(fromjson("{insert:\"c\", documents:[{}]}"), &errMsg));

        WriteBatch wb2("test.c", WriteBatch::WRITE_UPDATE);
        ASSERT(wb2.parse(fromjson("{update:\"c\", updates:[{q:{}, u:{}}]}"), &errMsg));

        WriteBatch wb3("test.c", WriteBatch::WRITE_DELETE);
        ASSERT(wb3.parse(fromjson("{delete:\"c\", deletes:[{q:{}}]}"), &errMsg));
    }

    TEST(BatchParsing, MissingRequiredFields) {
        // Omitting the batch's "container field" (e.g. "documents") is not acceptable.

        string errMsg;

        WriteBatch wb1("test.c", WriteBatch::WRITE_INSERT);
        ASSERT_FALSE(wb1.parse(fromjson("{insert:\"c\"}"), &errMsg));

        WriteBatch wb2("test.c", WriteBatch::WRITE_UPDATE);
        ASSERT_FALSE(wb2.parse(fromjson("{update:\"c\"}"), &errMsg));

        WriteBatch wb3("test.c", WriteBatch::WRITE_DELETE);
        ASSERT_FALSE(wb3.parse(fromjson("{delete:\"c\"}"), &errMsg));
    }

    TEST(BatchParsing, WithOptionalFields) {
        string errMsg;

        WriteBatch wb1("test.c", WriteBatch::WRITE_INSERT);
        ASSERT(wb1.parse(fromjson("{insert:\"c\","
                                  " documents:[{}],"
                                  " continueOnError:false,"
                                  " writeConcern:{}}"),
                         &errMsg));
    }

    TEST(BatchParsing, InvalidTypes) {
        // The batch container field accepts a list of subdocuments.
        // Field "continueOnErrror" accepts a bool.
        // Field "writeConcern" accepts a subdocument.

        string errMsg;

        WriteBatch wb1("test.c", WriteBatch::WRITE_INSERT);
        ASSERT_FALSE(wb1.parse(fromjson("{insert:\"c\","
                                        " documents:{}}"),
                               &errMsg));

        WriteBatch wb2("test.c", WriteBatch::WRITE_INSERT);
        ASSERT_FALSE(wb2.parse(fromjson("{insert:\"c\","
                                        " documents:[4]}"),
                               &errMsg));

        WriteBatch wb3("test.c", WriteBatch::WRITE_INSERT);
        ASSERT_FALSE(wb3.parse(fromjson("{insert:\"c\","
                                        " documents:[{}],"
                                        " continueOnError:1}"),
                               &errMsg));

        WriteBatch wb4("test.c", WriteBatch::WRITE_INSERT);
        ASSERT_FALSE(wb4.parse(fromjson("{insert:\"c\","
                                        " documents:[{}],"
                                        " writeConcern:1}"),
                               &errMsg));
    }

}
