/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/pipeline/document_source_current_op.h"

#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

namespace {
const StringData kAllUsersFieldName = "allUsers"_sd;
const StringData kIdleConnectionsFieldName = "idleConnections"_sd;
const StringData kTruncateOpsFieldName = "truncateOps"_sd;

const StringData kOpIdFieldName = "opid"_sd;
const StringData kClientFieldName = "client"_sd;
const StringData kMongosClientFieldName = "client_s"_sd;
const StringData kShardFieldName = "shard"_sd;
}  // namespace

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(currentOp,
                         DocumentSourceCurrentOp::LiteParsed::parse,
                         DocumentSourceCurrentOp::createFromBson);

std::unique_ptr<DocumentSourceCurrentOp::LiteParsed> DocumentSourceCurrentOp::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    // Need to check the value of allUsers; if true then inprog privilege is required.
    if (spec.type() != BSONType::Object) {
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "$currentOp options must be specified in an object, but found: "
                                << typeName(spec.type()));
    }

    bool allUsers = false;

    // Check the spec for all fields named 'allUsers'. If any of them are 'true', we require
    // the 'inprog' privilege. This avoids the possibility that a spec with multiple
    // allUsers fields might allow an unauthorized user to view all operations.
    for (auto&& elem : spec.embeddedObject()) {
        if (elem.fieldNameStringData() == "allUsers"_sd) {
            if (elem.type() != BSONType::Bool) {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "The 'allUsers' parameter of the $currentOp stage "
                                           "must be a boolean value, but found: "
                                        << typeName(elem.type()));
            }

            allUsers = allUsers || elem.boolean();
        }
    }

    return stdx::make_unique<DocumentSourceCurrentOp::LiteParsed>(allUsers);
}


const char* DocumentSourceCurrentOp::getSourceName() const {
    return "$currentOp";
}

DocumentSource::InitialSourceType DocumentSourceCurrentOp::getInitialSourceType() const {
    return InitialSourceType::kCollectionlessInitialSource;
}

DocumentSource::GetNextResult DocumentSourceCurrentOp::getNext() {
    if (_ops.empty()) {
        _ops =
            _mongod->getCurrentOps(_includeIdleConnections, _includeOpsFromAllUsers, _truncateOps);

        _opsIter = _ops.begin();

        if (pExpCtx->inShard) {
            _shardName = _mongod->getShardName(pExpCtx->opCtx);

            uassert(40465,
                    "Aggregation request specified 'fromRouter' but unable to retrieve shard name "
                    "for $currentOp pipeline stage.",
                    !_shardName.empty());
        }
    }

    if (_opsIter != _ops.end()) {
        if (!pExpCtx->inShard) {
            return Document(*_opsIter++);
        }

        // This $currentOp is running in a sharded context.
        invariant(!_shardName.empty());

        const BSONObj& op = *_opsIter++;
        MutableDocument doc;

        // Add the shard name to the output document.
        doc.addField(kShardFieldName, Value(_shardName));

        // For operations on a shard, we change the opid from the raw numeric form to
        // 'shardname:opid'. We also change the fieldname 'client' to 'client_s' to indicate
        // that the IP is that of the mongos which initiated this request.
        for (auto&& elt : op) {
            StringData fieldName = elt.fieldNameStringData();

            if (fieldName == kOpIdFieldName) {
                uassert(ErrorCodes::TypeMismatch,
                        str::stream() << "expected numeric opid for $currentOp response from '"
                                      << _shardName
                                      << "' but got: "
                                      << typeName(elt.type()),
                        elt.isNumber());

                std::string shardOpID = (str::stream() << _shardName << ":" << elt.numberInt());
                doc.addField(kOpIdFieldName, Value(shardOpID));
            } else if (fieldName == kClientFieldName) {
                doc.addField(kMongosClientFieldName, Value(elt.str()));
            } else {
                doc.addField(fieldName, Value(elt));
            }
        }

        return doc.freeze();
    }

    return GetNextResult::makeEOF();
}

intrusive_ptr<DocumentSource> DocumentSourceCurrentOp::createFromBson(
    BSONElement spec, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$currentOp options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    const NamespaceString& nss = pExpCtx->ns;

    uassert(ErrorCodes::InvalidNamespace,
            "$currentOp must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == NamespaceString::kAdminDb && nss.isCollectionlessAggregateNS());

    ConnMode includeIdleConnections = ConnMode::kExcludeIdle;
    UserMode includeOpsFromAllUsers = UserMode::kExcludeOthers;
    TruncationMode truncateOps = TruncationMode::kNoTruncation;

    for (auto&& elem : spec.embeddedObject()) {
        const auto fieldName = elem.fieldNameStringData();

        if (fieldName == kIdleConnectionsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'idleConnections' parameter of the $currentOp stage must "
                                     "be a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            includeIdleConnections =
                (elem.Bool() ? ConnMode::kIncludeIdle : ConnMode::kExcludeIdle);
        } else if (fieldName == kAllUsersFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'allUsers' parameter of the $currentOp stage must be a "
                                     "boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            includeOpsFromAllUsers =
                (elem.Bool() ? UserMode::kIncludeAll : UserMode::kExcludeOthers);
        } else if (fieldName == kTruncateOpsFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "The 'truncateOps' parameter of the $currentOp stage must be "
                                     "a boolean value, but found: "
                                  << typeName(elem.type()),
                    elem.type() == BSONType::Bool);
            truncateOps =
                (elem.Bool() ? TruncationMode::kTruncateOps : TruncationMode::kNoTruncation);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Unrecognized option '" << fieldName
                                    << "' in $currentOp stage.");
        }
    }

    return intrusive_ptr<DocumentSourceCurrentOp>(new DocumentSourceCurrentOp(
        pExpCtx, includeIdleConnections, includeOpsFromAllUsers, truncateOps));
}

intrusive_ptr<DocumentSourceCurrentOp> DocumentSourceCurrentOp::create(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    ConnMode includeIdleConnections,
    UserMode includeOpsFromAllUsers,
    TruncationMode truncateOps) {
    return intrusive_ptr<DocumentSourceCurrentOp>(new DocumentSourceCurrentOp(
        pExpCtx, includeIdleConnections, includeOpsFromAllUsers, truncateOps));
}

Value DocumentSourceCurrentOp::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{
        {getSourceName(),
         Document{{kIdleConnectionsFieldName, (_includeIdleConnections == ConnMode::kIncludeIdle)},
                  {kAllUsersFieldName, (_includeOpsFromAllUsers == UserMode::kIncludeAll)},
                  {kTruncateOpsFieldName, (_truncateOps == TruncationMode::kTruncateOps)}}}});
}
}  // namespace mongo
