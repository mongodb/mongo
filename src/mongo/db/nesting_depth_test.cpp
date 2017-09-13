/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <exception>

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/json.h"
#include "mongo/client/connection_string.h"
#include "mongo/executor/network_interface_asio_integration_fixture.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace executor {
namespace {
class NestingDepthFixture : public NetworkInterfaceASIOIntegrationFixture {
public:
    void setUp() final {
        startNet();
    }
};

constexpr auto kCollectionName = "depthTest";

/**
 * Appends an object to 'builder' that is nested 'depth' levels deep.
 */
void appendNestedObject(BSONObjBuilder* builder, size_t depth) {
    if (depth == 1) {
        builder->append("a", 1);
    } else {
        BSONObjBuilder subobj(builder->subobjStart("a"));
        appendNestedObject(&subobj, depth - 1);
        subobj.doneFast();
    }
}

/**
 * Appends a command to 'builder' that inserts a document nested 'depth' levels deep. Calling
 * obj() on the builder returns the completed insert command as a BSONObj.
 */
void appendInsertCommandWithNestedDocument(BSONObjBuilder* builder, size_t depth) {
    builder->append("insert", kCollectionName);
    {
        BSONArrayBuilder array(builder->subarrayStart("documents"));
        {
            BSONObjBuilder document(array.subobjStart());
            appendNestedObject(&document, depth);
            document.doneFast();
        }
        array.doneFast();
    }
    builder->doneFast();
}

TEST_F(NestingDepthFixture, CanInsertLargeNestedDocumentAtOrUnderDepthLimit) {
    BSONObjBuilder insertDocumentOneLessThanLimit;
    appendInsertCommandWithNestedDocument(&insertDocumentOneLessThanLimit,
                                          BSONDepth::getMaxDepthForUserStorage() - 1);
    assertCommandOK(kCollectionName, insertDocumentOneLessThanLimit.obj());

    // Insert a document exactly at the BSON nesting limit.
    BSONObjBuilder insertCommandExactLimit;
    appendInsertCommandWithNestedDocument(&insertCommandExactLimit,
                                          BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, insertCommandExactLimit.obj());
}

TEST_F(NestingDepthFixture, CannotInsertLargeNestedDocumentExceedingDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, insertCmd.obj(), ErrorCodes::Overflow);
}

/**
 * Appends an array to 'builder' that is nested 'depth' levels deep.
 */
void appendNestedArray(BSONArrayBuilder* builder, size_t depth) {
    if (depth == 1) {
        builder->append(1);
    } else {
        BSONArrayBuilder subarr(builder->subarrayStart());
        appendNestedArray(&subarr, depth - 1);
        subarr.doneFast();
    }
}

/**
 * Appends a command to 'builder' that inserts a document with an array nested 'depth' levels deep.
 * Calling obj() on the builder returns the completed insert command as a BSONObj.
 */
void appendInsertCommandWithNestedArray(BSONObjBuilder* builder, size_t depth) {
    builder->append("insert", kCollectionName);
    {
        BSONArrayBuilder documentsBuilder(builder->subarrayStart("documents"));
        {
            BSONObjBuilder docBuilder(documentsBuilder.subobjStart());
            {
                BSONArrayBuilder arrayBuilder(docBuilder.subarrayStart("a"));
                appendNestedArray(&arrayBuilder, depth - 1);
                arrayBuilder.doneFast();
            }
            docBuilder.doneFast();
        }
        documentsBuilder.doneFast();
    }
    builder->doneFast();
}

TEST_F(NestingDepthFixture, CanInsertLargeNestedArrayAtOrUnderDepthLimit) {
    BSONObjBuilder insertDocumentOneLessThanLimit;
    appendInsertCommandWithNestedArray(&insertDocumentOneLessThanLimit,
                                       BSONDepth::getMaxDepthForUserStorage() - 1);
    assertCommandOK(kCollectionName, insertDocumentOneLessThanLimit.obj());

    // Insert a document exactly at the BSON nesting limit.
    BSONObjBuilder insertCommandExactLimit;
    appendInsertCommandWithNestedArray(&insertCommandExactLimit,
                                       BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, insertCommandExactLimit.obj());
}

TEST_F(NestingDepthFixture, CannotInsertLargeNestedArrayExceedingDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, insertCmd.obj(), ErrorCodes::Overflow);
}

/**
 * Creates a field name string that represents a document nested 'depth' levels deep.
 */
std::string getRepeatedFieldName(size_t depth) {
    ASSERT_GT(depth, 0U);

    StringBuilder builder;
    for (size_t i = 0U; i < depth - 1; i++) {
        builder << "a.";
    }
    builder << "a";
    return builder.str();
}

/**
 * Appends a command to 'builder' that updates a document with nesting depth 'originalNestingDepth'
 * to be 'newNestingDepth' levels deep. For example,
 *
 *      BSONObjBuilder b;
 *      appendUpdateCommandWithNestedDocuments(&b, 1, 2);
 *
 * appends an update to 'b' that updates { a: 1 } to be { a: { a: 1 } }.
 */
void appendUpdateCommandWithNestedDocuments(BSONObjBuilder* builder,
                                            size_t originalNestingDepth,
                                            size_t newNestingDepth) {
    ASSERT_GT(newNestingDepth, originalNestingDepth);

    auto originalFieldName = getRepeatedFieldName(originalNestingDepth);
    builder->append("update", kCollectionName);
    {
        BSONArrayBuilder updates(builder->subarrayStart("updates"));
        {
            BSONObjBuilder updateDoc(updates.subobjStart());
            {
                BSONObjBuilder query(updateDoc.subobjStart("q"));
                query.append(originalFieldName, 1);
                query.doneFast();
            }
            {
                BSONObjBuilder update(updateDoc.subobjStart("u"));
                BSONObjBuilder set(update.subobjStart("$set"));
                BSONObjBuilder field(set.subobjStart(originalFieldName));
                appendNestedObject(&field, newNestingDepth - originalNestingDepth);
                field.doneFast();
                set.doneFast();
                update.doneFast();
            }
            updateDoc.doneFast();
        }
    }
    builder->doneFast();
}

/**
 * Appends a command to 'builder' that replaces a document with nesting depth 'originalNestingDepth'
 * with a document 'newNestingDepth' levels deep. For example,
 *
 *      BSONObjBuilder b;
 *      appendUpdateCommandWithNestedDocuments(&b, 1, 2);
 *
 * appends an update to 'b' that replaces { a: 1 } with { a: { a: 1 } }.
 */
void appendUpdateReplaceCommandWithNestedDocuments(BSONObjBuilder* builder,
                                                   size_t originalNestingDepth,
                                                   size_t newNestingDepth) {
    ASSERT_GT(newNestingDepth, originalNestingDepth);

    auto originalFieldName = getRepeatedFieldName(originalNestingDepth);
    builder->append("update", kCollectionName);
    {
        BSONArrayBuilder updates(builder->subarrayStart("updates"));
        {
            BSONObjBuilder updateDoc(updates.subobjStart());
            {
                BSONObjBuilder query(updateDoc.subobjStart("q"));
                query.append(originalFieldName, 1);
                query.doneFast();
            }
            {
                BSONObjBuilder update(updateDoc.subobjStart("u"));
                appendNestedObject(&update, newNestingDepth);
                update.doneFast();
            }
            updateDoc.doneFast();
        }
    }
    builder->doneFast();
}

TEST_F(NestingDepthFixture, CanUpdateDocumentIfItStaysWithinDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, 3);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedDocuments(&updateCmd, 3, 5);
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CanUpdateDocumentToBeExactlyAtDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 2;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedDocuments(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CannotUpdateDocumentToExceedDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 3;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedDocuments(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, updateCmd.obj(), ErrorCodes::Overflow);
}

TEST_F(NestingDepthFixture, CanReplaceDocumentIfItStaysWithinDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, 3);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedDocuments(&updateCmd, 3, 5);
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CanReplaceDocumentToBeExactlyAtDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 2;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedDocuments(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CannotReplaceDocumentToExceedDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 3;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedDocument(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedDocuments(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, updateCmd.obj(), ErrorCodes::Overflow);
}

/**
 * Creates a field name string that represents an array nested 'depth' levels deep.
 */
std::string getRepeatedArrayPath(size_t depth) {
    ASSERT_GT(depth, 0U);

    StringBuilder builder;
    builder << "a";
    for (size_t i = 0U; i < depth - 1; i++) {
        builder << ".0";
    }
    return builder.str();
}

/**
 * Appends a command to 'builder' that updates a document with an array nested
 * 'originalNestingDepth' levels deep to be 'newNestingDepth' levels deep. For example,
 *
 *      BSONObjBuilder b;
 *      appendUpdateCommandWithNestedDocuments(&b, 3, 4);
 *
 * appends an update to 'b' that updates { a: [[1]] } to be { a: [[[1]]] }.
 */
void appendUpdateCommandWithNestedArrays(BSONObjBuilder* builder,
                                         size_t originalNestingDepth,
                                         size_t newNestingDepth) {
    ASSERT_GT(newNestingDepth, originalNestingDepth);

    auto originalFieldName = getRepeatedArrayPath(originalNestingDepth);
    builder->append("update", kCollectionName);
    {
        BSONArrayBuilder updates(builder->subarrayStart("updates"));
        {
            BSONObjBuilder updateDoc(updates.subobjStart());
            {
                BSONObjBuilder query(updateDoc.subobjStart("q"));
                query.append(originalFieldName, 1);
                query.doneFast();
            }
            {
                BSONObjBuilder update(updateDoc.subobjStart("u"));
                BSONObjBuilder set(update.subobjStart("$set"));
                BSONArrayBuilder field(set.subobjStart(originalFieldName));
                appendNestedArray(&field, newNestingDepth - originalNestingDepth);
                field.doneFast();
                set.doneFast();
                update.doneFast();
            }
            updateDoc.doneFast();
        }
    }
    builder->doneFast();
}

/**
 * Appends a command to 'builder' that replaces a document with an array nested
 * 'originalNestingDepth' levels deep with a document with an array nested 'newNestingDepth' levels
 * deep. For example,
 *
 *      BSONObjBuilder b;
 *      appendUpdateCommandWithNestedDocuments(&b, 3, 4);
 *
 * appends an update to 'b' that replaces { a: [[1]] } with { a: [[[1]]] }.
 */
void appendUpdateReplaceCommandWithNestedArrays(BSONObjBuilder* builder,
                                                size_t originalNestingDepth,
                                                size_t newNestingDepth) {
    ASSERT_GT(newNestingDepth, originalNestingDepth);

    auto originalFieldName = getRepeatedArrayPath(originalNestingDepth);
    builder->append("update", kCollectionName);
    {
        BSONArrayBuilder updates(builder->subarrayStart("updates"));
        {
            BSONObjBuilder updateDoc(updates.subobjStart());
            {
                BSONObjBuilder query(updateDoc.subobjStart("q"));
                query.append(originalFieldName, 1);
                query.doneFast();
            }
            {
                BSONObjBuilder update(updateDoc.subobjStart("u"));
                BSONArrayBuilder field(update.subobjStart(originalFieldName));
                appendNestedArray(&field, newNestingDepth - 1);
                field.doneFast();
                update.doneFast();
            }
            updateDoc.doneFast();
        }
    }
    builder->doneFast();
}

TEST_F(NestingDepthFixture, CanUpdateArrayIfItStaysWithinDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, 3);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedArrays(&updateCmd, 3, 5);
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CanUpdateArrayToBeExactlyAtDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 1;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedArrays(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CannotUpdateArrayToExceedDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 4;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateCommandWithNestedArrays(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, updateCmd.obj(), ErrorCodes::Overflow);
}

TEST_F(NestingDepthFixture, CanReplaceArrayIfItStaysWithinDepthLimit) {
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, 3);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedArrays(&updateCmd, 3, 5);
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CanReplaceArrayToBeExactlyAtDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 1;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedArrays(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage());
    assertCommandOK(kCollectionName, updateCmd.obj());
}

TEST_F(NestingDepthFixture, CannotReplaceArrayToExceedDepthLimit) {
    const auto largeButValidDepth = BSONDepth::getMaxDepthForUserStorage() - 4;
    BSONObjBuilder insertCmd;
    appendInsertCommandWithNestedArray(&insertCmd, largeButValidDepth);
    assertCommandOK(kCollectionName, insertCmd.obj());

    BSONObjBuilder updateCmd;
    appendUpdateReplaceCommandWithNestedArrays(
        &updateCmd, largeButValidDepth, BSONDepth::getMaxDepthForUserStorage() + 1);
    assertWriteError(kCollectionName, updateCmd.obj(), ErrorCodes::Overflow);
}
}  // namespace
}  // namespace executor
}  // namespace mongo
