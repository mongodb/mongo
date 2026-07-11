// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class [[MONGO_MOD_OPEN]] EncryptionHooks {
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
