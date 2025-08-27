/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/pipeline/document_source_out.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {


REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(out, DocumentSourceOut::id)

StageConstraints DocumentSourceOut::constraints(PipelineSplitState pipeState) const {
    StageConstraints result{StreamType::kStreaming,
                            PositionRequirement::kLast,
                            HostTypeRequirement::kNone,
                            DiskUseRequirement::kWritesPersistentData,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed};
    if (pipeState == PipelineSplitState::kSplitForMerge) {
        // If output collection resides on a single shard, we should route $out to it to perform
        // local writes. Note that this decision is inherently racy and subject to become stale.
        // This is okay because either choice will work correctly, we are simply applying a
        // heuristic optimization.
        result.mergeShardId = getMergeShardId();
    }
    return result;
}

DocumentSourceOutSpec DocumentSourceOut::parseOutSpecAndResolveTargetNamespace(
    const BSONElement& spec, const DatabaseName& defaultDB) {
    DocumentSourceOutSpec outSpec;
    if (spec.type() == BSONType::string) {
        outSpec.setColl(spec.valueStringData());
        // TODO SERVER-77000: access a SerializationContext object to serialize properly
        outSpec.setDb(defaultDB.serializeWithoutTenantPrefix_UNSAFE());
    } else if (spec.type() == BSONType::object) {
        // TODO SERVER-77000: access a SerializationContext object to pass into the IDLParserContext
        outSpec = mongo::DocumentSourceOutSpec::parse(spec.embeddedObject(),
                                                      IDLParserContext(kStageName));
    } else {
        uassert(16990,
                fmt::format("{} only supports a string or object argument, but found {}",
                            kStageName,
                            typeName(spec.type())),
                spec.type() == BSONType::string);
    }

    return outSpec;
}

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    auto outSpec = parseOutSpecAndResolveTargetNamespace(spec, nss.dbName());
    NamespaceString targetNss = NamespaceStringUtil::deserialize(nss.dbName().tenantId(),
                                                                 outSpec.getDb(),
                                                                 outSpec.getColl(),
                                                                 outSpec.getSerializationContext());

    uassert(
        ErrorCodes::InvalidNamespace,
        fmt::format("Invalid {} target namespace, {}", kStageName, targetNss.toStringForErrorMsg()),
        targetNss.isValid());
    return std::make_unique<DocumentSourceOut::LiteParsed>(spec.fieldName(), std::move(targetNss));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::create(
    NamespaceString outputNs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<TimeseriesOptions> timeseries) {
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            fmt::format("{} cannot be used in a transaction", kStageName),
            !expCtx->getOperationContext()->inMultiDocumentTransaction());

    uassert(
        ErrorCodes::InvalidNamespace,
        fmt::format("Invalid {} target namespace, {}", kStageName, outputNs.toStringForErrorMsg()),
        outputNs.isValid());

    uassert(17385,
            fmt::format("Can't {} to special collection: {}", kStageName, outputNs.coll()),
            !outputNs.isSystem());

    uassert(31321,
            fmt::format("Can't {} to internal database: {}",
                        kStageName,
                        outputNs.dbName().toStringForErrorMsg()),
            !outputNs.isOnInternalDb());
    return new DocumentSourceOut(std::move(outputNs), std::move(timeseries), expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto outSpec =
        parseOutSpecAndResolveTargetNamespace(elem, expCtx->getNamespaceString().dbName());
    NamespaceString targetNss =
        NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName().tenantId(),
                                         outSpec.getDb(),
                                         outSpec.getColl(),
                                         outSpec.getSerializationContext());
    return create(std::move(targetNss), expCtx, std::move(outSpec.getTimeseries()));
}

Value DocumentSourceOut::serialize(const SerializationOptions& opts) const {
    BSONObjBuilder bob;
    DocumentSourceOutSpec spec;
    // TODO SERVER-77000: use SerializatonContext from expCtx and DatabaseNameUtil to serialize
    // spec.setDb(DatabaseNameUtil::serialize(
    //     getOutputNs().dbName(),
    //     SerializationContext::stateCommandReply(getExpCtx()->getSerializationContext())));
    spec.setDb(getOutputNs().dbName().serializeWithoutTenantPrefix_UNSAFE());
    spec.setColl(getOutputNs().coll());

    spec.setTimeseries(_timeseries ? boost::optional<mongo::TimeseriesOptions>(*_timeseries)
                                   : boost::none);
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}


}  // namespace mongo
