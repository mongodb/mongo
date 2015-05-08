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

#include <boost/optional.hpp>

#include "mongo/util/stringutils.h"

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
        // Required fields.
        boost::optional<CursorId> cursorid;
        boost::optional<std::string> fullns;

        // Optional field, set to its default.
        int batchSize = kDefaultBatchSize;

        for (BSONElement el : cmdObj) {
            const char* fieldName = el.fieldName();
            if (str::equals(fieldName, "getMore")) {
                if (el.type() != BSONType::NumberLong) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "Field 'getMore' must be of type long in: " << cmdObj};
                }

                cursorid = el.Long();
            }
            else if (str::equals(fieldName, "collection")) {
                if (el.type() != BSONType::String) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "Field 'collection' must be of type string in: "
                                          << cmdObj};
                }

                fullns = parseNs(dbname, cmdObj);
            }
            else if (str::equals(fieldName, "batchSize")) {
                if (!el.isNumber()) {
                    return {ErrorCodes::TypeMismatch,
                            str::stream() << "Field 'batchSize' must be a number in: " << cmdObj};
                }

                batchSize = el.numberInt();
            }
            else if (!str::startsWith(fieldName, "$")) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << "Failed to parse: " << cmdObj << ". "
                                      << "Unrecognized field '" << fieldName << "'."};
            }
        }

        if (!cursorid) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Field 'getMore' missing in: " << cmdObj};
        }

        if (!fullns) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Field 'collection' missing in: " << cmdObj};
        }

        GetMoreRequest request(*fullns, *cursorid, batchSize);
        Status validStatus = request.isValid();
        if (!validStatus.isOK()) {
            return validStatus;
        }

        return request;
    }

} // namespace mongo
