// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain_common.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/version_context.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"

#include <string_view>

namespace mongo::explain_common {
namespace append_if_room {
/*
 * This constant restricts the maximum output object size for 'appendIfRoom' functions. By setting
 * it slightly lower than 'BSONObjMaxUserSize', we ensure the final output stays within limits even
 * after adding mandatory small fields that are not individually size-checked.
 *
 * Assumption: BSONObjMaxUserSize will never be below 10KB.
 */
constexpr int OutputObjectMaxSize = BSONObjMaxUserSize - 10 * 1024;

void appendWarningMessage(std::string_view fieldName, BSONObjBuilder* out) {
    constexpr int kWarningMsgOverhead = 60;
    // The reserved buffer size for the warning message if 'out' exceeds the max BSON user size.
    const int warningMsgSize = fieldName.size() + kWarningMsgOverhead;

    // Unless 'out' has already exceeded the max BSON user size, add a warning indicating
    // that data has been truncated.
    if (out->len() < OutputObjectMaxSize - warningMsgSize) {
        out->append("warning",
                    str::stream() << "'" << fieldName << "'"
                                  << " has been omitted due to BSON size limit");
    }
}
}  // namespace append_if_room

void generateServerInfo(BSONObjBuilder* out) {
    BSONObjBuilder serverBob(out->subobjStart("serverInfo"));
    serverBob.append("host", getHostNameCached());
    serverBob.appendNumber("port", serverGlobalParams.port);
    auto&& vii = VersionInfoInterface::instance();
    serverBob.append("version", vii.version());
    serverBob.append("gitVersion", vii.gitVersion());
    serverBob.doneFast();
}

void generateServerParameters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              BSONObjBuilder* out) {
    BSONObjBuilder serverBob(out->subobjStart("serverParameters"));
    serverBob.appendNumber("internalQueryFacetMaxOutputDocSizeBytes",
                           internalQueryFacetMaxOutputDocSizeBytes.load());
    serverBob.appendNumber("internalLookupStageIntermediateDocumentMaxSizeBytes",
                           internalLookupStageIntermediateDocumentMaxSizeBytes.load());
    serverBob.appendNumber("internalQueryProhibitBlockingMergeOnMongoS",
                           internalQueryProhibitBlockingMergeOnMongoS.load());
    auto queryControl = expCtx->getQueryKnobConfiguration().getInternalQueryFrameworkControlForOp();
    serverBob.append("internalQueryFrameworkControl", idl::serialize(queryControl));
    serverBob.appendNumber("internalQueryPlannerIgnoreIndexWithCollationForRegex",
                           internalQueryPlannerIgnoreIndexWithCollationForRegex.load());
    serverBob.doneFast();
}

void generateQueryKnobs(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        BSONObjBuilder* out) {
    auto* opCtx = expCtx->getOperationContext();
    if (!feature_flags::gFeatureFlagPqsQueryKnobs.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }
    auto serializedKnobs = expCtx->getQueryKnobConfiguration().serializeForExplain();
    if (!serializedKnobs.isEmpty()) {
        appendIfRoom(serializedKnobs, "queryKnobs", out);
    }
}

void generateQueryShapeHash(OperationContext* opCtx, BSONObjBuilder* out) {
    if (auto&& queryShapeHash = mongo::CurOp::get(opCtx)->debug().getQueryShapeHash()) {
        out->append("queryShapeHash", queryShapeHash->toHexString());
    }
}

void generatePeakTrackedMemBytes(const OperationContext* opCtx, BSONObjBuilder* out) {
    if (int64_t peakTrackedMemBytes = mongo::CurOp::get(opCtx)->getPeakTrackedMemoryBytes()) {
        out->append("peakTrackedMemBytes", peakTrackedMemBytes);
    }
}

bool appendIfRoom(const BSONObj& toAppend, std::string_view fieldName, BSONObjBuilder* out) {
    if ((out->len() + toAppend.objsize()) < append_if_room::OutputObjectMaxSize) {
        out->append(fieldName, toAppend);
        return true;
    }

    append_if_room::appendWarningMessage(fieldName, out);

    return false;
}

bool appendIfRoom(const BSONArray& toAppend, std::string_view fieldName, BSONObjBuilder* out) {
    if ((out->len() + toAppend.objsize()) < append_if_room::OutputObjectMaxSize) {
        out->appendArray(fieldName, toAppend);
        return true;
    }

    append_if_room::appendWarningMessage(fieldName, out);

    return false;
}
}  // namespace mongo::explain_common
