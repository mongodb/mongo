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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/database_name.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace boost {
namespace filesystem {
class path;
}  // namespace filesystem
}  // namespace boost

namespace mongo {
class DataProtector;
class ServiceContext;

class EncryptionHooks {
public:
    static void set(ServiceContext* service, std::unique_ptr<EncryptionHooks> custHooks);

    static EncryptionHooks* get(ServiceContext* service);

    virtual ~EncryptionHooks();

    /**
     * Returns true if the encryption hooks are enabled.
     */
    virtual bool enabled() const;

    /**
     * Perform any encryption engine initialization/sanity checking that needs to happen after
     * storage engine initialization but before the server starts accepting incoming connections.
     *
     * Returns true if the server needs to be rebooted because of configuration changes.
     */
    virtual bool restartRequired();

    /**
     * Returns the maximum size addition when doing transforming temp data.
     */
    size_t additionalBytesForProtectedBuffer() {
        return 33;
    }

    /**
     * Get the data protector object
     */
    virtual std::unique_ptr<DataProtector> getDataProtector();

    /**
     * Get an implementation specific path suffix to tag files with
     */
    virtual boost::filesystem::path getProtectedPathSuffix();

    /**
     * Transform temporary data that has been spilled to disk into non-readable form. If dbName
     * is specified, the database key corresponding to dbName will be used to encrypt the data.
     * This key is persistent across process restarts. Otherwise, an ephemeral key that is only
     * consistent for the duration of the process will be generated and used for encryption.
     */
    virtual Status protectTmpData(ConstDataRange in,
                                  DataRange* out,
                                  boost::optional<DatabaseName> dbName);

    /**
     * Transform temporary data that has been spilled to disk back into readable form. If dbName
     * is specified, the database key corresponding to dbName will be used to decrypt the data.
     * This key is persistent across process restarts, so decryption will be successful even if a
     * restart had occurred after encryption. Otherwise, an ephemeral key that can only decrypt data
     * encrypted earlier in the current process's lifetime will be used.
     */
    virtual Status unprotectTmpData(ConstDataRange in,
                                    DataRange* out,
                                    boost::optional<DatabaseName> dbName);

    /**
     * Inform the encryption storage system to prepare its data such that its files can be copied
     * along with MongoDB data files for a backup.
     */
    virtual StatusWith<std::vector<std::string>> beginNonBlockingBackup();

    /**
     * Inform the encryption storage system that it can release resources associated with a
     * previous call to `beginNonBlockingBackup`. This function may be called without a pairing
     * `beginNonBlockingBackup`. In that case it must return `Status::OK()`;
     */
    virtual Status endNonBlockingBackup();
};

}  // namespace mongo
