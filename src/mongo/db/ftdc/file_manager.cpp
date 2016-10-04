/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/file_manager.h"

#include <boost/filesystem.hpp>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/jsobj.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

FTDCFileManager::FTDCFileManager(const FTDCConfig* config,
                                 const boost::filesystem::path& path,
                                 FTDCCollectorCollection* collection)
    : _config(config), _writer(_config), _path(path), _rotateCollectors(collection) {}

FTDCFileManager::~FTDCFileManager() {
    close();
}

StatusWith<std::unique_ptr<FTDCFileManager>> FTDCFileManager::create(
    const FTDCConfig* config,
    const boost::filesystem::path& path,
    FTDCCollectorCollection* collection,
    Client* client) {
    const boost::filesystem::path dir = boost::filesystem::absolute(path);

    if (!boost::filesystem::exists(dir)) {
        // Create the directory
        boost::system::error_code ec;
        boost::filesystem::create_directories(dir, ec);
        if (ec) {
            return {ErrorCodes::NonExistentPath,
                    str::stream() << "\'" << dir.generic_string() << "\' could not be created: "
                                  << ec.message()};
        }
    }

    auto mgr =
        std::unique_ptr<FTDCFileManager>(new FTDCFileManager(config, dir, std::move(collection)));

    // Enumerate the metrics files
    auto files = mgr->scanDirectory();

    // Recover the interim file
    auto interimDocs = mgr->recoverInterimFile();

    // Open the archive file for writing
    auto swFile = mgr->generateArchiveFileName(path, terseUTCCurrentTime());
    if (!swFile.isOK()) {
        return swFile.getStatus();
    }

    Status s = mgr->openArchiveFile(client, swFile.getValue(), interimDocs);
    if (!s.isOK()) {
        return s;
    }

    // Rotate as needed after we appended interim data to the archive file
    mgr->trimDirectory(files);

    return {std::move(mgr)};
}

std::vector<boost::filesystem::path> FTDCFileManager::scanDirectory() {
    std::vector<boost::filesystem::path> files;

    boost::filesystem::directory_iterator di(_path);
    for (; di != boost::filesystem::directory_iterator(); di++) {
        boost::filesystem::directory_entry& de = *di;
        auto filename = de.path().filename();

        std::string str = filename.generic_string();
        if (str.compare(0, strlen(kFTDCArchiveFile), kFTDCArchiveFile) == 0 &&
            str != kFTDCInterimFile) {
            files.emplace_back(_path / filename);
        }
    }

    std::sort(files.begin(), files.end());

    return files;
}

StatusWith<boost::filesystem::path> FTDCFileManager::generateArchiveFileName(
    const boost::filesystem::path& path, StringData suffix) {
    auto fileName = path;
    fileName /= std::string(kFTDCArchiveFile);
    fileName += std::string(".");
    fileName += suffix.toString();

    if (_previousArchiveFileSuffix != suffix) {
        // If the suffix has changed, reset the uniquifier counter to zero
        _previousArchiveFileSuffix = suffix.toString();
        _fileNameUniquifier = 0;
    }

    if (boost::filesystem::exists(path)) {
        for (; _fileNameUniquifier < FTDCConfig::kMaxFileUniqifier; ++_fileNameUniquifier) {
            char buf[20];

            // Use leading zeros so the numbers sort lexigraphically
            int ret = snprintf(&buf[0], sizeof(buf), "%05u", _fileNameUniquifier);
            invariant(ret > 0 && ret < static_cast<int>((sizeof(buf) - 1)));

            auto fileNameUnique = fileName;
            fileNameUnique += std::string("-") + &buf[0];

            if (!boost::filesystem::exists(fileNameUnique)) {
                return fileNameUnique;
            }
        }

        return {ErrorCodes::InvalidPath,
                "Maximum limit reached for FTDC files in a second. The maximum file uniqifier has "
                "been reached."};
    }

    return {fileName};
}

Status FTDCFileManager::openArchiveFile(
    Client* client,
    const boost::filesystem::path& path,
    const std::vector<std::tuple<FTDCBSONUtil::FTDCType, BSONObj, Date_t>>& docs) {
    auto sOpen = _writer.open(path);
    if (!sOpen.isOK()) {
        return sOpen;
    }

    // Append any old interim records
    for (auto& triplet : docs) {
        if (std::get<0>(triplet) == FTDCBSONUtil::FTDCType::kMetadata) {
            Status s = _writer.writeMetadata(std::get<1>(triplet), std::get<2>(triplet));

            if (!s.isOK()) {
                return s;
            }
        } else {
            Status s = _writer.writeSample(std::get<1>(triplet), std::get<2>(triplet));

            if (!s.isOK()) {
                return s;
            }
        }
    }

    // After the system restarts or a new file has been started,
    // collect one-time information
    // This is appened after the file is opened to ensure a user can determine which bson objects
    // where collected from which server instance.
    auto sample = _rotateCollectors->collect(client);
    if (!std::get<0>(sample).isEmpty()) {
        Status s = _writer.writeMetadata(std::get<0>(sample), std::get<1>(sample));

        if (!s.isOK()) {
            return s;
        }
    }

    return Status::OK();
}

void FTDCFileManager::trimDirectory(std::vector<boost::filesystem::path>& files) {
    std::uint64_t maxSize = _config->maxDirectorySizeBytes;
    std::uint64_t size = 0;

    dassert(std::is_sorted(files.begin(), files.end()));

    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        std::uint64_t fileSize = boost::filesystem::file_size(*it);
        size += fileSize;

        if (size >= maxSize) {
            LOG(1) << "Cleaning file over full-time diagnostic data capture quota, file: "
                   << (*it).generic_string() << " with size " << fileSize;
            boost::filesystem::remove(*it);
        }
    }
}

std::vector<std::tuple<FTDCBSONUtil::FTDCType, BSONObj, Date_t>>
FTDCFileManager::recoverInterimFile() {
    decltype(recoverInterimFile()) docs;

    auto interimFile = FTDCUtil::getInterimFile(_path);

    // Nothing to do if it does not exist
    if (!boost::filesystem::exists(interimFile)) {
        return docs;
    }

    size_t size = boost::filesystem::file_size(interimFile);
    if (size == 0) {
        return docs;
    }

    FTDCFileReader read;
    auto s = read.open(interimFile);
    if (!s.isOK()) {
        log() << "Unclean full-time diagnostic data capture shutdown detected, found interim file, "
                 "but failed "
                 "to open it, some "
                 "metrics may have been lost. "
              << s;

        // Note: We ignore any actual errors as reading from the interim files is a best-effort
        return docs;
    }

    StatusWith<bool> m = read.hasNext();
    for (; m.isOK() && m.getValue(); m = read.hasNext()) {
        auto triplet = read.next();
        docs.emplace_back(std::tuple<FTDCBSONUtil::FTDCType, BSONObj, Date_t>(
            std::get<0>(triplet), std::get<1>(triplet).getOwned(), std::get<2>(triplet)));
    }

    // Warn if the interim file was corrupt or we had an unclean shutdown
    if (!m.isOK() || !docs.empty()) {
        log() << "Unclean full-time diagnostic data capture shutdown detected, found interim file, "
                 "some "
                 "metrics may have been lost. "
              << m.getStatus();
    }

    // Note: We ignore any actual errors as reading from the interim files is a best-effort
    return docs;
}

Status FTDCFileManager::rotate(Client* client) {
    auto s = _writer.close();
    if (!s.isOK()) {
        return s;
    }

    auto files = scanDirectory();

    // Rotate as needed
    trimDirectory(files);

    auto swFile = generateArchiveFileName(_path, terseUTCCurrentTime());
    if (!swFile.isOK()) {
        return swFile.getStatus();
    }

    return openArchiveFile(client, swFile.getValue(), {});
}

Status FTDCFileManager::writeSampleAndRotateIfNeeded(Client* client,
                                                     const BSONObj& sample,
                                                     Date_t date) {
    Status s = _writer.writeSample(sample, date);

    if (!s.isOK()) {
        return s;
    }

    if (_writer.getSize() > _config->maxFileSizeBytes) {
        return rotate(client);
    }

    return Status::OK();
}

Status FTDCFileManager::close() {
    return _writer.close();
}

}  // namespace mongo
