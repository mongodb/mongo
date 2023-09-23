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


#include <boost/filesystem/operations.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>  // IWYU pragma: keep
#include <utility>

#include <boost/filesystem/path.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


using std::ios_base;
using std::ofstream;
using std::string;
using std::stringstream;

namespace mongo {

RemoveSaver::RemoveSaver(const string& a,
                         const string& b,
                         const string& why,
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

    stringstream ss;
    ss << why << "." << terseCurrentTimeForFilename() << "." << NUM++ << ".bson";
    _file /= ss.str();

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

        size_t resultLen;
        Status status = _protector->finalize(protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            LOGV2_FATAL(34350,
                        "Unable to finalize DataProtector while closing RemoveSaver: {error}",
                        "Unable to finalize DataProtector while closing RemoveSaver",
                        "error"_attr = redact(status));
        }

        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), resultLen);
        if (_out->fail()) {
            auto ec = lastSystemError();
            LOGV2_FATAL(34351,
                        "Couldn't write finalized DataProtector data to: {file} for remove "
                        "saving: {error}",
                        "Couldn't write finalized DataProtector for remove saving",
                        "file"_attr = _file.generic_string(),
                        "error"_attr = redact(errorMessage(ec)));
        }

        protectedBuffer.reset(new uint8_t[protectedSizeMax]);
        status = _protector->finalizeTag(protectedBuffer.get(), protectedSizeMax, &resultLen);
        if (!status.isOK()) {
            LOGV2_FATAL(
                34352,
                "Unable to get finalizeTag from DataProtector while closing RemoveSaver: {error}",
                "Unable to get finalizeTag from DataProtector while closing RemoveSaver",
                "error"_attr = redact(status));
        }

        if (resultLen != _protector->getNumberOfBytesReservedForTag()) {
            LOGV2_FATAL(34353,
                        "Attempted to write tag of size {sizeBytes} when DataProtector only "
                        "reserved {reservedBytes} bytes",
                        "Attempted to write tag of larger size than DataProtector reserved size",
                        "sizeBytes"_attr = resultLen,
                        "reservedBytes"_attr = _protector->getNumberOfBytesReservedForTag());
        }

        _out->seekp(0);
        _out->write(reinterpret_cast<const char*>(protectedBuffer.get()), resultLen);

        if (_out->fail()) {
            auto ec = lastSystemError();
            LOGV2_FATAL(34354,
                        "Couldn't write finalizeTag from DataProtector to: {file} for "
                        "remove saving: {error}",
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
            string msg = str::stream() << "couldn't create file: " << _file.string()
                                       << " for remove saving: " << redact(errorMessage(ec));
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

        size_t resultLen;
        Status status = _protector->protect(
            data, dataSize, protectedBuffer.get(), protectedSizeMax, &resultLen);

        if (!status.isOK()) {
            return status;
        }

        data = protectedBuffer.get();
        dataSize = resultLen;
    }

    _out->write(reinterpret_cast<const char*>(data), dataSize);

    if (_out->fail()) {
        auto errorStr = redact(errorMessage(lastSystemError()));
        string msg = str::stream() << "couldn't write document to file: " << _file.string()
                                   << " for remove saving: " << errorStr;
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
