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


#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/client_metadata.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

namespace {
constexpr auto kClientMetadataFieldName = "$client"_sd;

constexpr auto kApplication = "application"_sd;
constexpr auto kDriver = "driver"_sd;
constexpr auto kOperatingSystem = "os"_sd;

constexpr auto kArchitecture = "architecture"_sd;
constexpr auto kName = "name"_sd;
constexpr auto kPid = "pid"_sd;
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

struct ClientMetadataState {
    bool isFinalized = false;
    boost::optional<ClientMetadata> meta;
};
const auto getClientState = Client::declareDecoration<ClientMetadataState>();
const auto getOperationState = OperationContext::declareDecoration<ClientMetadataState>();

}  // namespace

StatusWith<boost::optional<ClientMetadata>> ClientMetadata::parse(const BSONElement& element) try {
    if (element.eoo()) {
        return {boost::none};
    }

    if (!element.isABSONObj()) {
        return Status(ErrorCodes::TypeMismatch, "The client metadata document must be a document");
    }

    return boost::make_optional(parseFromBSON(element.Obj()));
} catch (const DBException& ex) {
    return ex.toStatus();
}

ClientMetadata::ClientMetadata(BSONObj doc) {
    uint32_t maxLength = kMaxMongoDMetadataDocumentByteLength;
    if (isMongos()) {
        maxLength = kMaxMongoSMetadataDocumentByteLength;
    }

    uassert(ErrorCodes::ClientMetadataDocumentTooLarge,
            str::stream() << "The client metadata document must be less then or equal to "
                          << maxLength << " bytes",
            static_cast<uint32_t>(doc.objsize()) <= maxLength);

    const auto isobj = [](StringData name, const BSONElement& e) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream()
                    << "The '" << name
                    << "' field is required to be a BSON document in the client metadata document",
                e.isABSONObj());
    };

    // Get a copy so that we can take a stable reference to the app name inside
    _document = doc.getOwned();

    bool foundDriver = false;
    bool foundOperatingSystem = false;
    for (const auto& e : _document) {
        auto name = e.fieldNameStringData();

        if (name == kApplication) {
            // Application is an optional sub-document, but we require it to be a document if
            // specified.
            isobj(kApplication, e);
            _appName = uassertStatusOK(parseApplicationDocument(e.Obj()));
        } else if (name == kDriver) {
            isobj(kDriver, e);
            uassertStatusOK(validateDriverDocument(e.Obj()));
            foundDriver = true;
        } else if (name == kOperatingSystem) {
            isobj(kOperatingSystem, e);
            uassertStatusOK(validateOperatingSystemDocument(e.Obj()));
            foundOperatingSystem = true;
        }

        // Ignore other fields as extra fields are allowed.
    }

    // Driver is a required sub document.
    uassert(ErrorCodes::ClientMetadataMissingField,
            str::stream() << "Missing required sub-document '" << kDriver
                          << "' in the client metadata document",
            foundDriver);

    // OS is a required sub document.
    uassert(ErrorCodes::ClientMetadataMissingField,
            str::stream() << "Missing required sub-document '" << kOperatingSystem
                          << "' in the client metadata document",
            foundOperatingSystem);
}

StatusWith<StringData> ClientMetadata::parseApplicationDocument(const BSONObj& doc) {
    BSONObjIterator i(doc);

    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        // Name is the only required field, and any other fields are simply ignored.
        if (name == kName) {

            if (e.type() != String) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The '" << kApplication << "." << kName
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
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "The '" << kDriver << "." << kName
                                  << "' field must be a string in the client metadata document");
            }

            foundName = true;
        } else if (name == kVersion) {
            if (e.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "The '" << kDriver << "." << kVersion
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
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "The '" << kOperatingSystem << "." << kType
                                  << "' field must be a string in the client metadata document");
            }

            foundType = true;
        }
    }

    if (foundType == false) {
        return Status(ErrorCodes::ClientMetadataMissingField,
                      str::stream() << "Missing required field '" << kOperatingSystem << "."
                                    << kType << "' in the client metadata document");
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

    std::string appName;
    if (TestingProctor::instance().isEnabled()) {
        appName = processInfo.getProcessName();
        if (appName.length() > kMaxApplicationNameByteLength) {
            static constexpr auto kEllipsis = "..."_sd;
            appName.replace(appName.begin() + kMaxApplicationNameByteLength - kEllipsis.size(),
                            appName.end(),
                            kEllipsis.begin(),
                            kEllipsis.end());
        }
    }

    serializePrivate(driverName,
                     driverVersion,
                     processInfo.getOsType(),
                     processInfo.getOsName(),
                     processInfo.getArch(),
                     processInfo.getOsVersion(),
                     appName,
                     builder)
        .ignore();
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
            if (TestingProctor::instance().isEnabled()) {
                subObjBuilder.append(kPid, ProcessId::getCurrent().toString());
            }
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
    if (getDocument().isEmpty()) {
        return;
    }

    LOGV2(51800,
          "received client metadata from {remote} {client}: {doc}",
          "client metadata",
          "remote"_attr = client->getRemote(),
          "client"_attr = client->desc(),
          "doc"_attr = getDocument());
}

StringData ClientMetadata::fieldName() {
    return kClientMetadataFieldName;
}

bool ClientMetadata::tryFinalize(Client* client) {
    auto lk = stdx::unique_lock(*client);
    auto& state = getClientState(client);
    if (std::exchange(state.isFinalized, true)) {
        return false;
    }

    lk.unlock();

    if (state.meta) {
        // If we reach this point, the ClientMetadata is effectively immutable because isFinalized
        // is true.
        state.meta->logClientMetadata(client);
    }

    return true;
}

const ClientMetadata* ClientMetadata::getForClient(Client* client) noexcept {
    auto& state = getClientState(client);
    if (!state.meta) {
        // If we haven't finalized, it's still okay to return our existing value.
        return nullptr;
    }
    return &state.meta.value();
}

const ClientMetadata* ClientMetadata::getForOperation(OperationContext* opCtx) noexcept {
    auto& state = getOperationState(opCtx);
    if (!state.isFinalized) {
        return nullptr;
    }
    invariant(state.meta);
    return &state.meta.value();
}

const ClientMetadata* ClientMetadata::get(Client* client) noexcept {
    if (auto opCtx = client->getOperationContext()) {
        if (auto meta = getForOperation(opCtx)) {
            return meta;
        }
    }

    return getForClient(client);
}

void ClientMetadata::setAndFinalize(Client* client, boost::optional<ClientMetadata> meta) {
    invariant(TestingProctor::instance().isEnabled());

    auto lk = stdx::lock_guard(*client);

    auto& state = getClientState(client);
    state.isFinalized = true;
    state.meta = std::move(meta);
}

void ClientMetadata::setFromMetadataForOperation(OperationContext* opCtx, BSONElement& elem) {
    if (MONGO_unlikely(elem.eoo())) {
        return;
    }
    auto lk = stdx::lock_guard(*opCtx->getClient());

    auto& state = getOperationState(opCtx);
    uassert(ErrorCodes::ClientMetadataCannotBeMutated,
            "The client metadata document may only be set once per operation",
            !state.meta && !state.isFinalized);
    auto inputMetadata = ClientMetadata::readFromMetadata(elem);

    state.isFinalized = true;
    state.meta = std::move(inputMetadata);
}

void ClientMetadata::setFromMetadata(Client* client, BSONElement& elem) {
    if (elem.eoo()) {
        return;
    }

    auto& state = getClientState(client);
    {
        auto lk = stdx::lock_guard(*client);
        uassert(ErrorCodes::ClientMetadataCannotBeMutated,
                "The client metadata document may only be sent in the first hello",
                !state.isFinalized);
    }

    auto meta = ClientMetadata::readFromMetadata(elem);
    if (meta && isMongos()) {
        // If we had a full ClientMetadata and we're on mongos, attach some additional client data.
        meta->setMongoSMetadata(getHostNameCachedAndPort(),
                                client->clientAddress(true),
                                VersionInfoInterface::instance().version());
    }

    auto lk = stdx::lock_guard(*client);
    state.meta = std::move(meta);
}

boost::optional<ClientMetadata> ClientMetadata::readFromMetadata(BSONElement& element) {
    return uassertStatusOK(ClientMetadata::parse(element));
}

void ClientMetadata::writeToMetadata(BSONObjBuilder* builder) const noexcept {
    auto& document = getDocument();
    if (document.isEmpty()) {
        // Skip appending metadata if there is none
        return;
    }

    builder->append(ClientMetadata::fieldName(), document);
}

}  // namespace mongo
