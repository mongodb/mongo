/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "kms.h"

#include "mongo/idl/idl_parser.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <utility>

#include <absl/container/node_hash_map.h>


namespace mongo {

BSONObj KMSService::encryptDataKeyByString(ConstDataRange cdr, StringData keyId) {
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

std::unique_ptr<KMSService> KMSServiceController::createFromClient(StringData kmsProvider,
                                                                   const BSONObj& config) {
    KMSProviderEnum provider =
        KMSProvider_parse(kmsProvider, IDLParserContext("client fle options"));

    auto service = _factories.at(provider)->create(config);
    uassert(51192, str::stream() << "Cannot find client kms provider " << kmsProvider, service);
    return service;
}

std::unique_ptr<KMSService> KMSServiceController::createFromDisk(const BSONObj& config,
                                                                 const BSONObj& masterKey) {
    auto providerObj = masterKey.getStringField("provider"_sd);
    auto provider = KMSProvider_parse(providerObj, IDLParserContext("root"));
    auto service = _factories.at(provider)->create(config);
    uassert(51193, str::stream() << "Cannot find disk kms provider " << providerObj, service);
    return service;
}

}  // namespace mongo
