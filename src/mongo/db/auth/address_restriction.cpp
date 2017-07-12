/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/address_restriction_gen.h"
#include "mongo/stdx/memory.h"

constexpr mongo::StringData mongo::address_restriction_detail::ClientSource::label;
constexpr mongo::StringData mongo::address_restriction_detail::ClientSource::field;

constexpr mongo::StringData mongo::address_restriction_detail::ServerAddress::label;
constexpr mongo::StringData mongo::address_restriction_detail::ServerAddress::field;

mongo::StatusWith<mongo::RestrictionSet<>> mongo::parseAddressRestrictionSet(
    const BSONObj& obj) try {
    IDLParserErrorContext ctx("address restriction");
    const auto ar = Address_restriction::parse(ctx, obj);
    std::vector<std::unique_ptr<Restriction>> vec;

    const boost::optional<std::vector<StringData>>& client = ar.getClientSource();
    if (client) {
        vec.push_back(stdx::make_unique<ClientSourceRestriction>(client.get()));
    }

    const boost::optional<std::vector<StringData>>& server = ar.getServerAddress();
    if (server) {
        vec.push_back(stdx::make_unique<ServerAddressRestriction>(server.get()));
    }

    if (vec.empty()) {
        return Status(ErrorCodes::CollectionIsEmpty,
                      "At least one of 'clientSource' or 'serverAddress' must be set");
    }
    return RestrictionSet<>(std::move(vec));
} catch (const DBException& e) {
    return Status(ErrorCodes::BadValue, e.what());
}
