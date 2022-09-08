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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_coll_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/time_support.h"

using boost::intrusive_ptr;

namespace mongo {

REGISTER_DOCUMENT_SOURCE(collStats,
                         DocumentSourceCollStats::LiteParsed::parse,
                         DocumentSourceCollStats::createFromBson,
                         AllowedWithApiStrict::kConditionally);

void DocumentSourceCollStats::LiteParsed::assertPermittedInAPIVersion(
    const APIParameters& apiParameters) const {
    if (apiParameters.getAPIVersion() && *apiParameters.getAPIVersion() == "1" &&
        apiParameters.getAPIStrict().value_or(false)) {
        uassert(ErrorCodes::APIStrictError,
                "only the 'count' option to $collStats is supported in API Version 1",
                !_spec.getLatencyStats() && !_spec.getQueryExecStats() && !_spec.getStorageStats());
    }
}

const char* DocumentSourceCollStats::getSourceName() const {
    return kStageName.rawData();
}

intrusive_ptr<DocumentSource> DocumentSourceCollStats::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40166,
            str::stream() << "$collStats must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    auto spec =
        DocumentSourceCollStatsSpec::parse(IDLParserContext(kStageName), specElem.embeddedObject());

    return make_intrusive<DocumentSourceCollStats>(pExpCtx, std::move(spec));
}

BSONObj DocumentSourceCollStats::makeStatsForNs(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const DocumentSourceCollStatsSpec& spec) {
    BSONObjBuilder builder;

    builder.append("ns", nss.ns());

    auto shardName = expCtx->mongoProcessInterface->getShardName(expCtx->opCtx);

    if (!shardName.empty()) {
        builder.append("shard", shardName);
    }

    builder.append("host", getHostNameCachedAndPort());
    builder.appendDate("localTime", jsTime());

    if (auto latencyStatsSpec = spec.getLatencyStats()) {
        expCtx->mongoProcessInterface->appendLatencyStats(
            expCtx->opCtx, nss, latencyStatsSpec->getHistograms(), &builder);
    }

    if (auto storageStats = spec.getStorageStats()) {
        // If the storageStats field exists, it must have been validated as an object when parsing.
        BSONObjBuilder storageBuilder(builder.subobjStart("storageStats"));
        uassertStatusOKWithContext(expCtx->mongoProcessInterface->appendStorageStats(
                                       expCtx->opCtx, nss, *storageStats, &storageBuilder),
                                   "Unable to retrieve storageStats in $collStats stage");
        storageBuilder.doneFast();
    }

    if (spec.getCount()) {
        uassertStatusOKWithContext(
            expCtx->mongoProcessInterface->appendRecordCount(expCtx->opCtx, nss, &builder),
            "Unable to retrieve count in $collStats stage");
    }

    if (spec.getQueryExecStats()) {
        uassertStatusOKWithContext(
            expCtx->mongoProcessInterface->appendQueryExecStats(expCtx->opCtx, nss, &builder),
            "Unable to retrieve queryExecStats in $collStats stage");
    }
    return builder.obj();
}

DocumentSource::GetNextResult DocumentSourceCollStats::doGetNext() {
    if (_finished) {
        return GetNextResult::makeEOF();
    }

    _finished = true;

    return {Document(makeStatsForNs(pExpCtx, pExpCtx->ns, _collStatsSpec))};
}

Value DocumentSourceCollStats::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{getSourceName(), _collStatsSpec.toBSON()}});
}

}  // namespace mongo
