/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/rpc/document_range.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(RequestBuilder, RoundTrip) {
    auto databaseName = "barbaz";
    auto commandName = "foobar";

    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();

    BSONObjBuilder commandArgsBob{};
    commandArgsBob.append(commandName, "baz");
    auto commandArgs = commandArgsBob.done();

    BSONObjBuilder inputDoc1Bob{};
    inputDoc1Bob.append("z", "t");
    auto inputDoc1 = inputDoc1Bob.done();

    BSONObjBuilder inputDoc2Bob{};
    inputDoc2Bob.append("h", "j");
    auto inputDoc2 = inputDoc2Bob.done();

    BSONObjBuilder inputDoc3Bob{};
    inputDoc3Bob.append("g", "p");
    auto inputDoc3 = inputDoc3Bob.done();

    BufBuilder inputDocs;
    inputDoc1.appendSelfToBufBuilder(inputDocs);
    inputDoc2.appendSelfToBufBuilder(inputDocs);
    inputDoc3.appendSelfToBufBuilder(inputDocs);

    rpc::DocumentRange inputDocRange{inputDocs.buf(), inputDocs.buf() + inputDocs.len()};

    rpc::CommandRequestBuilder r;

    auto msg = r.setDatabase(databaseName)
                   .setCommandName(commandName)
                   .setCommandArgs(commandArgs)
                   .setMetadata(metadata)
                   .addInputDocs(inputDocRange)
                   .done();

    rpc::CommandRequest parsed(&msg);

    ASSERT_EQUALS(parsed.getDatabase(), databaseName);
    ASSERT_EQUALS(parsed.getCommandName(), commandName);
    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_EQUALS(parsed.getCommandArgs(), commandArgs);
    // need ostream overloads for ASSERT_EQUALS
    ASSERT_TRUE(parsed.getInputDocs() == inputDocRange);
}

}  // namespace
