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

#pragma once

#include "mongo/db/matcher/expression_tree.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaXor keyword. Returns true only if exactly
 * one of its child nodes matches.
 */
class InternalSchemaXorMatchExpression final : public ListOfMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaXor"_sd;

    InternalSchemaXorMatchExpression() : ListOfMatchExpression(INTERNAL_SCHEMA_XOR) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails* details = nullptr) const final;

    virtual std::unique_ptr<MatchExpression> shallowClone() const {
        auto xorCopy = stdx::make_unique<InternalSchemaXorMatchExpression>();
        for (size_t i = 0; i < numChildren(); ++i) {
            xorCopy->add(getChild(i)->shallowClone().release());
        }
        if (getTag()) {
            xorCopy->setTag(getTag()->clone());
        }
        return std::move(xorCopy);
    }

    void debugString(StringBuilder& debug, int level = 0) const final;

    void serialize(BSONObjBuilder* out) const final;
};
}  // namespace mongo
