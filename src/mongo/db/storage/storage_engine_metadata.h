// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/filesystem/path.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This reads and write the storage engine metadata file 'storage.bson'
 * in the data directory (See --dbpath).
 * 'storage.engine' is the only mandatory field in the BSON metadata file.
 * Fields other than 'storage.engine' are ignored.
 */
class StorageEngineMetadata {
    StorageEngineMetadata(const StorageEngineMetadata&) = delete;
    StorageEngineMetadata& operator=(const StorageEngineMetadata&) = delete;

public:
    /**
     * Returns a metadata object describing the storage engine that backs the data files
     * contained in 'dbpath', and nullptr otherwise.
     */
    static std::unique_ptr<StorageEngineMetadata> forPath(const std::string& dbpath);

    /**
     * Returns the name of the storage engine that backs the data files contained in 'dbpath',
     * and none otherwise.
     */
    static boost::optional<std::string> getStorageEngineForPath(const std::string& dbpath);

    /**
     * Sets fields to defaults.
     * Use read() load metadata from file.
     */
    StorageEngineMetadata(const std::string& dbpath);

    virtual ~StorageEngineMetadata();

    /**
     * Returns name of storage engine in metadata.
     */
    const std::string& getStorageEngine() const;

    /**
     * Returns storage engine options in metadata.
     */
    const BSONObj& getStorageEngineOptions() const;

    /**
     * Sets name of storage engine in metadata.
     */
    void setStorageEngine(const std::string& storageEngine);

    /**
     * Sets storage engine options in metadata.
     */
    void setStorageEngineOptions(const BSONObj& storageEngineOptions);

    /**
     * Resets fields to default values.
     */
    void reset();

    /**
     * Reads metadata from 'storage.bson' in 'dbpath' directory.
     */
    Status read();

    /**
     * Writes metadata to file.
     */
    Status write() const;

    /**
     * Validates a single field in the storage engine options. Currently, only boolean fields are
     * supported. If the 'fieldName' does not exist in the 'storage.bson' file and a
     * 'defaultValue' is passed in, the 'expectedValue' must match the 'defaultValue'.
     */
    template <typename T>
    Status validateStorageEngineOption(std::string_view fieldName,
                                       T expectedValue,
                                       boost::optional<T> defaultValue = boost::none) const;

private:
    std::string _dbpath;
    std::string _storageEngine;
    BSONObj _storageEngineOptions;
};

bool fsyncFile(boost::filesystem::path path);
void flushMyDirectory(const boost::filesystem::path& file);

}  // namespace mongo
