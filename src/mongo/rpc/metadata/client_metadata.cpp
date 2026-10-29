// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/rpc/metadata/client_metadata.h"

#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/metadata/client_metadata_server_parameters_gen.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/version.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
constexpr auto kClientMetadataFieldName = "$client"sv;

constexpr auto kApplication = "application"sv;
constexpr auto kDriver = "driver"sv;
constexpr auto kOperatingSystem = "os"sv;

constexpr auto kArchitecture = "architecture"sv;
constexpr auto kName = "name"sv;
constexpr auto kPid = "pid"sv;
constexpr auto kType = "type"sv;
constexpr auto kVersion = "version"sv;

// "cid" is a driver-assigned client identifier reported in clientUpdate metadata documents.
constexpr auto kCid = "cid"sv;
constexpr auto kMongoS = "mongos"sv;
constexpr auto kHost = "host"sv;
constexpr auto kClient = "client"sv;

constexpr uint32_t kMaxMongoSMetadataDocumentByteLength = 512U;
// Due to MongoS appending more information to the client metadata document, we use a higher limit
// for MongoD to try to ensure that the appended information does not cause a failure.
constexpr uint32_t kMaxMongoDMetadataDocumentByteLength = 1024U;
constexpr uint32_t kMaxApplicationNameByteLength = 128U;

logv2::SeveritySuppressor& getClientMetadataUpdateLogSuppressor() {
    static logv2::SeveritySuppressor suppressor{
        Milliseconds(1000 / gClientMetadataUpdateLogRatePerSec),
        logv2::LogSeverity::Info(),
        logv2::LogSeverity::Debug(2)};
    return suppressor;
}

int getClientMetadataUpdateLogSeverityLevel() {
    if (gClientMetadataUpdateLogRatePerSec == 0) {
        // 0 means "no suppression": log all entries at INFO.
        return logv2::LogSeverity::Info().toInt();
    }
    auto& suppressor = getClientMetadataUpdateLogSuppressor();
    return suppressor().toInt();
}

struct ClientMetadataState {
    bool isFinalized = false;
    boost::optional<ClientMetadata> meta;
};
const auto getClientState = Client::declareDecoration<ClientMetadataState>();
const auto getOperationState = OperationContext::declareDecoration<ClientMetadataState>();

// Timestamp (milliseconds since epoch) of the last clientUpdate log emitted for this connection.
const auto getLastClientMetadataUpdateLogTimeMillis = Client::declareDecoration<int64_t>();

auto& clientMetadataUpdateValidationFailures =
    *MetricBuilder<Counter64>("network.clientMetadataUpdate.validationFailures");
}  // namespace

void ClientMetadata::setUpdateLogSuppressorClockSource_forTest(ClockSource* cs) {
    auto& suppressor = getClientMetadataUpdateLogSuppressor();
    suppressor.resetWithClockSource_forTest(cs);
    // The suppressor's period is captured once at first construction. Resyncing it here allows a
    // test that changes gClientMetadataUpdateLogRatePerSec between cases to observe the new value.
    suppressor.setPeriod(Milliseconds(1000 / gClientMetadataUpdateLogRatePerSec));
}

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
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        maxLength = kMaxMongoSMetadataDocumentByteLength;
    }

    uassert(ErrorCodes::ClientMetadataDocumentTooLarge,
            str::stream() << "The client metadata document must be less then or equal to "
                          << maxLength << " bytes",
            static_cast<uint32_t>(doc.objsize()) <= maxLength);

    const auto isobj = [](std::string_view name, const BSONElement& e) {
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
            _driverName = std::string{e.Obj()[kName].checkAndGetStringData()};
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

StatusWith<std::string> ClientMetadata::parseApplicationDocument(const BSONObj& doc) {
    BSONObjIterator i(doc);

    while (i.more()) {
        BSONElement e = i.next();
        std::string_view name = e.fieldNameStringData();

        // Name is the only required field, and any other fields are simply ignored.
        if (name == kName) {

            if (e.type() != BSONType::string) {
                return {ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The '" << kApplication << "." << kName
                            << "' field must be a string in the client metadata document"};
            }

            std::string value = str::escape(std::string{e.checkAndGetStringData()});

            if (value.size() > kMaxApplicationNameByteLength) {
                return {ErrorCodes::ClientMetadataAppNameTooLarge,
                        str::stream() << "The '" << kApplication << "." << kName
                                      << "' field must be less then or equal to "
                                      << kMaxApplicationNameByteLength
                                      << " bytes in the client metadata document"};
            }

            return std::move(value);
        }
    }

    return std::string();
}

Status ClientMetadata::validateDriverDocument(const BSONObj& doc) {
    bool foundName = false;
    bool foundVersion = false;

    BSONObjIterator i(doc);
    while (i.more()) {
        BSONElement e = i.next();
        std::string_view name = e.fieldNameStringData();

        if (name == kName) {
            if (e.type() != BSONType::string) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "The '" << kDriver << "." << kName
                                  << "' field must be a string in the client metadata document");
            }

            foundName = true;
        } else if (name == kVersion) {
            if (e.type() != BSONType::string) {
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
        std::string_view name = e.fieldNameStringData();

        if (name == kType) {
            if (e.type() != BSONType::string) {
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

void ClientMetadata::setMongoSMetadata(std::string_view hostAndPort,
                                       std::string_view mongosClient,
                                       std::string_view version) {
    _documentWithoutMongosInfo = _document;
    BSONObjBuilder builder;
    builder.appendElements(_document);

    {
        auto sub = BSONObjBuilder(builder.subobjStart(kMongoS));
        sub.append(kHost, hostAndPort);
        sub.append(kClient, mongosClient);
        sub.append(kVersion, version);
    }

    _document = builder.obj();
}

void ClientMetadata::serialize(std::string_view driverName,
                               std::string_view driverVersion,
                               BSONObjBuilder* builder) {

    ProcessInfo processInfo;

    std::string appName;
    if (TestingProctor::instance().isEnabled()) {
        appName = processInfo.getProcessName();
        if (appName.length() > kMaxApplicationNameByteLength) {
            static constexpr auto kEllipsis = "..."sv;
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

Status ClientMetadata::serialize(std::string_view driverName,
                                 std::string_view driverVersion,
                                 std::string_view appName,
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

Status ClientMetadata::serializePrivate(std::string_view driverName,
                                        std::string_view driverVersion,
                                        std::string_view osType,
                                        std::string_view osName,
                                        std::string_view osArchitecture,
                                        std::string_view osVersion,
                                        std::string_view appName,
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

std::string_view ClientMetadata::getApplicationName() const {
    return std::string_view(_appName);
}

std::string_view ClientMetadata::getDriverName() const {
    return std::string_view(_driverName);
}

const BSONObj& ClientMetadata::getDocument() const {
    return _document;
}

unsigned long ClientMetadata::hashWithoutMongosInfo() const {
    return static_cast<unsigned long>(_hashWithoutMongos.get(documentWithoutMongosInfo()));
}

const BSONObj& ClientMetadata::documentWithoutMongosInfo() const {
    return _documentWithoutMongosInfo.get(_document);
}

void ClientMetadata::logClientMetadata(Client* client) const {
    if (getDocument().isEmpty()) {
        return;
    }

    if (serverGlobalParams.quiet.load()) {
        return;
    }

    auto negotiatedCompressors =
        MessageCompressorManager::forSession(client->session()).getNegotiatedCompressors();
    std::vector<std::string_view> negotiatedCompressorNames(negotiatedCompressors.size());
    std::transform(
        negotiatedCompressors.begin(),
        negotiatedCompressors.end(),
        negotiatedCompressorNames.begin(),
        [](auto& messageCompressor) { return std::string_view(messageCompressor->getName()); });

    LOGV2(51800,
          "client metadata",
          "remote"_attr = client->getRemote(),
          "client"_attr = client->desc(),
          "negotiatedCompressors"_attr = negotiatedCompressorNames,
          "doc"_attr = getDocument());
}

std::string_view ClientMetadata::fieldName() {
    return kClientMetadataFieldName;
}

Status ClientMetadata::validateClientMetadataUpdate(const BSONObj& doc) {
    if (doc.objsize() > gClientMetadataUpdateDocumentMaxByteLength) {
        return Status(
            ErrorCodes::ClientMetadataDocumentTooLarge,
            fmt::format(
                "The client metadata update document must be less than or equal to {} bytes",
                gClientMetadataUpdateDocumentMaxByteLength));
    }

    const BSONElement cidElem = doc.getField(kCid);
    if (!cidElem.eoo() && cidElem.type() != BSONType::string) {
        return Status(
            ErrorCodes::TypeMismatch,
            fmt::format("The '{}' field must be a string in the client metadata update document",
                        kCid));
    }

    return Status::OK();
}

void ClientMetadata::logClientMetadataUpdate(Client* client, const BSONObj& updateDoc) {
    if (serverGlobalParams.quiet.load()) {
        return;
    }

    auto& lastLogTimeMillis = getLastClientMetadataUpdateLogTimeMillis(client);
    const auto now = client->getServiceContext()->getFastClockSource()->now();
    const Date_t lastLogTime = Date_t::fromMillisSinceEpoch(lastLogTimeMillis);
    if (now - lastLogTime < Seconds(gClientMetadataUpdateLogPerConnectionThrottlingSecs.load())) {
        return;
    }

    lastLogTimeMillis = now.toMillisSinceEpoch();

    // Skip validation and auth enrichment when the entry won't be emitted - validation traverses
    // the BSON document and should not run on suppressed entries.
    const auto severity = logv2::LogSeverity::cast(getClientMetadataUpdateLogSeverityLevel());
    if (!logv2::shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, severity)) {
        return;
    }

    auto status = validateClientMetadataUpdate(updateDoc);
    if (!status.isOK()) {
        clientMetadataUpdateValidationFailures.increment();
        return;
    }

    const bool authenticated = AuthorizationSession::exists(client) &&
        AuthorizationSession::get(client)->isAuthenticated();

    LOGV2_DEBUG(51817,
                severity.toInt(),
                "client metadata",
                "remote"_attr = client->getRemote(),
                "client"_attr = client->desc(),
                "auth"_attr = authenticated,
                "doc"_attr = updateDoc);
}

bool ClientMetadata::tryFinalize(Client* client) {
    auto lk = std::unique_lock(*client);
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

const ClientMetadata* ClientMetadata::getForClient(Client* client) {
    auto& state = getClientState(client);
    if (!state.meta) {
        // If we haven't finalized, it's still okay to return our existing value.
        return nullptr;
    }
    return &state.meta.value();
}

const ClientMetadata* ClientMetadata::getForOperation(OperationContext* opCtx) {
    auto& state = getOperationState(opCtx);
    if (!state.isFinalized) {
        return nullptr;
    }
    invariant(state.meta);
    return &state.meta.value();
}

const ClientMetadata* ClientMetadata::get(Client* client) {
    if (auto opCtx = client->getOperationContext()) {
        if (auto meta = getForOperation(opCtx)) {
            return meta;
        }
    }

    return getForClient(client);
}

void ClientMetadata::setAndFinalize(Client* client, boost::optional<ClientMetadata> meta) {
    invariant(TestingProctor::instance().isEnabled());

    auto lk = std::lock_guard(*client);

    auto& state = getClientState(client);
    state.isFinalized = true;
    state.meta = std::move(meta);
}

void ClientMetadata::setFromMetadataForOperation(OperationContext* opCtx, const BSONObj& obj) {
    auto lk = std::lock_guard(*opCtx->getClient());

    auto& state = getOperationState(opCtx);
    uassert(ErrorCodes::ClientMetadataCannotBeMutated,
            "The client metadata document may only be set once per operation",
            !state.meta && !state.isFinalized);
    auto inputMetadata = ClientMetadata::parseFromBSON(obj);

    state.isFinalized = true;
    state.meta = std::move(inputMetadata);
}

void ClientMetadata::setFromMetadata(Client* client,
                                     const BSONElement& elem,
                                     bool isInternalClient) {
    if (elem.eoo()) {
        return;
    }

    auto& state = getClientState(client);
    {
        auto lk = std::lock_guard(*client);
        uassert(ErrorCodes::ClientMetadataCannotBeMutated,
                "The client metadata document may only be sent in the first hello",
                !state.isFinalized);
    }

    auto meta = ClientMetadata::readFromMetadata(elem);

    if (!isInternalClient) {
        uassert(ErrorCodes::ClientMetadataDocumentTooLarge,
                str::stream() << "The client metadata document must be less than or equal to "
                              << kMaxMongoSMetadataDocumentByteLength << " bytes",
                static_cast<uint32_t>(meta->_document.objsize()) <=
                    kMaxMongoSMetadataDocumentByteLength);
    }

    if (meta && serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        // If we had a full ClientMetadata and we're on mongos, attach some additional client data.
        meta->setMongoSMetadata(prettyHostNameAndPort(client->getLocalPort()),
                                client->clientAddress(true),
                                VersionInfoInterface::instance().version());
    }

    auto lk = std::lock_guard(*client);
    state.meta = std::move(meta);
}

boost::optional<ClientMetadata> ClientMetadata::readFromMetadata(const BSONElement& element) {
    return uassertStatusOK(ClientMetadata::parse(element));
}

void ClientMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    auto& document = getDocument();
    if (document.isEmpty()) {
        // Skip appending metadata if there is none
        return;
    }

    builder->append(ClientMetadata::fieldName(), document);
}

}  // namespace mongo
