// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/remove_saver.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

RemoveSaver::RemoveSaver(const std::string& a,
                         const std::string& b,
                         const std::string& why,
                         std::unique_ptr<Storage> storage)
    : _storage(std::move(storage)) {
    static int NUM = 0;

    _root = storageGlobalParams.dbpath;
    if (a.size())
        _root /= a;
    if (b.size())
        _root /= b;
    MONGO_verify(a.size() || b.size());

    _file = _root;
    _file /= fmt::format("{}.{}.{}.bson", why, terseCurrentTimeForFilename(), NUM++);

    auto encryptionHooks = EncryptionHooks::get(getGlobalServiceContext());
    if (encryptionHooks->enabled()) {
        _protector = encryptionHooks->getDataProtector();
        _file += encryptionHooks->getProtectedPathSuffix();
    }
}

RemoveSaver::~RemoveSaver() {
    if (_protector && _out) {
        auto encryptionHooks = EncryptionHooks::get(getGlobalServiceContext());
        invariant(encryptionHooks->enabled());

        size_t protectedSizeMax = encryptionHooks->additionalBytesForProtectedBuffer();
        std::unique_ptr<uint8_t[]> protectedBuffer(new uint8_t[protectedSizeMax]);
        DataRange outRange(protectedBuffer.get(), protectedSizeMax);

        Status status = _protector->finalize(&outRange);
        if (!status.isOK()) {
            LOGV2_FATAL(34350,
                        "Unable to finalize DataProtector while closing RemoveSaver",
                        "error"_attr = redact(status));
        }

        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), outRange.length());
        if (_out->fail()) {
            auto ec = lastSystemError();
            LOGV2_FATAL(34351,
                        "Couldn't write finalized DataProtector for remove saving",
                        "file"_attr = _file.generic_string(),
                        "error"_attr = redact(errorMessage(ec)));
        }

        protectedBuffer.reset(new uint8_t[protectedSizeMax]);
        outRange = DataRange(protectedBuffer.get(), protectedSizeMax);
        status = _protector->finalizeTag(&outRange);
        if (!status.isOK()) {
            LOGV2_FATAL(34352,
                        "Unable to get finalizeTag from DataProtector while closing RemoveSaver",
                        "error"_attr = redact(status));
        }

        if (outRange.length() != _protector->getNumberOfBytesReservedForTag()) {
            LOGV2_FATAL(34353,
                        "Attempted to write tag of larger size than DataProtector reserved size",
                        "sizeBytes"_attr = outRange.length(),
                        "reservedBytes"_attr = _protector->getNumberOfBytesReservedForTag());
        }

        _out->seekp(0);
        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), outRange.length());

        if (_out->fail()) {
            auto ec = lastSystemError();
            LOGV2_FATAL(34354,
                        "Couldn't write finalizeTag from DataProtector for remove saving",
                        "file"_attr = _file.generic_string(),
                        "error"_attr = redact(errorMessage(ec)));
        }

        _storage->dumpBuffer();
    }
}

Status RemoveSaver::goingToDelete(const BSONObj& o) {
    if (!_out) {
        _out = _storage->makeOstream(_file, _root);

        if (_out->fail()) {
            auto ec = lastSystemError();
            std::string msg = fmt::format("couldn't create file: {} for remove saving: {}",
                                          _file.string(),
                                          redact(errorMessage(ec)));
            LOGV2_ERROR(23734,
                        "Failed to create file for remove saving",
                        "file"_attr = _file.generic_string(),
                        "error"_attr = redact(errorMessage(ec)));
            _out.reset();
            _out = nullptr;
            return Status(ErrorCodes::FileNotOpen, msg);
        }
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(o.objdata());
    size_t dataSize = o.objsize();

    std::unique_ptr<uint8_t[]> protectedBuffer;
    if (_protector) {
        auto encryptionHooks = EncryptionHooks::get(getGlobalServiceContext());
        invariant(encryptionHooks->enabled());

        size_t protectedSizeMax = dataSize + encryptionHooks->additionalBytesForProtectedBuffer();
        protectedBuffer.reset(new uint8_t[protectedSizeMax]);

        DataRange outRange(protectedBuffer.get(), protectedSizeMax);
        Status status = _protector->protect(ConstDataRange(data, dataSize), &outRange);

        if (!status.isOK()) {
            return status;
        }

        data = protectedBuffer.get();
        dataSize = outRange.length();
    }

    _out->write(reinterpret_cast<const char*>(data), dataSize);

    if (_out->fail()) {
        auto errorStr = redact(errorMessage(lastSystemError()));
        std::string msg = fmt::format(
            "couldn't write document to file: {} for remove saving: {}", _file.string(), errorStr);
        LOGV2_ERROR(23735,
                    "Couldn't write document to file for remove saving",
                    "file"_attr = _file.generic_string(),
                    "error"_attr = errorStr);
        return Status(ErrorCodes::OperationFailed, msg);
    }

    return Status::OK();
}

std::unique_ptr<std::ostream> RemoveSaver::Storage::makeOstream(
    const boost::filesystem::path& file, const boost::filesystem::path& root) {
    // We don't expect to ever pass "" to create_directories below, but catch
    // this anyway as per SERVER-26412.
    invariant(!root.empty());
    boost::filesystem::create_directories(root);
    return std::make_unique<std::ofstream>(file.string().c_str(),
                                           std::ios_base::out | std::ios_base::binary);
}
}  // namespace mongo
