// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/rpc/rewrite_state_change_errors.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/rewrite_state_change_errors_server_parameter_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/pcre.h"
#include "mongo/util/static_immortal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::rpc {
namespace {
using namespace std::literals::string_view_literals;

struct RewriteEnabled {
    bool enabled = rewriteStateChangeErrors;
};

/** Enable for the entire service. */
auto enabledForService = ServiceContext::declareDecoration<RewriteEnabled>();

/** Enable for a single operation. */
auto enabledForOperation = OperationContext::declareDecoration<RewriteEnabled>();


/**
 * We must replace key phrases in `errmsg` with sensible strings that
 * drivers will ignore. Return scrubbed `val`, or no value if it
 * doesn't need scrubbing.
 *
 * See
 * https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#not-master-and-node-is-recovering
 */
boost::optional<std::string> scrubErrmsg(std::string_view val) {
    struct Scrub {
        Scrub(std::string pat, std::string sub) : pat(std::move(pat)), sub(std::move(sub)) {}
        pcre::Regex pat;
        std::string sub;
    };
    static const StaticImmortal scrubs = std::array{
        Scrub{"not master", "(NOT_PRIMARY)"},
        Scrub{"node is recovering", "(NODE_IS_RECOVERING)"},
    };
    // Fast scan for the common case that no key phrase is present.
    static const StaticImmortal fastScan = [] {
        std::string pat;
        std::string_view sep;
        auto out = std::back_inserter(pat);
        for (const auto& scrub : *scrubs) {
            out = fmt::format_to(out, "{}({})", sep, scrub.pat.pattern());
            sep = "|"sv;
        }
        return pcre::Regex(pat);
    }();

    if (fastScan->matchView(val)) {
        std::string s{val};
        bool didSub = false;
        for (auto&& scrub : *scrubs) {
            bool subOk = scrub.pat.substitute(scrub.sub, &s, pcre::SUBSTITUTE_GLOBAL);
            didSub = (didSub || subOk);
        }
        if (didSub)
            return s;
    }
    return {};
}

/**
 * Change the {code, codeName, errmsg} fields to cloak a proxied state change
 * error. We choose `HostUnreachable` as it is retryable but doesn't induce an
 * SDAM state change.
 */
void editErrorNode(mutablebson::Element&& node) {
    if (auto codeElement = node["code"]; codeElement.ok()) {
        static constexpr auto newCode = ErrorCodes::HostUnreachable;
        uassertStatusOK(codeElement.setValueInt(newCode));
        // If there's a corresponding `codeName`, replace it to match.
        if (auto codeName = node["codeName"]; codeName.ok())
            uassertStatusOK(codeName.setValueString(ErrorCodes::errorString(newCode)));
    }
    if (auto errmsg = node["errmsg"]; errmsg.ok() && errmsg.isType(BSONType::string))
        if (auto scrubbed = scrubErrmsg(errmsg.getValueString()))
            uassertStatusOK(errmsg.setValueString(*scrubbed));
}

/**
 * If `node` contains a numeric "code" field that indicates a state change,
 * returns the value of that code field as an `ErrorCodes::Error`. Otherwise
 * returns a disengaged optional.
 *
 * There are two categories of state change errors: ShutdownError and
 * NotPrimaryError.
 */
boost::optional<ErrorCodes::Error> needsRewrite(ServiceContext* sc, const BSONObj& node) {
    int32_t intCode{};
    if (!node["code"].coerce(&intCode))
        return {};
    ErrorCodes::Error ec{intCode};

    if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(ec)) {
        return ec;
    }

    // ShutdownError codes are correct if this server is also in shutdown. If
    // this server is shutting down, then even if the shutdown error didn't
    // originate from this server, it might as well have.
    if (ErrorCodes::isA<ErrorCategory::ShutdownError>(ec) && !sc->getKillAllOperations()) {
        return ec;
    }

    return {};
}

/**
 * Returns a copy of doc with errors rewritten to cloak state change errors if
 * necessary. Returns disengaged optional if no changes were necessary.
 */
boost::optional<BSONObj> rewriteDocument(const BSONObj& doc, OperationContext* opCtx) {
    boost::optional<mutablebson::Document> mutableDoc;
    auto lazyMutableRoot = [&] {
        if (!mutableDoc)
            mutableDoc.emplace(doc);
        return mutableDoc->root();
    };

    ServiceContext* sc = opCtx->getServiceContext();

    // Skip unless there's an "ok" element value equivalent to expected 0 or 1.
    // "ok" is conventionally a NumberDouble, but coerce since this is unspecified.
    double okValue = 0;
    if (!doc.getField("ok").coerce(&okValue) || (okValue != 0 && okValue != 1))
        return {};  // Skip: missing or unusable "ok" field.

    boost::optional<ErrorCodes::Error> oldCode;

    // The root of the doc is an error-bearing node if not ok.
    if (okValue == 0 && (oldCode = needsRewrite(sc, doc)))
        editErrorNode(lazyMutableRoot());

    // The `writeErrors` and `writeConcernError` nodes might need editing.
    // `writeErrors` is an array of error-bearing nodes like the doc root.
    if (const auto& we = doc["writeErrors"]; we.type() == BSONType::array) {
        size_t idx = 0;
        BSONObj bArr = we.Obj();
        for (auto ai = bArr.begin(); ai != bArr.end(); ++ai, ++idx)
            if (ai->type() == BSONType::object && (oldCode = needsRewrite(sc, ai->Obj())))
                editErrorNode(lazyMutableRoot()["writeErrors"][idx]);
    }

    // `writeConcernError` is a single error-bearing node.
    if (const auto& wce = doc["writeConcernError"]; wce.type() == BSONType::object) {
        if ((oldCode = needsRewrite(sc, wce.Obj())))
            editErrorNode(lazyMutableRoot()["writeConcernError"]);
    }

    if (mutableDoc) {
        LOGV2_DEBUG(5054900, 1, "Rewrote state change error", "code"_attr = oldCode);
        return mutableDoc->getObject();
    }
    return {};
}

}  // namespace


bool RewriteStateChangeErrors::getEnabled(OperationContext* opCtx) {
    return enabledForOperation(opCtx).enabled;
}

void RewriteStateChangeErrors::setEnabled(OperationContext* opCtx, bool e) {
    enabledForOperation(opCtx).enabled = e;
}

bool RewriteStateChangeErrors::getEnabled(ServiceContext* sc) {
    return enabledForService(sc).enabled;
}

void RewriteStateChangeErrors::setEnabled(ServiceContext* sc, bool e) {
    enabledForService(sc).enabled = e;
}

void RewriteStateChangeErrors::onActiveFailCommand(OperationContext* opCtx, const BSONObj& data) {
    bool b;
    if (!bsonExtractBooleanField(data, "allowRewriteStateChange", &b).isOK() || !b)
        setEnabled(opCtx, false);
}

boost::optional<BSONObj> RewriteStateChangeErrors::rewrite(BSONObj doc, OperationContext* opCtx) {
    auto sc = opCtx->getServiceContext();
    if (!serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) ||
        (sc && !getEnabled(sc)) || !getEnabled(opCtx))
        return {};
    return rewriteDocument(doc, opCtx);
}

}  // namespace mongo::rpc
