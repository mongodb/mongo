/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/getmore_request.h"

namespace mongo {

    const int GetMoreRequest::kDefaultBatchSize = 101;

    GetMoreRequest::GetMoreRequest()
        : cursorid(0),
          batchSize(0) { }

    GetMoreRequest::GetMoreRequest(const std::string& fullns, CursorId id, int sizeOfBatch)
        : nss(fullns),
          cursorid(id),
          batchSize(sizeOfBatch) { }

    Status GetMoreRequest::isValid() const {
        if (!nss.isValid()) {
            return Status(ErrorCodes::BadValue, str::stream()
                << "Invalid namespace for getMore: " << nss.ns());
        }

        if (cursorid == 0) {
            return Status(ErrorCodes::BadValue, "Cursor id for getMore must be non-zero");
        }

        if (batchSize < 0) {
            return Status(ErrorCodes::BadValue, str::stream()
                << "Batch size for getMore must be non-negative, "
                << "but received: " << batchSize);
        }

        return Status::OK();
    }

    // static
    std::string GetMoreRequest::parseNs(const std::string& dbname, const BSONObj& cmdObj) {
        BSONElement collElt = cmdObj["collection"];
        const std::string coll = (collElt.type() == BSONType::String) ? collElt.String()
                                                                      : "";

        return str::stream() << dbname << "." << coll;
    }

    // static
    StatusWith<GetMoreRequest> GetMoreRequest::parseFromBSON(const std::string& dbname,
                                                             const BSONObj& cmdObj) {
        if (!str::equals(cmdObj.firstElementFieldName(), "getMore")) {
            return StatusWith<GetMoreRequest>(ErrorCodes::FailedToParse, str::stream()
                << "First field name must be 'getMore' in: " << cmdObj);
        }

        BSONElement cursorIdElt = cmdObj.firstElement();
        if (cursorIdElt.type() != BSONType::NumberLong) {
            return StatusWith<GetMoreRequest>(ErrorCodes::TypeMismatch, str::stream()
                << "Field 'getMore' must be of type long in: " << cmdObj);
        }
        const CursorId cursorid = cursorIdElt.Long();

        BSONElement collElt = cmdObj["collection"];
        if (collElt.type() != BSONType::String) {
            return StatusWith<GetMoreRequest>(ErrorCodes::TypeMismatch, str::stream()
                << "Field 'collection' must be of type string in: " << cmdObj);
        }
        const std::string fullns = parseNs(dbname, cmdObj);

        int batchSize = kDefaultBatchSize;
        BSONElement batchSizeElt = cmdObj["batchSize"];
        if (batchSizeElt.type() != BSONType::NumberInt && !batchSizeElt.eoo()) {
            return StatusWith<GetMoreRequest>(ErrorCodes::TypeMismatch, str::stream()
                << "Field 'batchSize' must be of type int in: " << cmdObj);
        }
        else if (!batchSizeElt.eoo()) {
            batchSize = batchSizeElt.Int();
        }

        GetMoreRequest request(fullns, cursorid, batchSize);
        Status validStatus = request.isValid();
        if (!validStatus.isOK()) {
            return StatusWith<GetMoreRequest>(validStatus);
        }

        return StatusWith<GetMoreRequest>(request);
    }

} // namespace mongo
