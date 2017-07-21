/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;

constexpr StringData FeatureCompatibilityVersion::kCollection;
constexpr StringData FeatureCompatibilityVersion::kCommandName;
constexpr StringData FeatureCompatibilityVersion::kDatabase;
constexpr StringData FeatureCompatibilityVersion::kParameterName;
constexpr StringData FeatureCompatibilityVersion::kVersionField;

namespace {
BSONObj makeUpdateCommand(StringData newVersion, BSONObj writeConcern) {
    BSONObjBuilder updateCmd;

    NamespaceString nss(FeatureCompatibilityVersion::kCollection);
    updateCmd.append("update", nss.coll());
    {
        BSONArrayBuilder updates(updateCmd.subarrayStart("updates"));
        {
            BSONObjBuilder updateSpec(updates.subobjStart());
            {
                BSONObjBuilder queryFilter(updateSpec.subobjStart("q"));
                queryFilter.append("_id", FeatureCompatibilityVersion::kParameterName);
            }
            {
                BSONObjBuilder updateMods(updateSpec.subobjStart("u"));
                updateMods.append(FeatureCompatibilityVersion::kVersionField, newVersion);
            }
            updateSpec.appendBool("upsert", true);
        }
    }

    if (!writeConcern.isEmpty()) {
        updateCmd.append(WriteConcernOptions::kWriteConcernField, writeConcern);
    }

    return updateCmd.obj();
}
}  // namespace

StatusWith<ServerGlobalParams::FeatureCompatibility::Version> FeatureCompatibilityVersion::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    bool foundVersionField = false;
    ServerGlobalParams::FeatureCompatibility::Version version;

    for (auto&& elem : featureCompatibilityVersionDoc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == FeatureCompatibilityVersion::kVersionField) {
            foundVersionField = true;
            if (elem.type() != BSONType::String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream()
                        << FeatureCompatibilityVersion::kVersionField
                        << " must be of type String, but was of type "
                        << typeName(elem.type())
                        << ". Contents of "
                        << FeatureCompatibilityVersion::kParameterName
                        << " document in "
                        << FeatureCompatibilityVersion::kCollection
                        << ": "
                        << featureCompatibilityVersionDoc
                        << ". See http://dochub.mongodb.org/core/3.6-feature-compatibility.");
            }
            if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion36) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k36;
            } else if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion34) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k34;
            } else {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "Invalid value for "
                        << FeatureCompatibilityVersion::kVersionField
                        << ", found "
                        << elem.String()
                        << ", expected '"
                        << FeatureCompatibilityVersionCommandParser::kVersion36
                        << "' or '"
                        << FeatureCompatibilityVersionCommandParser::kVersion34
                        << "'. Contents of "
                        << FeatureCompatibilityVersion::kParameterName
                        << " document in "
                        << FeatureCompatibilityVersion::kCollection
                        << ": "
                        << featureCompatibilityVersionDoc
                        << ". See http://dochub.mongodb.org/core/3.6-feature-compatibility.");
            }
        } else {
            return Status(
                ErrorCodes::BadValue,
                str::stream() << "Unrecognized field '" << elem.fieldName() << "''. Contents of "
                              << FeatureCompatibilityVersion::kParameterName
                              << " document in "
                              << FeatureCompatibilityVersion::kCollection
                              << ": "
                              << featureCompatibilityVersionDoc
                              << ". See http://dochub.mongodb.org/core/3.6-feature-compatibility.");
        }
    }

    if (!foundVersionField) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Missing required field '"
                          << FeatureCompatibilityVersion::kVersionField
                          << "''. Contents of "
                          << FeatureCompatibilityVersion::kParameterName
                          << " document in "
                          << FeatureCompatibilityVersion::kCollection
                          << ": "
                          << featureCompatibilityVersionDoc
                          << ". See http://dochub.mongodb.org/core/3.6-feature-compatibility.");
    }

    return version;
}

void FeatureCompatibilityVersion::set(OperationContext* opCtx, StringData version) {
    uassert(40284,
            "featureCompatibilityVersion must be '3.6' or '3.4'. See "
            "http://dochub.mongodb.org/core/3.6-feature-compatibility.",
            version == FeatureCompatibilityVersionCommandParser::kVersion36 ||
                version == FeatureCompatibilityVersionCommandParser::kVersion34);

    DBDirectClient client(opCtx);
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);

    // Update the featureCompatibilityVersion document stored in the "admin.system.version"
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.db().toString(),
                      makeUpdateCommand(version, WriteConcernOptions::Majority),
                      updateResult);
    uassertStatusOK(getStatusFromCommandResult(updateResult));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(updateResult));

    // Close all internal connections to versions lower than 3.6.
    if (version == FeatureCompatibilityVersionCommandParser::kVersion36) {
        opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
            transport::Session::kLatestVersionInternalClientKeepOpen |
            transport::Session::kExternalClientKeepOpen);
    }
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* opCtx,
                                                    repl::StorageInterface* storageInterface) {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        std::vector<std::string> dbNames;
        StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
        storageEngine->listDatabases(&dbNames);

        for (auto&& dbName : dbNames) {
            if (dbName != "local") {
                return;
            }
        }

        UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
        NamespaceString nss(FeatureCompatibilityVersion::kCollection);

        {
            AutoGetOrCreateDb autoDB(opCtx, nss.db(), MODE_X);

            // We reached this point because the only database that exists on the server is "local"
            // and we have just created an empty "admin" database. Therefore, it is safe to create
            // the "admin.system.version" collection.
            invariant(autoDB.justCreated());

            // We update the value of the isSchemaVersion36 server parameter so the
            // admin.system.version collection gets a UUID.
            serverGlobalParams.featureCompatibility.isSchemaVersion36.store(true);

            uassertStatusOK(storageInterface->createCollection(opCtx, nss, {}));
        }

        // We then insert the featureCompatibilityVersion document into the "admin.system.version"
        // collection. The server parameter will be updated on commit by the op observer.
        uassertStatusOK(storageInterface->insertDocument(
            opCtx,
            nss,
            BSON("_id" << FeatureCompatibilityVersion::kParameterName
                       << FeatureCompatibilityVersion::kVersionField
                       << FeatureCompatibilityVersionCommandParser::kVersion36)));
    }
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersion::parse(doc));
    log() << "setting featureCompatibilityVersion to " << toString(newVersion);
    opCtx->recoveryUnit()->onCommit([newVersion]() {
        serverGlobalParams.featureCompatibility.version.store(newVersion);
        serverGlobalParams.featureCompatibility.isSchemaVersion36.store(
            serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k36);
    });
}

void FeatureCompatibilityVersion::onDelete(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion34;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k34);
        serverGlobalParams.featureCompatibility.isSchemaVersion36.store(false);
    });
}

void FeatureCompatibilityVersion::onDropCollection(OperationContext* opCtx) {
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion34;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k34);
        serverGlobalParams.featureCompatibility.isSchemaVersion36.store(false);
    });
}

/**
 * Read-only server parameter for featureCompatibilityVersion.
 */
class FeatureCompatibilityVersionParameter : public ServerParameter {
public:
    FeatureCompatibilityVersionParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          FeatureCompatibilityVersion::kParameterName.toString(),
                          false,  // allowedToChangeAtStartup
                          false   // allowedToChangeAtRuntime
                          ) {}

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        b.append(name,
                 FeatureCompatibilityVersion::toString(
                     serverGlobalParams.featureCompatibility.version.load()));
    }

    virtual Status set(const BSONElement& newValueElement) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter. See "
                                       "http://dochub.mongodb.org/core/3.6-feature-compatibility.");
    }

    virtual Status setFromString(const std::string& str) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter. See "
                                       "http://dochub.mongodb.org/core/3.6-feature-compatibility.");
    }
} featureCompatibilityVersionParameter;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(internalValidateFeaturesAsMaster, bool, true);

}  // namespace mongo
