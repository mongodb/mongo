// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/util/modules.h"

#include <memory>
#include <ostream>
#include <string>

#include <boost/filesystem/path.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This class provides facility for saving bson objects to a flat file. The common use case is for
 * making a back-up copy of a document before an internal operation (like migration or rollback)
 * deletes it. To use this correctly, the caller must call goingToDelete first before the actual
 * deletion, otherwise the document will be lost if the process gets terminated in between.
 */
class RemoveSaver {
    RemoveSaver(const RemoveSaver&) = delete;
    RemoveSaver& operator=(const RemoveSaver&) = delete;

public:
    class Storage;
    RemoveSaver(const std::string& type,
                const std::string& ns,
                const std::string& why,
                std::unique_ptr<Storage> storage = std::make_unique<Storage>());
    ~RemoveSaver();

    /**
     * Writes document to file. File is created lazily before writing the first document.
     * Returns error status if the file could not be created or if there were errors writing
     * to the file.
     */
    Status goingToDelete(const BSONObj& o);

    /**
     * A path object describing the directory containing the file with deleted documents.
     */
    const auto& root() const& {
        return _root;
    }
    void root() && = delete;

    /**
     * A path object describing the actual file containing BSON documents.
     */
    const auto& file() const& {
        return _file;
    }
    void file() && = delete;

    class [[MONGO_MOD_OPEN]] Storage {
    public:
        virtual ~Storage() = default;
        virtual std::unique_ptr<std::ostream> makeOstream(const boost::filesystem::path& file,
                                                          const boost::filesystem::path& root);
        virtual void dumpBuffer() {}
    };

private:
    boost::filesystem::path _root;
    boost::filesystem::path _file;
    std::unique_ptr<DataProtector> _protector;
    std::unique_ptr<std::ostream> _out;
    std::unique_ptr<Storage> _storage;
};

}  // namespace mongo
