/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/commit_quorum_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/str.h"

namespace mongo {

const StringData CommitQuorumOptions::kCommitQuorumField = "commitQuorum"_sd;
const char CommitQuorumOptions::kMajority[] = "majority";
const char CommitQuorumOptions::kAll[] = "all";

const BSONObj CommitQuorumOptions::Majority(BSON(kCommitQuorumField
                                                 << CommitQuorumOptions::kMajority));
const BSONObj CommitQuorumOptions::all(BSON(kCommitQuorumField << CommitQuorumOptions::kAll));

CommitQuorumOptions::CommitQuorumOptions(int numNodesOpts) {
    reset();
    numNodes = numNodesOpts;
    invariant(numNodes >= 0);
}

CommitQuorumOptions::CommitQuorumOptions(const std::string& modeOpts) {
    reset();
    mode = modeOpts;
    invariant(!mode.empty());
}

Status CommitQuorumOptions::parse(const BSONElement& commitQuorumElement) {
    reset();

    if (commitQuorumElement.isNumber()) {
        auto cNumNodes = commitQuorumElement.safeNumberLong();
        if (cNumNodes < 0 ||
            cNumNodes > static_cast<decltype(cNumNodes)>(repl::ReplSetConfig::kMaxMembers)) {
            return Status(
                ErrorCodes::FailedToParse,
                str::stream()
                    << "commitQuorum has to be a non-negative number and not greater than "
                    << repl::ReplSetConfig::kMaxMembers);
        }
        numNodes = static_cast<decltype(numNodes)>(cNumNodes);
    } else if (commitQuorumElement.type() == String) {
        mode = commitQuorumElement.valuestrsafe();
    } else {
        return Status(ErrorCodes::FailedToParse, "commitQuorum has to be a number or a string");
    }

    return Status::OK();
}

CommitQuorumOptions CommitQuorumOptions::deserializerForIDL(
    const BSONElement& commitQuorumElement) {
    CommitQuorumOptions commitQuorumOptions;
    uassertStatusOK(commitQuorumOptions.parse(commitQuorumElement));
    return commitQuorumOptions;
}

BSONObj CommitQuorumOptions::toBSON() const {
    BSONObjBuilder builder;
    appendToBuilder(kCommitQuorumField, &builder);
    return builder.obj();
}

void CommitQuorumOptions::appendToBuilder(StringData fieldName, BSONObjBuilder* builder) const {
    if (mode.empty()) {
        builder->append(fieldName, numNodes);
    } else {
        builder->append(fieldName, mode);
    }
}

}  // namespace mongo
