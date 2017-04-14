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

#include "mongo/db/update/array_filter.h"

#include "mongo/db/matcher/expression_parser.h"

#include <boost/regex.hpp>

namespace mongo {

namespace {

// The array filter must begin with a lowercase letter and contain no special characters.
boost::regex idRegex("^[a-z][a-zA-Z0-9]*$");

/**
 * Finds the top-level field that 'expr' is over. The must be unique and not the empty string.
 */
StatusWith<StringData> parseId(MatchExpression* expr) {
    if (expr->isArray() || expr->isLeaf()) {
        auto firstDotPos = expr->path().find('.');
        if (firstDotPos == std::string::npos) {
            return expr->path();
        }
        return expr->path().substr(0, firstDotPos);
    } else if (expr->isLogical()) {
        if (expr->numChildren() == 0) {
            return Status(ErrorCodes::FailedToParse,
                          "No top-level field name found in array filter.");
        }

        StringData id;
        for (size_t i = 0; i < expr->numChildren(); ++i) {
            auto statusWithId = parseId(expr->getChild(i));
            if (!statusWithId.isOK()) {
                return statusWithId.getStatus();
            }

            if (id == StringData()) {
                id = statusWithId.getValue();
                continue;
            }

            if (id != statusWithId.getValue()) {
                return Status(
                    ErrorCodes::FailedToParse,
                    str::stream()
                        << "Each array filter must use a single top-level field name, found '"
                        << id
                        << "' and '"
                        << statusWithId.getValue()
                        << "'");
            }
        }
        return id;
    }

    MONGO_UNREACHABLE;
}

}  // namespace

// static
StatusWith<std::unique_ptr<ArrayFilter>> ArrayFilter::parse(
    BSONObj rawArrayFilter,
    const ExtensionsCallback& extensionsCallback,
    const CollatorInterface* collator) {
    StatusWithMatchExpression statusWithFilter =
        MatchExpressionParser::parse(rawArrayFilter, extensionsCallback, collator);
    if (!statusWithFilter.isOK()) {
        return statusWithFilter.getStatus();
    }
    auto filter = std::move(statusWithFilter.getValue());

    auto statusWithId = parseId(filter.get());
    if (!statusWithId.isOK()) {
        return statusWithId.getStatus();
    }
    auto id = statusWithId.getValue().toString();
    if (!boost::regex_match(id, idRegex)) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "The top-level field name in an array filter must be an alphanumeric "
                             "string beginning with a lowercase letter, found '"
                          << id
                          << "'");
    }

    auto arrayFilter = stdx::make_unique<ArrayFilter>(std::move(id), std::move(filter));
    return {std::move(arrayFilter)};
}

}  // namespace mongo
