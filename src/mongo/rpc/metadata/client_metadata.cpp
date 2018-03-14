/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/client_metadata.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"

namespace mongo {

namespace {
constexpr auto kClientMetadataFieldName = "$client"_sd;

constexpr auto kApplication = "application"_sd;
constexpr auto kDriver = "driver"_sd;
constexpr auto kOperatingSystem = "os"_sd;

constexpr auto kArchitecture = "architecture"_sd;
constexpr auto kName = "name"_sd;
constexpr auto kType = "type"_sd;
constexpr auto kVersion = "version"_sd;

constexpr auto kMongoS = "mongos"_sd;
constexpr auto kHost = "host"_sd;
constexpr auto kClient = "client"_sd;

constexpr uint32_t kMaxMongoSMetadataDocumentByteLength = 512U;
// Due to MongoS appending more information to the client metadata document, we use a higher limit
// for MongoD to try to ensure that the appended information does not cause a failure.
constexpr uint32_t kMaxMongoDMetadataDocumentByteLength = 1024U;
constexpr uint32_t kMaxApplicationNameByteLength = 128U;

}  // namespace

StatusWith<boost::optional<ClientMetadata>> ClientMetadata::parse(const BSONElement& element) {
    if (element.eoo()) {
        return {boost::none};
    }

    if (!element.isABSONObj()) {
        return Status(ErrorCodes::TypeMismatch, "The client metadata document must be a document");
    }

    ClientMetadata clientMetadata;
    Status s = clientMetadata.parseClientMetadataDocument(element.Obj());
    if (!s.isOK()) {
        return s;
    }

    return {std::move(clientMetadata)};
}

Status ClientMetadata::parseClientMetadataDocument(const BSONObj& doc) {
    uint32_t maxLength = kMaxMongoDMetadataDocumentByteLength;
    if (isMongos()) {
        maxLength = kMaxMongoSMetadataDocumentByteLength;
    }

    if (static_cast<uint32_t>(doc.objsize()) > maxLength) {
        return Status(ErrorCodes::ClientMetadataDocumentTooLarge,
                      str::stream() << "The client metadata document must be less then or equal to "
                                    << maxLength
                                    << "bytes");
    }

    // Get a copy so that we can take a stable reference to the app name inside
    BSONObj docOwned = doc.getOwned();

    StringData appName;
    bool foundDriver = false;
    bool foundOperatingSystem = false;

    BSONObjIterator i(docOwned);
    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == kApplication) {
            // Application is an optional sub-document, but we require it to be a document if
            // specified.
            if (!e.isABSONObj()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "The '" << kApplication
                                            << "' field is required to be a BSON document in the "
                                               "client metadata document");
            }

            auto swAppName = parseApplicationDocument(e.Obj());
            if (!swAppName.getStatus().isOK()) {
                return swAppName.getStatus();
            }

            appName = swAppName.getValue();

        } else if (name == kDriver) {
            if (!e.isABSONObj()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "The '" << kDriver << "' field is required to be a "
                                                                     "BSON document in the client "
                                                                     "metadata document");
            }

            Status s = validateDriverDocument(e.Obj());
            if (!s.isOK()) {
                return s;
            }

            foundDriver = true;
        } else if (name == kOperatingSystem) {
            if (!e.isABSONObj()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "The '" << kOperatingSystem
                                            << "' field is required to be a BSON document in the "
                                               "client metadata document");
            }

            Status s = validateOperatingSystemDocument(e.Obj());
            if (!s.isOK()) {
                return s;
            }

            foundOperatingSystem = true;
        }

        // Ignore other fields as extra fields are allowed.
    }

    // Driver is a required sub document.
    if (!foundDriver) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required sub-document '" << kDriver
                                    << "' in the client metadata document");
    }

    // OS is a required sub document.
    if (!foundOperatingSystem) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required sub-document '" << kOperatingSystem
                                    << "' in the client metadata document");
    }

    _document = std::move(docOwned);
    _appName = std::move(appName);

    return Status::OK();
}

StatusWith<StringData> ClientMetadata::parseApplicationDocument(const BSONObj& doc) {
    BSONObjIterator i(doc);

    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        // Name is the only required field, and any other fields are simply ignored.
        if (name == kName) {

            if (e.type() != String) {
                return {
                    ErrorCodes::TypeMismatch,
                    str::stream() << "The '" << kApplication << "." << kName
                                  << "' field must be a string in the client metadata document"};
            }

            StringData value = e.checkAndGetStringData();

            if (value.size() > kMaxApplicationNameByteLength) {
                return {ErrorCodes::ClientMetadataAppNameTooLarge,
                        str::stream() << "The '" << kApplication << "." << kName
                                      << "' field must be less then or equal to "
                                      << kMaxApplicationNameByteLength
                                      << " bytes in the client metadata document"};
            }

            return {std::move(value)};
        }
    }

    return {StringData()};
}

Status ClientMetadata::validateDriverDocument(const BSONObj& doc) {
    bool foundName = false;
    bool foundVersion = false;

    BSONObjIterator i(doc);
    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == kName) {
            if (e.type() != String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream() << "The '" << kDriver << "." << kName
                                  << "' field must be a string in the client metadata document");
            }

            foundName = true;
        } else if (name == kVersion) {
            if (e.type() != String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream() << "The '" << kDriver << "." << kVersion
                                  << "' field must be a string in the client metadata document");
            }

            foundVersion = true;
        }
    }

    if (foundName == false) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required field '" << kDriver << "." << kName
                                    << "' in the client metadata document");
    }

    if (foundVersion == false) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required field '" << kDriver << "." << kVersion
                                    << "' in the client metadata document");
    }

    return Status::OK();
}

Status ClientMetadata::validateOperatingSystemDocument(const BSONObj& doc) {
    bool foundType = false;

    BSONObjIterator i(doc);
    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == kType) {
            if (e.type() != String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream() << "The '" << kOperatingSystem << "." << kType
                                  << "' field must be a string in the client metadata document");
            }

            foundType = true;
        }
    }

    if (foundType == false) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required field '" << kOperatingSystem << "."
                                    << kType
                                    << "' in the client metadata document");
    }

    return Status::OK();
}

void ClientMetadata::setMongoSMetadata(StringData hostAndPort,
                                       StringData mongosClient,
                                       StringData version) {
    BSONObjBuilder builder;
    builder.appendElements(_document);

    {
        auto sub = BSONObjBuilder(builder.subobjStart(kMongoS));
        sub.append(kHost, hostAndPort);
        sub.append(kClient, mongosClient);
        sub.append(kVersion, version);
    }

    auto document = builder.obj();

    if (!_appName.empty()) {
        // The _appName field points into the existing _document, which we are about to replace.
        // We must redirect _appName to point into the new doc *before* replacing the old doc. We
        // expect the 'application' metadata of the new document to be identical to the old.
        auto appMetaData = document[kApplication];
        invariant(appMetaData.isABSONObj());

        auto appNameEl = appMetaData[kName];
        invariant(appNameEl.type() == BSONType::String);

        auto appName = appNameEl.valueStringData();
        invariant(appName == _appName);

        _appName = appName;
    }

    _document = std::move(document);
}

void ClientMetadata::serialize(StringData driverName,
                               StringData driverVersion,
                               BSONObjBuilder* builder) {

    ProcessInfo processInfo;

    serializePrivate(driverName,
                     driverVersion,
                     processInfo.getOsType(),
                     processInfo.getOsName(),
                     processInfo.getArch(),
                     processInfo.getOsVersion(),
                     builder);
}

void ClientMetadata::serializePrivate(StringData driverName,
                                      StringData driverVersion,
                                      StringData osType,
                                      StringData osName,
                                      StringData osArchitecture,
                                      StringData osVersion,
                                      BSONObjBuilder* builder) {
    BSONObjBuilder metaObjBuilder(builder->subobjStart(kMetadataDocumentName));

    {
        BSONObjBuilder subObjBuilder(metaObjBuilder.subobjStart(kDriver));
        subObjBuilder.append(kName, driverName);
        subObjBuilder.append(kVersion, driverVersion);
    }

    {
        BSONObjBuilder subObjBuilder(metaObjBuilder.subobjStart(kOperatingSystem));
        subObjBuilder.append(kType, osType);
        subObjBuilder.append(kName, osName);
        subObjBuilder.append(kArchitecture, osArchitecture);
        subObjBuilder.append(kVersion, osVersion);
    }
}

Status ClientMetadata::serialize(StringData driverName,
                                 StringData driverVersion,
                                 StringData appName,
                                 BSONObjBuilder* builder) {

    ProcessInfo processInfo;

    return serializePrivate(driverName,
                            driverVersion,
                            processInfo.getOsType(),
                            processInfo.getOsName(),
                            processInfo.getArch(),
                            processInfo.getOsVersion(),
                            appName,
                            builder);
}

Status ClientMetadata::serializePrivate(StringData driverName,
                                        StringData driverVersion,
                                        StringData osType,
                                        StringData osName,
                                        StringData osArchitecture,
                                        StringData osVersion,
                                        StringData appName,
                                        BSONObjBuilder* builder) {
    if (appName.size() > kMaxApplicationNameByteLength) {
        return Status(ErrorCodes::ClientMetadataAppNameTooLarge,
                      str::stream() << "The '" << kApplication << "." << kName
                                    << "' field must be less then or equal to "
                                    << kMaxApplicationNameByteLength
                                    << " bytes in the client metadata document");
    }

    {
        BSONObjBuilder metaObjBuilder(builder->subobjStart(kMetadataDocumentName));

        if (!appName.empty()) {
            BSONObjBuilder subObjBuilder(metaObjBuilder.subobjStart(kApplication));
            subObjBuilder.append(kName, appName);
        }

        {
            BSONObjBuilder subObjBuilder(metaObjBuilder.subobjStart(kDriver));
            subObjBuilder.append(kName, driverName);
            subObjBuilder.append(kVersion, driverVersion);
        }

        {
            BSONObjBuilder subObjBuilder(metaObjBuilder.subobjStart(kOperatingSystem));
            subObjBuilder.append(kType, osType);
            subObjBuilder.append(kName, osName);
            subObjBuilder.append(kArchitecture, osArchitecture);
            subObjBuilder.append(kVersion, osVersion);
        }
    }

    return Status::OK();
}

StringData ClientMetadata::getApplicationName() const {
    return _appName;
}

const BSONObj& ClientMetadata::getDocument() const {
    return _document;
}

void ClientMetadata::logClientMetadata(Client* client) const {
    invariant(!getDocument().isEmpty());
    log() << "received client metadata from " << client->getRemote().toString() << " "
          << client->desc() << ": " << getDocument();
}

StringData ClientMetadata::fieldName() {
    return kClientMetadataFieldName;
}

}  // namespace mongo
