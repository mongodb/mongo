// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/fle_util.h"

#include "mongo/db/service_context.h"

extern "C" {
#include <mongocrypt-private.h>
#include <mongocrypt.h>

#include <bson/bson.h>
}
namespace mongo {

namespace {

const auto getMongoCrypt = ServiceContext::declareDecoration<
    libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_t, mongocrypt_destroy>>();

ServiceContext::ConstructorActionRegisterer mongoCryptRegisterer(
    "mongocrypt", [](ServiceContext* svcCtx) {
        getMongoCrypt(svcCtx) =
            libmongocrypt_support_detail::libmongocrypt_unique_ptr<mongocrypt_t,
                                                                   mongocrypt_destroy>(
                mongocrypt_new());
    });

}  // namespace

mongocrypt_t* getGlobalMongoCrypt() {
    return getMongoCrypt(getGlobalServiceContext()).get();
}

}  // namespace mongo
