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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"

#include <memory>
#include <string>

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
    Status validateStorageEngineOption(StringData fieldName,
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
