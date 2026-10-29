// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/shell/kms.h"

#include "mongo/idl/idl_parser.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <string_view>
#include <utility>

#include <absl/container/node_hash_map.h>


namespace mongo {
using namespace std::literals::string_view_literals;

BSONObj KMSService::encryptDataKeyByString(ConstDataRange cdr, std::string_view keyId) {
    uasserted(5380101,
              str::stream() << "Customer Master Keys for " << name()
                            << " must be BSON object, not a string.");
}

BSONObj KMSService::encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId) {
    uasserted(5380102,
              str::stream() << "Customer Master Keys for " << name()
                            << " must be a string, not a BSON object.");
}

stdx::unordered_map<KMSProviderEnum, std::unique_ptr<KMSServiceFactory>>
    KMSServiceController::_factories;

void KMSServiceController::registerFactory(KMSProviderEnum provider,
                                           std::unique_ptr<KMSServiceFactory> factory) {
    auto ret = _factories.insert({provider, std::move(factory)});
    invariant(ret.second);
}

std::unique_ptr<KMSService> KMSServiceController::createFromClient(std::string_view kmsProvider,
                                                                   const BSONObj& config) {
    KMSProviderEnum provider =
        idl::deserialize<KMSProviderEnum>(kmsProvider, IDLParserContext("client fle options"));

    auto service = _factories.at(provider)->create(config);
    uassert(51192, str::stream() << "Cannot find client kms provider " << kmsProvider, service);
    return service;
}

std::unique_ptr<KMSService> KMSServiceController::createFromDisk(const BSONObj& config,
                                                                 const BSONObj& masterKey) {
    auto providerObj = masterKey.getStringField("provider"sv);
    auto provider = idl::deserialize<KMSProviderEnum>(providerObj, IDLParserContext("root"));
    auto service = _factories.at(provider)->create(config);
    uassert(51193, str::stream() << "Cannot find disk kms provider " << providerObj, service);
    return service;
}

}  // namespace mongo
