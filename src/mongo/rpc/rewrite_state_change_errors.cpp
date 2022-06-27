/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/rpc/rewrite_state_change_errors.h"

#include "mongo/platform/basic.h"

#include <array>
#include <string>

#include <boost/optional.hpp>
#include <fmt/format.h>

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/rewrite_state_change_errors_server_parameter_gen.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/static_immortal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::rpc {
namespace {

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
boost::optional<std::string> scrubErrmsg(StringData val) {
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
        StringData sep;
        auto out = std::back_inserter(pat);
        for (const auto& scrub : *scrubs) {
            out = format_to(out, FMT_STRING("{}({})"), sep, scrub.pat.pattern());
            sep = "|"_sd;
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
    if (auto errmsg = node["errmsg"]; errmsg.ok() && errmsg.isType(String))
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
    if (const auto& we = doc["writeErrors"]; we.type() == Array) {
        size_t idx = 0;
        BSONObj bArr = we.Obj();
        for (auto ai = bArr.begin(); ai != bArr.end(); ++ai, ++idx)
            if (ai->type() == Object && (oldCode = needsRewrite(sc, ai->Obj())))
                editErrorNode(lazyMutableRoot()["writeErrors"][idx]);
    }

    // `writeConcernError` is a single error-bearing node.
    if (const auto& wce = doc["writeConcernError"]; wce.type() == Object) {
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
    if (!isMongos() || (sc && !getEnabled(sc)) || !getEnabled(opCtx))
        return {};
    return rewriteDocument(doc, opCtx);
}

}  // namespace mongo::rpc
