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

#include <boost/filesystem/path.hpp>
#include <memory>
#include <tuple>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/file_writer.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class Client;

/**
 * Manages a directory full of archive files, and an interim file.
 *
 * Manages file rotation, and directory size management.
 */
class FTDCFileManager {
    FTDCFileManager(const FTDCFileManager&) = delete;
    FTDCFileManager& operator=(const FTDCFileManager&) = delete;

public:
    ~FTDCFileManager();

    /**
     * Creates the directory if it does not exist.
     * NOTE: This must be run on a thread with a Client context, i.e., not a static initializer.
     *
     * Collectors are used to collect data to be stored as metadata on file rotation or system
     * restart.
     *
     * Recovers data from the interim file as needed.
     * Rotates files if needed.
     */
    static StatusWith<std::unique_ptr<FTDCFileManager>> create(const FTDCConfig* config,
                                                               const boost::filesystem::path& path,
                                                               FTDCCollectorCollection* collection,
                                                               Client* client);

    /**
     * Rotates files
     */
    Status rotate(Client* client);

    /**
     * Writes a sample to disk via FTDCFileWriter.
     *
     * Rotates files as needed.
     */
    Status writeSampleAndRotateIfNeeded(Client* client, const BSONObj& sample, Date_t date);

    /**
     * Closes the current file manager down.
     */
    Status close();

public:
    /**
     * Generate a new file name for the archive.
     * Public for use by unit tests only.
     */
    StatusWith<boost::filesystem::path> generateArchiveFileName(const boost::filesystem::path& path,
                                                                StringData suffix);

private:
    FTDCFileManager(const FTDCConfig* config,
                    const boost::filesystem::path& path,
                    FTDCCollectorCollection* collection);

    /**
     * Gets a list of metrics files in a directory.
     */
    std::vector<boost::filesystem::path> scanDirectory();

    /**
     * Recover the interim file.
     *
     * Checks if the file is non-empty, and if gets a list of documents with the original times they
     * were written disk based on the _id fields.
     */
    std::vector<std::tuple<FTDCBSONUtil::FTDCType, BSONObj, Date_t>> recoverInterimFile();

    /**
     * Removes the oldest files if the directory is over quota
     */
    Status trimDirectory(std::vector<boost::filesystem::path>& files);

    /**
     * Open a new file for writing.
     *
     * Steps:
     * 1. Writes any recovered interim file samples into the file. These entries are written to the
     *    archive file with the time they were written to the interim file instead of the time this
     *    recovery is written.
     * 2. Appends file rotation collectors upon opening the file.
     */
    Status openArchiveFile(
        Client* client,
        const boost::filesystem::path& path,
        const std::vector<std::tuple<FTDCBSONUtil::FTDCType, BSONObj, Date_t>>& docs);

private:
    // config to use
    const FTDCConfig* const _config;

    // file to log samples to
    FTDCFileWriter _writer;

    // last archive file name suffix used
    std::string _previousArchiveFileSuffix;

    // last file name id uniquifier used
    // this starts from zero for each new file suffix
    std::uint32_t _fileNameUniquifier = 0;

    // Path of metrics directory
    boost::filesystem::path _path;

    // collection of collectors to add to new files on rotation, and server restart
    FTDCCollectorCollection* const _rotateCollectors;
};

}  // namespace mongo
