
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
#include <ostream>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/data_protector.h"

namespace mongo {

/**
 * This class provides facility for saving bson objects to a flat file. The common use case is for
 * making a back-up copy of a document before an internal operation (like migration or rollback)
 * deletes it. To use this correctly, the caller must call goingToDelete first before the actual
 * deletion, otherwise the document will be lost if the process gets terminated in between.
 */
class RemoveSaver {
    MONGO_DISALLOW_COPYING(RemoveSaver);

public:
    RemoveSaver(const std::string& type, const std::string& ns, const std::string& why);
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

private:
    boost::filesystem::path _root;
    boost::filesystem::path _file;
    std::unique_ptr<DataProtector> _protector;
    std::unique_ptr<std::ostream> _out;
};

}  // namespace mongo
