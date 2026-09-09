// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/clean_shutdown_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/clean_shutdown_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync

namespace mongo {
namespace repl {
namespace {

constexpr auto kIdFieldName = "_id";
constexpr auto kParserName = "CleanShutdownDocument";

}  // namespace

BSONObj makeCleanShutdownBaselineFindCmd() {
    // The sync source's most recent clean shutdown, i.e. the highest _id in the collection:
    //     find system.cleanShutdownLog, sort {_id: -1}, limit 1
    return BSON("find" << NamespaceString::kCleanShutdownLogNamespace.coll() << "sort"
                       << BSON(kIdFieldName << -1) << "limit" << 1
                       << ReadConcernArgs::kReadConcernFieldName
                       << ReadConcernArgs::kLocal.toBSONInner());
}

BSONObj makeCleanShutdownCheckFindCmd(long long baseCleanShutdownId) {
    // The oldest clean shutdown the sync source recorded after our baseline, i.e. the lowest _id
    // above it:
    //     find system.cleanShutdownLog, filter {_id: {$gte: base + 1}}, sort {_id: 1}, limit 1
    //
    // Nothing matched means no shutdown happened; a match at exactly base + 1 is the first shutdown
    // and the one whose checkpoint timestamp decides the verdict; anything higher means base + 1
    // was truncated away.
    return BSON("find" << NamespaceString::kCleanShutdownLogNamespace.coll() << "filter"
                       << BSON(kIdFieldName << BSON("$gte" << baseCleanShutdownId + 1)) << "sort"
                       << BSON(kIdFieldName << 1) << "limit" << 1
                       << ReadConcernArgs::kReadConcernFieldName
                       << ReadConcernArgs::kLocal.toBSONInner());
}

StatusWith<long long> parseCleanShutdownBaseline(const std::vector<BSONObj>& docs) {
    if (docs.empty()) {
        return kNoCleanShutdownId;
    }

    try {
        return CleanShutdownDocument::parse(docs.front(), IDLParserContext(kParserName)).getId();
    } catch (const DBException&) {
        return exceptionToStatus();
    }
}

Status checkCleanShutdownResult(const boost::optional<BSONObj>& firstDocAfterBase,
                                long long baseCleanShutdownId,
                                Timestamp beginApplyingTimestamp,
                                const HostAndPort& syncSource) {
    if (!firstDocAfterBase) {
        // The sync source has not cleanly shut down since this attempt established its baseline.
        return Status::OK();
    }

    try {
        auto doc = CleanShutdownDocument::parse(*firstDocAfterBase, IDLParserContext(kParserName));

        if (doc.getId() > baseCleanShutdownId + 1) {
            // We cannot find the base + 1 document for some reason. Fail the attempt.
            return Status(
                ErrorCodes::InitialSyncFailure,
                str::stream()
                    << "Sync source " << syncSource
                    << " has truncated away the clean shutdown this initial sync attempt needed to "
                       "evaluate. Expected to find the shutdown with _id "
                    << baseCleanShutdownId + 1 << " but the oldest one still recorded has _id "
                    << doc.getId());
        }

        auto lastCheckpointTs = doc.getCleanShutdownLastCheckpointTimestamp();
        if (lastCheckpointTs >= beginApplyingTimestamp) {
            // The vulnerable window is empty. Everything before the checkpoint survived the
            // restart, and everything at or after beginApplyingTimestamp is covered by oplog
            // replay, so no write this attempt cloned can have been rolled back.
            return Status::OK();
        }

        return Status(ErrorCodes::InitialSyncFailure,
                      str::stream()
                          << "Sync source " << syncSource
                          << " cleanly shut down during initial sync and rolled back to a "
                             "checkpoint taken at "
                          << lastCheckpointTs.toString()
                          << ", which precedes this attempt's beginApplyingTimestamp of "
                          << beginApplyingTimestamp.toString()
                          << ". Writes in that window may have been lost from this node's view, so "
                             "the attempt cannot safely continue");
    } catch (const DBException&) {
        return exceptionToStatus();
    }
}

StatusWith<std::vector<BSONObj>> extractFirstBatch(const BSONObj& response) {
    auto cursorElem = response["cursor"];
    if (cursorElem.type() != BSONType::object) {
        return Status(ErrorCodes::CommandFailed,
                      str::stream() << "Find on the sync source's clean shutdown collection "
                                       "returned no cursor: "
                                    << response.toString());
    }

    auto batchElem = cursorElem.Obj()["firstBatch"];
    if (batchElem.type() != BSONType::array) {
        return Status(ErrorCodes::CommandFailed,
                      str::stream() << "Find on the sync source's clean shutdown collection "
                                       "returned a cursor with no first batch: "
                                    << response.toString());
    }

    std::vector<BSONObj> docs;
    for (auto&& doc : batchElem.Obj()) {
        if (doc.type() != BSONType::object) {
            return Status(ErrorCodes::CommandFailed,
                          str::stream() << "Sync source returned a non-document in its clean "
                                           "shutdown collection: "
                                        << response.toString());
        }
        docs.push_back(doc.Obj().getOwned());
    }
    return docs;
}

CleanShutdownChecker::CleanShutdownChecker(executor::TaskExecutor* executor, HostAndPort syncSource)
    : _executor(executor), _syncSource(std::move(syncSource)) {
    uassert(ErrorCodes::BadValue, "null task executor", executor);
}

CleanShutdownChecker::~CleanShutdownChecker() {}

StatusWith<CleanShutdownChecker::CallbackHandle> CleanShutdownChecker::reset(
    const CallbackFn& nextAction) {
    return _scheduleFind(
        makeCleanShutdownBaselineFindCmd(),
        [this, nextAction](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            if (!args.response.isOK()) {
                nextAction(args.response.status);
                return;
            }

            auto docs = extractFirstBatch(args.response.data);
            if (!docs.isOK()) {
                nextAction(docs.getStatus());
                return;
            }

            // Every node that records clean shutdowns seeds this collection with a sentinel on
            // startup, so a sync source that returns nothing at all does not have the collection
            // and is almost certainly running an older binary. Initial sync still proceeds - the
            // checks simply never match anything against such a source - but it does so without the
            // protection they exist to give, which is worth saying out loud.
            if (docs.getValue().empty()) {
                LOGV2_WARNING(
                    13224506,
                    "Sync source does not have a clean shutdown collection, so this initial sync "
                    "attempt cannot detect whether the sync source cleanly shuts down while it "
                    "runs. The sync source is most likely running an older binary. A clean restart "
                    "of the sync source during this attempt could roll back writes the attempt has "
                    "already cloned without initial sync noticing",
                    "syncSource"_attr = _syncSource,
                    logAttrs(NamespaceString::kCleanShutdownLogNamespace));
            }

            auto baseId = parseCleanShutdownBaseline(docs.getValue());
            if (!baseId.isOK()) {
                nextAction(baseId.getStatus());
                return;
            }

            {
                clang_checked::lock_guard lk(_mutex);
                _baseCleanShutdownId = baseId.getValue();
                // What an earlier baseline proved says nothing about this one.
                _verifiedSafe = false;
            }
            nextAction(Status::OK());
        });
}

StatusWith<CleanShutdownChecker::CallbackHandle> CleanShutdownChecker::checkForCleanShutdown(
    Timestamp beginApplyingTimestamp, const CallbackFn& nextAction) {
    {
        clang_checked::lock_guard lk(_mutex);
        if (_verifiedSafe) {
            // A check already saw a shutdown and found it safe, and a later shutdown cannot reopen
            // the window that one closed, so there is nothing a further query could tell us. Asking
            // again would only risk a spurious failure.
            return _executor->scheduleWork(
                [nextAction](const executor::TaskExecutor::CallbackArgs& args) {
                    nextAction(args.status);
                });
        }
    }

    auto baseId = getBaseCleanShutdownId();
    return _scheduleFind(makeCleanShutdownCheckFindCmd(baseId),
                         [this, baseId, beginApplyingTimestamp, nextAction](
                             const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                             if (!args.response.isOK()) {
                                 nextAction(args.response.status);
                                 return;
                             }

                             auto docs = extractFirstBatch(args.response.data);
                             if (!docs.isOK()) {
                                 nextAction(docs.getStatus());
                                 return;
                             }

                             boost::optional<BSONObj> firstDocAfterBase;
                             if (!docs.getValue().empty()) {
                                 firstDocAfterBase = docs.getValue().front();
                             }
                             auto result = checkCleanShutdownResult(
                                 firstDocAfterBase, baseId, beginApplyingTimestamp, _syncSource);
                             if (firstDocAfterBase && result.isOK()) {
                                 // A shutdown we actually saw and cleared. Remember it so later
                                 // checks need not ask again.
                                 clang_checked::lock_guard lk(_mutex);
                                 _verifiedSafe = true;
                             }
                             nextAction(result);
                         });
}

long long CleanShutdownChecker::getBaseCleanShutdownId() {
    clang_checked::lock_guard lk(_mutex);
    return _baseCleanShutdownId;
}

StatusWith<CleanShutdownChecker::CallbackHandle> CleanShutdownChecker::_scheduleFind(
    const BSONObj& findCmd, const RemoteCommandCallbackFn& nextAction) {
    executor::RemoteCommandRequest request(
        _syncSource, NamespaceString::kCleanShutdownLogNamespace.dbName(), findCmd, nullptr);
    return _executor->scheduleRemoteCommand(request, nextAction);
}

}  // namespace repl
}  // namespace mongo
