/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

/**
 * The base structure for all fields that are common for all write operations.
 *
 * Unlike ParsedUpdate and UpdateRequest (and the Delete counterparts), types deriving from this are
 * intended to represent entire operations that may consist of multiple sub-operations.
 */
struct ParsedWriteOp {
    NamespaceString ns;
    bool bypassDocumentValidation = false;
    bool continueOnError = false;
};

/**
 * A parsed insert insert operation.
 */
struct InsertOp : ParsedWriteOp {
    std::vector<BSONObj> documents;
};

/**
 * A parsed update operation.
 */
struct UpdateOp : ParsedWriteOp {
    struct SingleUpdate {
        BSONObj query;
        BSONObj update;
        BSONObj collation;
        bool multi = false;
        bool upsert = false;
    };

    std::vector<SingleUpdate> updates;
};

/**
 * A parsed Delete operation.
 */
struct DeleteOp : ParsedWriteOp {
    struct SingleDelete {
        BSONObj query;
        BSONObj collation;
        bool multi = true;
    };

    std::vector<SingleDelete> deletes;
};

}  // namespace mongo
