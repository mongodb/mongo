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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/wire_version.h"
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

Lock::ResourceMutex FeatureCompatibilityVersion::fcvLock("featureCompatibilityVersionLock");

StatusWith<ServerGlobalParams::FeatureCompatibility::Version> FeatureCompatibilityVersion::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    ServerGlobalParams::FeatureCompatibility::Version version =
        ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior;
    std::string versionString;
    std::string targetVersionString;

    for (auto&& elem : featureCompatibilityVersionDoc) {
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
                                            << featureCompatibilityVersionDoc
                                            << ". See "
                                            << feature_compatibility_version::kDochubLink
                                            << ".");
            }

            if (elem.String() != FeatureCompatibilityVersionCommandParser::kVersion40 &&
                elem.String() != FeatureCompatibilityVersionCommandParser::kVersion36 &&
                elem.String() != FeatureCompatibilityVersionCommandParser::kVersion34) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid value for " << fieldName << ", found "
                                            << elem.String()
                                            << ", expected '"
                                            << FeatureCompatibilityVersionCommandParser::kVersion40
                                            << "', '"
                                            << FeatureCompatibilityVersionCommandParser::kVersion36
                                            << "' or '"
                                            << FeatureCompatibilityVersionCommandParser::kVersion34
                                            << "'. Contents of "
                                            << FeatureCompatibilityVersion::kParameterName
                                            << " document in "
                                            << FeatureCompatibilityVersion::kCollection
                                            << ": "
                                            << featureCompatibilityVersionDoc
                                            << ". See "
                                            << feature_compatibility_version::kDochubLink
                                            << ".");
            }

            if (fieldName == FeatureCompatibilityVersion::kVersionField) {
                versionString = elem.String();
            } else if (fieldName == FeatureCompatibilityVersion::kTargetVersionField) {
                targetVersionString = elem.String();
            }
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unrecognized field '" << fieldName << "'. Contents of "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionDoc
                                        << ". See "
                                        << feature_compatibility_version::kDochubLink
                                        << ".");
        }
    }

    if (versionString == FeatureCompatibilityVersionCommandParser::kVersion34) {
        if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion40) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid state for "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionDoc
                                        << ". See "
                                        << feature_compatibility_version::kDochubLink
                                        << ".");
        } else if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion36) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36;
        } else if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion34) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34;
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34;
        }
    } else if (versionString == FeatureCompatibilityVersionCommandParser::kVersion36) {
        if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion40) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40;
        } else if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion36) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36;
        } else if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion34) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid state for "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionDoc
                                        << ". See "
                                        << feature_compatibility_version::kDochubLink
                                        << ".");
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36;
        }
    } else if (versionString == FeatureCompatibilityVersionCommandParser::kVersion40) {
        if (targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion40 ||
            targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion36 ||
            targetVersionString == FeatureCompatibilityVersionCommandParser::kVersion34) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid state for "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionDoc
                                        << ". See "
                                        << feature_compatibility_version::kDochubLink
                                        << ".");
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40;
        }
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Missing required field '"
                                    << FeatureCompatibilityVersion::kVersionField
                                    << "''. Contents of "
                                    << FeatureCompatibilityVersion::kParameterName
                                    << " document in "
                                    << FeatureCompatibilityVersion::kCollection
                                    << ": "
                                    << featureCompatibilityVersionDoc
                                    << ". See "
                                    << feature_compatibility_version::kDochubLink
                                    << ".");
    }

    return version;
}

void FeatureCompatibilityVersion::setTargetUpgrade_DEPRECATED(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion34);
        updateMods.append(FeatureCompatibilityVersion::kTargetVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion36);
    });
}

void FeatureCompatibilityVersion::setTargetDowngrade_DEPRECATED(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion34);
        updateMods.append(FeatureCompatibilityVersion::kTargetVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion34);
    });
}

void FeatureCompatibilityVersion::setTargetUpgrade(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion36);
        updateMods.append(FeatureCompatibilityVersion::kTargetVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion40);
    });
}

void FeatureCompatibilityVersion::setTargetDowngrade(OperationContext* opCtx) {
    // Sets both 'version' and 'targetVersion' fields.
    _runUpdateCommand(opCtx, [](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion36);
        updateMods.append(FeatureCompatibilityVersion::kTargetVersionField,
                          FeatureCompatibilityVersionCommandParser::kVersion36);
    });
}

void FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(OperationContext* opCtx,
                                                                StringData version) {
    _validateVersion(version);

    // Updates 'version' field, while also unsetting the 'targetVersion' field.
    _runUpdateCommand(opCtx, [version](auto updateMods) {
        updateMods.append(FeatureCompatibilityVersion::kVersionField, version);
    });
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* opCtx,
                                                    repl::StorageInterface* storageInterface) {
    if (!isCleanStartUp())
        return;

    // If the server was not started with --shardsvr, the default featureCompatibilityVersion on
    // clean startup is the upgrade version. If it was started with --shardsvr, the default
    // featureCompatibilityVersion is the downgrade version, so that it can be safely added to a
    // downgrade version cluster. The config server will run setFeatureCompatibilityVersion as part
    // of addShard.
    const bool storeUpgradeVersion = serverGlobalParams.clusterRole != ClusterRole::ShardServer;

    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);

    {
        CollectionOptions options;
        options.uuid = CollectionUUID::gen();
        uassertStatusOK(storageInterface->createCollection(opCtx, nss, options));
    }

    // We then insert the featureCompatibilityVersion document into the "admin.system.version"
    // collection. The server parameter will be updated on commit by the op observer.
    // TODO(SERVER-32597): If storeUpgradeVersion is true, kVersion38 should be stored.
    uassertStatusOK(storageInterface->insertDocument(
        opCtx,
        nss,
        repl::TimestampedBSONObj{
            BSON("_id" << FeatureCompatibilityVersion::kParameterName
                       << FeatureCompatibilityVersion::kVersionField
                       << (storeUpgradeVersion
                               ? FeatureCompatibilityVersionCommandParser::kVersion36
                               : FeatureCompatibilityVersionCommandParser::kVersion36)),
            Timestamp()},
        repl::OpTime::kUninitializedTerm));  // No timestamp or term because this write is not
                                             // replicated.
}

bool FeatureCompatibilityVersion::isCleanStartUp() {
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&dbNames);

    for (auto&& dbName : dbNames) {
        if (dbName != "local") {
            return false;
        }
    }
    return true;
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersion::parse(doc));

    // To avoid extra log messages when the targetVersion is set/unset, only log when the version
    // changes.
    auto oldVersion = serverGlobalParams.featureCompatibility.getVersion();
    if (oldVersion != newVersion) {
        log() << "setting featureCompatibilityVersion to " << toString(newVersion);
    }

    // On commit, update the server parameters, and close any incoming connections with a wire
    // version that is below the minimum.
    opCtx->recoveryUnit()->onCommit([opCtx, newVersion]() {
        serverGlobalParams.featureCompatibility.setVersion(newVersion);
        updateMinWireVersion();

        // Close all incoming connections from internal clients with binary versions lower than
        // ours. It would be desirable to close all outgoing connections to servers with lower
        // binary version, but it is not currently possible.
        if (newVersion != ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34) {
            opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
                transport::Session::kLatestVersionInternalClientKeepOpen |
                transport::Session::kExternalClientKeepOpen);
        }
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
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34);
        updateMinWireVersion();
    });
}

void FeatureCompatibilityVersion::onDropCollection(OperationContext* opCtx) {
    log() << "setting featureCompatibilityVersion to "
          << FeatureCompatibilityVersionCommandParser::kVersion34;
    opCtx->recoveryUnit()->onCommit([]() {
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34);
        updateMinWireVersion();
    });
}

void FeatureCompatibilityVersion::updateMinWireVersion() {
    WireSpec& spec = WireSpec::instance();

    switch (serverGlobalParams.featureCompatibility.getVersion()) {
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40:
        case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36:
        case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION - 1;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION - 1;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION - 2;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION - 2;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior:
            // getVersion() does not return this value.
            MONGO_UNREACHABLE;
    }
}

void FeatureCompatibilityVersion::_validateVersion(StringData version) {
    uassert(40284,
            str::stream() << "featureCompatibilityVersion must be '"
                          << FeatureCompatibilityVersionCommandParser::kVersion40
                          << "', '"
                          << FeatureCompatibilityVersionCommandParser::kVersion36
                          << "' or '"
                          << FeatureCompatibilityVersionCommandParser::kVersion34
                          << "'. See "
                          << feature_compatibility_version::kDochubLink
                          << ".",
            version == FeatureCompatibilityVersionCommandParser::kVersion40 ||
                version == FeatureCompatibilityVersionCommandParser::kVersion36 ||
                version == FeatureCompatibilityVersionCommandParser::kVersion34);
}

void FeatureCompatibilityVersion::_runUpdateCommand(OperationContext* opCtx,
                                                    UpdateBuilder builder) {
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
    uassertStatusOK(getStatusFromWriteCommandReply(updateResult));
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
        BSONObjBuilder featureCompatibilityVersionBuilder(b.subobjStart(name));
        switch (serverGlobalParams.featureCompatibility.getVersion()) {
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion40);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion36);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kTargetVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion40);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion36);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kTargetVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion36);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion36);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion34);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kTargetVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion36);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion34);
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kTargetVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion34);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34:
                featureCompatibilityVersionBuilder.append(
                    FeatureCompatibilityVersion::kVersionField,
                    FeatureCompatibilityVersionCommandParser::kVersion34);
                return;
            case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior:
                // getVersion() does not return this value.
                MONGO_UNREACHABLE;
        }
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
