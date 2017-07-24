/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {

/**
 * This class represents the layout and content of a batched insert runCommand,
 * the request side.
 */
class BatchedInsertRequest {
    MONGO_DISALLOW_COPYING(BatchedInsertRequest);

public:
    static const BSONField<std::string> collName;
    static const BSONField<std::vector<BSONObj>> documents;

    //
    // construction / destruction
    //

    BatchedInsertRequest();

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(BatchedInsertRequest* other) const;

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    void parseRequest(const OpMsgRequest& request);
    void clear();
    std::string toString() const;

    //
    // individual field accessors
    //

    void setNS(NamespaceString collName);
    const NamespaceString& getNS() const;

    void addToDocuments(const BSONObj& documents);
    std::size_t sizeDocuments() const;
    const std::vector<BSONObj>& getDocuments() const;
    const BSONObj& getDocumentsAt(std::size_t pos) const;
    void setDocumentAt(std::size_t pos, const BSONObj& doc);

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  collection we're inserting on
    NamespaceString _ns;
    bool _isNSSet;

    // (M)  array of documents to be inserted
    std::vector<BSONObj> _documents;
    bool _isDocumentsSet;
};

}  // namespace mongo
