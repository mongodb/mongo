/**
 * Copyright 2017 (c) 10gen Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"

namespace mongo {

class InternalSchemaMaxLengthMatchExpression final : public InternalSchemaStrLengthMatchExpression {

public:
    InternalSchemaMaxLengthMatchExpression()
        : InternalSchemaStrLengthMatchExpression(MatchType::INTERNAL_SCHEMA_MAX_LENGTH,
                                                 "$_internalSchemaMaxLength"_sd) {}

    Validator getComparator() const final {
        return [strLen = strLen()](int lenWithoutNullTerm) {
            return lenWithoutNullTerm <= strLen;
        };
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::unique_ptr<InternalSchemaMaxLengthMatchExpression> maxLen =
            stdx::make_unique<InternalSchemaMaxLengthMatchExpression>();
        invariantOK(maxLen->init(path(), strLen()));
        if (getTag()) {
            maxLen->setTag(getTag()->clone());
        }
        return std::move(maxLen);
    }
};

}  // namespace mongo
