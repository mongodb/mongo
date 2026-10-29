// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/encryption_hooks.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/service_context.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/util/decorable.h"

#include <memory>
#include <utility>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

struct EncryptionHooksHolder {
    std::unique_ptr<EncryptionHooks> ptr = std::make_unique<EncryptionHooks>();
};

const auto getEncryptionHooks = ServiceContext::declareDecoration<EncryptionHooksHolder>();
}  // namespace

void EncryptionHooks::set(ServiceContext* service, std::unique_ptr<EncryptionHooks> custHooks) {
    getEncryptionHooks(service).ptr = std::move(custHooks);
}

EncryptionHooks* EncryptionHooks::get(ServiceContext* service) {
    return getEncryptionHooks(service).ptr.get();
}

EncryptionHooks::~EncryptionHooks() {}

bool EncryptionHooks::enabled() const {
    return false;
}

bool EncryptionHooks::restartRequired() {
    return false;
}

std::unique_ptr<DataProtector> EncryptionHooks::getDataProtector() {
    return std::unique_ptr<DataProtector>();
}

boost::filesystem::path EncryptionHooks::getProtectedPathSuffix() {
    return "";
}

Status EncryptionHooks::protectTmpData(ConstDataRange in,
                                       DataRange* out,
                                       boost::optional<DatabaseName> dbName) {
    return Status(ErrorCodes::InternalError,
                  "Encryption hooks must be enabled to use preprocessTmpData.");
}

Status EncryptionHooks::unprotectTmpData(ConstDataRange in,
                                         DataRange* out,
                                         boost::optional<DatabaseName> dbName) {
    return Status(ErrorCodes::InternalError,
                  "Encryption hooks must be enabled to use postprocessTmpData.");
}

StatusWith<std::vector<std::string>> EncryptionHooks::beginNonBlockingBackup() {
    return std::vector<std::string>();
}

Status EncryptionHooks::endNonBlockingBackup() {
    return Status::OK();
}
}  // namespace mongo
