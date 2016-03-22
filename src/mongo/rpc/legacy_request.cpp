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

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/db/namespace_string.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

LegacyRequest::LegacyRequest(const Message* message)
    : _message(std::move(message)), _dbMessage(*message), _queryMessage(_dbMessage) {
    _database = nsToDatabaseSubstring(_queryMessage.ns);

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << "Invalid database name: '" << _database << "'",
        NamespaceString::validDBName(_database, NamespaceString::DollarInDbNameBehavior::Allow));

    std::tie(_upconvertedCommandArgs, _upconvertedMetadata) =
        uassertStatusOK(rpc::upconvertRequestMetadata(std::move(_queryMessage.query),
                                                      std::move(_queryMessage.queryOptions)));
}

LegacyRequest::~LegacyRequest() = default;

StringData LegacyRequest::getDatabase() const {
    return _database;
}

StringData LegacyRequest::getCommandName() const {
    return _upconvertedCommandArgs.firstElement().fieldNameStringData();
}

const BSONObj& LegacyRequest::getMetadata() const {
    // TODO SERVER-18236
    return _upconvertedMetadata;
}

const BSONObj& LegacyRequest::getCommandArgs() const {
    return _upconvertedCommandArgs;
}

DocumentRange LegacyRequest::getInputDocs() const {
    // return an empty document range.
    return DocumentRange{};
}

Protocol LegacyRequest::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

}  // namespace rpc
}  // namespace mongo
