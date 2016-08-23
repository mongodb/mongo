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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/status.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

constexpr StringData FeatureCompatibilityVersion::kCollection;
constexpr StringData FeatureCompatibilityVersion::kCommandName;
constexpr StringData FeatureCompatibilityVersion::kParameterName;
constexpr StringData FeatureCompatibilityVersion::kVersionField;
constexpr StringData FeatureCompatibilityVersion::kVersion34;
constexpr StringData FeatureCompatibilityVersion::kVersion32;

StatusWith<ServerGlobalParams::FeatureCompatibilityVersions> FeatureCompatibilityVersion::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    bool foundVersionField = false;
    ServerGlobalParams::FeatureCompatibilityVersions version;

    for (auto&& elem : featureCompatibilityVersionDoc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == FeatureCompatibilityVersion::kVersionField) {
            foundVersionField = true;
            if (elem.type() != BSONType::String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << FeatureCompatibilityVersion::kVersionField
                                            << " must be of type String, but was of type "
                                            << typeName(elem.type())
                                            << ". Contents of "
                                            << FeatureCompatibilityVersion::kParameterName
                                            << " document in "
                                            << FeatureCompatibilityVersion::kCollection
                                            << ": "
                                            << featureCompatibilityVersionDoc);
            }
            if (elem.String() == FeatureCompatibilityVersion::kVersion34) {
                version = ServerGlobalParams::FeatureCompatibilityVersion_34;
            } else if (elem.String() == FeatureCompatibilityVersion::kVersion32) {
                version = ServerGlobalParams::FeatureCompatibilityVersion_32;
            } else {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Invalid value for "
                                            << FeatureCompatibilityVersion::kVersionField
                                            << ", found "
                                            << elem.String()
                                            << ", expected '"
                                            << FeatureCompatibilityVersion::kVersion34
                                            << "' or '"
                                            << FeatureCompatibilityVersion::kVersion32
                                            << "'. Contents of "
                                            << FeatureCompatibilityVersion::kParameterName
                                            << " document in "
                                            << FeatureCompatibilityVersion::kCollection
                                            << ": "
                                            << featureCompatibilityVersionDoc);
            }
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unrecognized field '" << elem.fieldName()
                                        << "''. Contents of "
                                        << FeatureCompatibilityVersion::kParameterName
                                        << " document in "
                                        << FeatureCompatibilityVersion::kCollection
                                        << ": "
                                        << featureCompatibilityVersionDoc);
        }
    }

    if (!foundVersionField) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Missing required field '"
                                    << FeatureCompatibilityVersion::kVersionField
                                    << "''. Contents of "
                                    << FeatureCompatibilityVersion::kParameterName
                                    << " document in "
                                    << FeatureCompatibilityVersion::kCollection
                                    << ": "
                                    << featureCompatibilityVersionDoc);
    }

    return version;
}

void FeatureCompatibilityVersion::set(OperationContext* txn, StringData version) {
    uassert(40284,
            "featureCompatibilityVersion must be '3.4' or '3.2'",
            version == FeatureCompatibilityVersion::kVersion34 ||
                version == FeatureCompatibilityVersion::kVersion32);

    // Update admin.system.version.
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);
    BSONObjBuilder updateCmd;
    updateCmd.append("update", nss.coll());
    updateCmd.append(
        "updates",
        BSON_ARRAY(BSON("q" << BSON("_id" << FeatureCompatibilityVersion::kParameterName) << "u"
                            << BSON(FeatureCompatibilityVersion::kVersionField << version)
                            << "upsert"
                            << true)));
    updateCmd.append("writeConcern",
                     BSON("w"
                          << "majority"));
    DBDirectClient client(txn);
    BSONObj result;
    client.runCommand(nss.db().toString(), updateCmd.obj(), result);
    uassertStatusOK(getStatusFromCommandResult(result));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(result));

    // Update server parameter.
    if (version == FeatureCompatibilityVersion::kVersion34) {
        serverGlobalParams.featureCompatibilityVersion.store(
            ServerGlobalParams::FeatureCompatibilityVersion_34);
    } else if (version == FeatureCompatibilityVersion::kVersion32) {
        serverGlobalParams.featureCompatibilityVersion.store(
            ServerGlobalParams::FeatureCompatibilityVersion_32);
    }
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* txn,
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

        // Insert featureCompatibilityDocument into admin.system.version.
        txn->setReplicatedWrites(false);
        NamespaceString nss(FeatureCompatibilityVersion::kCollection);
        CollectionOptions options;
        uassertStatusOK(storageInterface->createCollection(txn, nss, options));
        uassertStatusOK(storageInterface->insertDocument(
            txn,
            nss,
            BSON("_id" << FeatureCompatibilityVersion::kParameterName
                       << FeatureCompatibilityVersion::kVersionField
                       << FeatureCompatibilityVersion::kVersion34)));

        // Update server parameter.
        serverGlobalParams.featureCompatibilityVersion.store(
            ServerGlobalParams::FeatureCompatibilityVersion_34);
    }
}

void FeatureCompatibilityVersion::onInsertOrUpdate(const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    serverGlobalParams.featureCompatibilityVersion.store(
        uassertStatusOK(FeatureCompatibilityVersion::parse(doc)));
}

void FeatureCompatibilityVersion::onDelete(const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersion::kParameterName) {
        return;
    }
    serverGlobalParams.featureCompatibilityVersion.store(
        ServerGlobalParams::FeatureCompatibilityVersion_32);
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

    StringData featureCompatibilityVersionStr() {
        switch (serverGlobalParams.featureCompatibilityVersion.load()) {
            case ServerGlobalParams::FeatureCompatibilityVersion_34:
                return FeatureCompatibilityVersion::kVersion34;
            case ServerGlobalParams::FeatureCompatibilityVersion_32:
                return FeatureCompatibilityVersion::kVersion32;
            default:
                MONGO_UNREACHABLE;
        }
    }

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b.append(name, featureCompatibilityVersionStr());
    }

    virtual Status set(const BSONElement& newValueElement) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter");
    }

    virtual Status setFromString(const std::string& str) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << FeatureCompatibilityVersion::kParameterName
                                    << " cannot be set via setParameter");
    }
} featureCompatibilityVersionParameter;

}  // namespace mongo
