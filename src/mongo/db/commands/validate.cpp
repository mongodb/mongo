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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;

// Sets the 'valid' result field to false and returns immediately.
MONGO_FAIL_POINT_DEFINE(validateCmdCollectionNotValid);

namespace {

// Protects the state below.
stdx::mutex _validationMutex;

// Holds the set of full `databaseName.collectionName` namespace strings in progress. Validation
// commands register themselves in this data structure so that subsequent commands on the same
// namespace will wait rather than run in parallel.
std::set<std::string> _validationsInProgress;

// This is waited upon if there is found to already be a validation command running on the targeted
// namespace, as _validationsInProgress would indicate. This is signaled when a validation command
// finishes on any namespace.
stdx::condition_variable _validationNotifier;

}  // namespace

/**
 * Example validate command:
 *   {
 *       validate: "collectionNameWithoutTheDBPart",
 *       full: <bool>  // If true, a more thorough (and slower) collection validation is performed.
 *   }
 */
class ValidateCmd : public BasicCommand {
public:
    ValidateCmd() : BasicCommand("validate") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Validate contents of a namespace by scanning its data structures for correctness.  "
               "Slow.\n"
               "Add full:true option to do a more thorough check";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool allowsAfterClusterTime(const BSONObj& cmd) const override {
        return false;
    }

    bool canIgnorePrepareConflicts() const override {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::validate);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        if (MONGO_FAIL_POINT(validateCmdCollectionNotValid)) {
            result.appendBool("valid", false);
            return true;
        }

        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));

        const bool full = cmdObj["full"].trueValue();

        ValidateCmdLevel level = kValidateNormal;
        if (full) {
            level = kValidateFull;
        }

        // TODO (SERVER-30357): Add support for background validation.
        const bool background = false;

        // Background validation requires the storage engine to support checkpoints because it
        // performs the validation on a checkpoint using checkpoint cursors.
        if (background && !opCtx->getServiceContext()->getStorageEngine()->supportsCheckpoints()) {
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "Running validate on collection " << nss
                                    << " with { background: true } is not supported on the "
                                    << storageGlobalParams.engine << " storage engine");
        }

        if (!serverGlobalParams.quiet.load()) {
            LOG(0) << "CMD: validate " << nss.ns();
        }

        AutoGetDb autoDB(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);

        Collection* collection =
            autoDB.getDb() ? autoDB.getDb()->getCollection(opCtx, nss) : nullptr;
        if (!collection) {
            if (autoDB.getDb() && ViewCatalog::get(autoDB.getDb())->lookup(opCtx, nss.ns())) {
                uasserted(ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view");
            }

            uasserted(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        result.append("ns", nss.ns());

        // Only one validation per collection can be in progress, the rest wait.
        {
            stdx::unique_lock<stdx::mutex> lock(_validationMutex);
            try {
                while (_validationsInProgress.find(nss.ns()) != _validationsInProgress.end()) {
                    opCtx->waitForConditionOrInterrupt(_validationNotifier, lock);
                }
            } catch (AssertionException& e) {
                CommandHelpers::appendCommandStatusNoThrow(
                    result,
                    {ErrorCodes::CommandFailed,
                     str::stream() << "Exception thrown during validation: " << e.toString()});
                return false;
            }

            _validationsInProgress.insert(nss.ns());
        }

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard<stdx::mutex> lock(_validationMutex);
            _validationsInProgress.erase(nss.ns());
            _validationNotifier.notify_all();
        });

        ValidateResults results;
        Status status =
            CollectionValidation::validate(opCtx, collection, level, background, &results, &result);
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        }

        CollectionOptions opts =
            DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, collection->ns());

        // All collections must have a UUID.
        if (!opts.uuid) {
            results.errors.push_back(str::stream() << "UUID missing on collection " << nss.ns());
            results.valid = false;
        }

        if (!full) {
            results.warnings.push_back(
                "Some checks omitted for speed. use {full:true} option to do more thorough scan.");
        }

        result.appendBool("valid", results.valid);
        result.append("warnings", results.warnings);
        result.append("errors", results.errors);
        result.append("extraIndexEntries", results.extraIndexEntries);
        result.append("missingIndexEntries", results.missingIndexEntries);

        if (!results.valid) {
            result.append("advice",
                          "A corrupt namespace has been detected. See "
                          "http://dochub.mongodb.org/core/data-recovery for recovery steps.");
        }

        return true;
    }

} validateCmd;
}  // namespace mongo
