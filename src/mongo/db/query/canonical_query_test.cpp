/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    /**
     * Utility function to parse the given JSON as a MatchExpression and normalize the expression
     * tree.  Returns the resulting tree, or an error Status.
     */
    StatusWithMatchExpression parseNormalize(const std::string& queryStr) {
        StatusWithMatchExpression swme = MatchExpressionParser::parse(fromjson(queryStr));
        if (!swme.getStatus().isOK()) {
            return swme;
        }
        return StatusWithMatchExpression(CanonicalQuery::normalizeTree(swme.getValue()));
    }

    TEST(CanonicalQueryTest, IsValidText) {
        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Valid: regular TEXT.
        swme = parseNormalize("{$text: {$search: 's'}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get()));

        // Valid: TEXT inside OR.
        swme = parseNormalize(
            "{$or: ["
            "    {$text: {$search: 's'}},"
            "    {a: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get()));

        // Valid: TEXT outside NOR.
        swme = parseNormalize("{$text: {$search: 's'}, $nor: [{a: 1}, {b: 1}]}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: TEXT inside NOR.
        swme = parseNormalize("{$nor: [{$text: {$search: 's'}}, {a: 1}]}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: TEXT inside NOR.
        swme = parseNormalize(
            "{$nor: ["
            "    {$or: ["
            "        {$text: {$search: 's'}},"
            "        {a: 1}"
            "    ]},"
            "    {a: 2}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: >1 TEXT.
        swme = parseNormalize(
            "{$and: ["
            "    {$text: {$search: 's'}},"
            "    {$text: {$search: 't'}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: >1 TEXT.
        swme = parseNormalize(
            "{$and: ["
            "    {$or: ["
            "        {$text: {$search: 's'}},"
            "        {a: 1}"
            "    ]},"
            "    {$or: ["
            "        {$text: {$search: 't'}},"
            "        {b: 1}"
            "    ]}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));
    }

    TEST(CanonicalQueryTest, IsValidGeo) {
        auto_ptr<MatchExpression> me;
        StatusWithMatchExpression swme(Status::OK());

        // Valid: regular GEO_NEAR.
        swme = parseNormalize("{a: {$near: [0, 0]}}");
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get()));

        // Valid: GEO_NEAR inside nested AND.
        swme = parseNormalize(
            "{$and: ["
            "    {$and: ["
            "        {a: {$near: [0, 0]}},"
            "        {b: 1}"
            "    ]},"
            "    {c: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: {$near: [0, 0]}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {a: {$geoNear: [0, 0]}},"
            "    {b: {$near: [0, 0]}}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: >1 GEO_NEAR.
        swme = parseNormalize(
            "{$and: ["
            "    {$and: ["
            "        {a: {$near: [0, 0]}},"
            "        {b: 1}"
            "    ]},"
            "    {$and: ["
            "        {c: {$near: [0, 0]}},"
            "        {d: 1}"
            "    ]}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: GEO_NEAR inside NOR.
        swme = parseNormalize(
            "{$nor: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));

        // Invalid: GEO_NEAR inside OR.
        swme = parseNormalize(
            "{$or: ["
            "    {a: {$near: [0, 0]}},"
            "    {b: 1}"
            "]}"
        );
        ASSERT_OK(swme.getStatus());
        me.reset(swme.getValue());
        ASSERT_NOT_OK(CanonicalQuery::isValid(me.get()));
    }

}
