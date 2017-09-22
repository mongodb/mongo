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
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
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
constexpr StringData FeatureCompatibilityVersion::kTargetVersionField;

StatusWith<FeatureCompatibilityVersionInfo> FeatureCompatibilityVersion::parse(
    const BSONObj& featureCompatibilityVersionInfo) {
    FeatureCompatibilityVersionInfo versionInfo;

    for (auto&& elem : featureCompatibilityVersionInfo) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == FeatureCompatibilityVersion::kVersionField ||
                   fieldName == FeatureCompatibilityVersion::kTargetVersionField) {
            if (elem.type() != BSONType::String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << fieldName
                                            << " must be of type String, but was of type "
                                            << typeName(elem.type())
                                            << ". Contents of "
                                            << FeatureCompatibilityVersion::kParameterName
                                            << " document in "
                                            << FeatureCompatibilityVersion::kCollection
                                            << ": "
                                            << featureCompatibilityVersionInfo
                                            << ". See "
                                            << feature_compatibility_version::kDochubLink
                                            << ".");
            }

            ServerGlobalParams::FeatureCompatibility::Version version;
            if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion36) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k36;
            } else if (elem.String() == FeatureCompatibilityVersionCommandParser::kVersion34) {
                version = ServerGlobalParams::FeatureCompatibility::Version::k34;
            } else {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid value for " << fieldName << ", found "
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
                                            << featureCompatibilityVersionInfo
                                            << ". See "
                                            << feature_compatibility_version::kDochubLink
                                            << ".");
            }

            if (fieldName == FeatureCompatibilityVersion::kVersionField) {
                versionInfo.version = version;
            } else if (fieldName == FeatureCompatibilityVersion::kTargetVersionField) {
                versionInfo.targetVersion = version;
            }
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unrecognized field '" << fieldName << "'. Contents of "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionInfo
                                        << ". See "
                                        << feature_compatibility_version::kDochubLink
                                        << ".");
        }
    }

    if (versionInfo.version == ServerGlobalParams::FeatureCompatibility::Version::kUnset) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Missing required field '"
                                    << FeatureCompatibilityVersion::kVersionField
                                    << "''. Contents of "
                                    << FeatureCompatibilityVersion::kParameterName
                                    << " document in "
                                    << FeatureCompatibilityVersion::kCollection
                                    << ": "
                                    << featureCompatibilityVersionInfo
                                    << ". See "
                                    << feature_compatibility_version::kDochubLink
                                    << ".");
    }

    return versionInfo;
}

void FeatureCompatibilityVersion::set(OperationContext* opCtx, StringData version) {
    // Upgrades to a version, which sets the 'version' field only.
    _runUpdateCommand(opCtx, version, [version](auto updateMods) {
        BSONObjBuilder setOp(updateMods.subobjStart("$set"));
        setOp.append(FeatureCompatibilityVersion::kVersionField, version);
    });
}

void FeatureCompatibilityVersion::setTargetUpgrade(OperationContext* opCtx, StringData version) {
    // Only set 'targetVersion' field.
    _runUpdateCommand(opCtx, version, [version](auto updateMods) {
        BSONObjBuilder setOp(updateMods.subobjStart("$set"));
        setOp.append(FeatureCompatibilityVersion::kTargetVersionField, version);
    });
}

void FeatureCompatibilityVersion::setTargetDowngrade(OperationContext* opCtx, StringData version) {
    // Set both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, version, [version](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField, version);
        updateMods.append(FeatureCompatibilityVersion::kTargetVersionField, version);
    });
}

void FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(OperationContext* opCtx,
                                                                StringData version) {
    // Updates 'version' field, while also unsetting the 'targetVersion' field.
    _runUpdateCommand(opCtx, version, [version](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField, version);
    });
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

            // We update the value of the version server parameter so that the admin.system.version
            // collection gets a UUID.
            serverGlobalParams.featureCompatibility.targetVersion.store(
                ServerGlobalParams::FeatureCompatibility::Version::k36);

            uassertStatusOK(storageInterface->createCollection(opCtx, nss, {}));
        }

        // We then insert the featureCompatibilityVersion document into the "admin.system.version"
        // collection. The server parameter will be updated on commit by the op observer.
        uassertStatusOK(storageInterface->insertDocument(
            opCtx,
            nss,
            repl::TimestampedBSONObj{BSON("_id"
                                          << FeatureCompatibilityVersion::kParameterName
                                          << FeatureCompatibilityVersion::kVersionField
                                          << FeatureCompatibilityVersionCommandParser::kVersion36),
                                     SnapshotName()},
            repl::OpTime::kUninitializedTerm));  // No timestamp or term because this write is not
                                                 // replicated.
    }
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    auto versionInfo = uassertStatusOK(FeatureCompatibilityVersion::parse(doc));

    // Indicate if an upgrade or downgrade is in progress.
    if (versionInfo.targetVersion != ServerGlobalParams::FeatureCompatibility::Version::kUnset) {
        log() << "targeting featureCompatibilityVersion " << toString(versionInfo.targetVersion);
    }

    // To avoid extra log messages when the targetVersion is set/unset, only log when the version
    // changes.
    auto oldVersion = serverGlobalParams.featureCompatibility.version.load();
    auto newVersion = versionInfo.version;
    if (oldVersion != newVersion) {
        log() << "setting featureCompatibilityVersion to " << toString(newVersion);
    }

    // On commit, update the server parameters, and close any incoming connections with a wire
    // version that is below the minimum.
    opCtx->recoveryUnit()->onCommit([opCtx, versionInfo]() {
        serverGlobalParams.featureCompatibility.version.store(versionInfo.version);
        serverGlobalParams.featureCompatibility.targetVersion.store(versionInfo.targetVersion);
        _closeConnectionsBelowVersion(opCtx, versionInfo);
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
        serverGlobalParams.featureCompatibility.targetVersion.store(
            ServerGlobalParams::FeatureCompatibility::Version::kUnset);
    });
}

void FeatureCompatibilityVersion::onDropCollection(OperationContext* opCtx) {
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion34;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k34);
        serverGlobalParams.featureCompatibility.targetVersion.store(
            ServerGlobalParams::FeatureCompatibility::Version::kUnset);
    });
}

void FeatureCompatibilityVersion::_validateVersion(StringData version) {
    uassert(40284,
            str::stream() << "featureCompatibilityVersion must be '"
                          << FeatureCompatibilityVersionCommandParser::kVersion36
                          << "' or '"
                          << FeatureCompatibilityVersionCommandParser::kVersion34
                          << "'. See "
                          << feature_compatibility_version::kDochubLink
                          << ".",
            version == FeatureCompatibilityVersionCommandParser::kVersion36 ||
                version == FeatureCompatibilityVersionCommandParser::kVersion34);
}

void FeatureCompatibilityVersion::_closeConnectionsBelowVersion(
    OperationContext* opCtx, FeatureCompatibilityVersionInfo versionInfo) {

    // Close  all internal connections to versions lower than 3.6. Keep open in-progress connections
    // that haven't sent isMaster yet.
    if (versionInfo.version == ServerGlobalParams::FeatureCompatibility::Version::k36 ||
        versionInfo.targetVersion == ServerGlobalParams::FeatureCompatibility::Version::k36) {
        opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
            transport::Session::kLatestVersionInternalClientKeepOpen |
            transport::Session::kExternalClientKeepOpen);
    }
}

void FeatureCompatibilityVersion::_runUpdateCommand(OperationContext* opCtx,
                                                    StringData version,
                                                    UpdateBuilder builder) {
    _validateVersion(version);

    DBDirectClient client(opCtx);
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);

    BSONObjBuilder updateCmd;
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
                builder(std::move(updateMods));
            }
            updateSpec.appendBool("upsert", true);
        }
    }
    updateCmd.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Majority);

    // Update the featureCompatibilityVersion document stored in the "admin.system.version"
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.db().toString(), updateCmd.obj(), updateResult);
    uassertStatusOK(getStatusFromCommandResult(updateResult));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(updateResult));
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
                                    << feature_compatibility_version::kDochubLink
                                    << ".");
    }

    virtual Status setFromString(const std::string& str) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter. See "
                                    << feature_compatibility_version::kDochubLink
                                    << ".");
    }
} featureCompatibilityVersionParameter;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(internalValidateFeaturesAsMaster, bool, true);

}  // namespace mongo
