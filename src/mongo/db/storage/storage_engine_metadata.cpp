// storage_engine_metadata.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_metadata.h"

#include <cstdio>
#include <boost/filesystem.hpp>
#include <fstream>
#include <limits>
#include <ostream>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

    const std::string kMetadataBasename = "storage.bson";

}  // namespace

    // static
    void StorageEngineMetadata::validate(const std::string& dbpath,
                                         const std::string& storageEngine) {
        if (boost::filesystem::is_empty(dbpath)) {
            return;
        }

        StorageEngineMetadata metadata(dbpath);
        Status status = metadata.read();
        std::string previousStorageEngine;
        if (status.isOK()) {
            previousStorageEngine = metadata.getStorageEngine();
        }
        else if (status.code() == ErrorCodes::NonExistentPath) {
            previousStorageEngine = "mmapv1";
        }
        else {
            // The storage metadata file is present but there was an issue
            // reading its contents.
            severe() << "Unable to verify the storage engine";
            uassertStatusOK(status);
        }
        uassert(28574, str::stream()
            << "Cannot start server. Detected data files in " << dbpath
            << " created by storage engine '" << previousStorageEngine
            << "'. The configured storage engine is '" << storageEngine << "'.",
            previousStorageEngine == storageEngine);
    }

    // static
    Status StorageEngineMetadata::updateIfMissing(const std::string& dbpath,
                                                  const std::string& storageEngine) {
        boost::filesystem::path metadataPath =
            boost::filesystem::path(dbpath) / kMetadataBasename;

        if (boost::filesystem::exists(metadataPath)) {
            return Status::OK();
        }

        StorageEngineMetadata metadata(dbpath);
        metadata.setStorageEngine(storageEngine);
        Status status = metadata.write();
        if (!status.isOK()) {
            warning() << "Unable to update storage engine metadata in " << dbpath
                      << ": " << status.toString();
        }
        return status;
    }

    StorageEngineMetadata::StorageEngineMetadata(const std::string& dbpath)
        : _dbpath(dbpath) {
        reset();
    }

    StorageEngineMetadata::~StorageEngineMetadata() { }

    void StorageEngineMetadata::reset() {
        _storageEngine.clear();
    }

    const std::string& StorageEngineMetadata::getStorageEngine() const {
        return _storageEngine;
    }

    void StorageEngineMetadata::setStorageEngine(const std::string& storageEngine) {
        _storageEngine = storageEngine;
    }

    Status StorageEngineMetadata::read() {
        reset();

        boost::filesystem::path metadataPath =
            boost::filesystem::path(_dbpath) / kMetadataBasename;

        if (!boost::filesystem::exists(metadataPath)) {
            return Status(ErrorCodes::NonExistentPath, str::stream()
                << "Metadata file " << metadataPath.string() << " not found.");
        }

        boost::uintmax_t fileSize = boost::filesystem::file_size(metadataPath);
        if (fileSize == 0) {
            return Status(ErrorCodes::InvalidPath, str::stream()
                << "Metadata file " << metadataPath.string() << " cannot be empty.");
        }
        if (fileSize == static_cast<boost::uintmax_t>(-1)) {
            return Status(ErrorCodes::InvalidPath, str::stream()
                << "Unable to determine size of metadata file " << metadataPath.string());
        }

        std::vector<char> buffer(fileSize);
        std::string filename = metadataPath.string();
        try {
            std::ifstream ifs(filename.c_str(), std::ios_base::in | std::ios_base::binary);
            if (!ifs) {
                return Status(ErrorCodes::FileNotOpen, str::stream()
                    << "Failed to read metadata from " << filename);
        }

        // Read BSON from file
        ifs.read(&buffer[0], buffer.size());
        if (!ifs) {
            return Status(ErrorCodes::FileStreamFailed, str::stream()
                << "Unable to read BSON data from " << filename);
        }
        }
        catch (const std::exception& ex) {
            return Status(ErrorCodes::FileStreamFailed, str::stream()
                << "Unexpected error reading BSON data from " << filename
                << ": " << ex.what());
        }

        BSONObj obj;
        try {
            obj = BSONObj(&buffer[0]);
        }
        catch (DBException& ex) {
            return Status(ErrorCodes::FailedToParse, str::stream()
                          << "Failed to convert data in " << filename
                          << " to BSON: " << ex.what());
        }

        // Validate 'storage.engine' field.
        BSONElement storageEngineElement = obj.getFieldDotted("storage.engine");
        if (storageEngineElement.type() != mongo::String) {
            return Status(ErrorCodes::FailedToParse, str::stream()
                          << "The 'storage.engine' field in metadata must be a string: "
                          << storageEngineElement.toString());
        }

        // Extract storage engine name from 'storage.engine' node.
        std::string storageEngine = storageEngineElement.String();
        if (storageEngine.empty()) {
            return Status(ErrorCodes::FailedToParse,
                          "The 'storage.engine' field in metadata cannot be empty string.");
        }
        _storageEngine = storageEngine;
        return Status::OK();
    }

    Status StorageEngineMetadata::write() const {
        if (_storageEngine.empty()) {
            return Status(ErrorCodes::BadValue,
                          "Cannot write empty storage engine name to metadata file.");
        }

        boost::filesystem::path metadataPath =
            boost::filesystem::path(_dbpath) / kMetadataBasename;
        std::string filenameTemp = metadataPath.string() + ".tmp";
        try {
            std::ofstream ofs(filenameTemp.c_str(), std::ios_base::out | std::ios_base::binary);
            if (!ofs) {
                return Status(ErrorCodes::FileNotOpen, str::stream()
                    << "Failed to write metadata to " << filenameTemp);
            }

            BSONObj obj = BSON("storage" << BSON("engine" << _storageEngine));
            ofs.write(obj.objdata(), obj.objsize());
            if (!ofs) {
                return Status(ErrorCodes::InternalError, str::stream()
                    << "Failed to write BSON data to " << filenameTemp);
            }
            ofs.flush();
        }
        catch (const std::exception& ex) {
            return Status(ErrorCodes::InternalError, str::stream()
                << "Unexpected error while writing metadata to " << filenameTemp
                << ": " << ex.what());
        }

        // Rename temp file to actual metadata file.
        // Removing original metafile first
        std::string filename = metadataPath.string();
        ::remove(filename.c_str());
        if (::rename(filenameTemp.c_str(), filename.c_str())) {
            return Status(ErrorCodes::FileRenameFailed, str::stream()
                << "Failed to rename " << filename << " to " << filenameTemp
                << ": " << errnoWithDescription());
        }

        return Status::OK();
    }

}  // namespace mongo
