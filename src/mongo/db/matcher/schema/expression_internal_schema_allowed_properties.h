/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/util/pcre.h"

namespace mongo {

/**
 * Match expression that matches documents whose properties meet certain requirements based on field
 * name. Specifically, a document matches if:
 *
 *  - each field that matches a regular expression in '_patternProperties' also matches the
 *    corresponding match expression; and
 *  - any field not contained in '_properties' nor matching a pattern in '_patternProperties'
 *    matches the '_otherwise' match expression.
 *
 * For example, consider the match expression
 *
 *  {$_internalSchemaAllowedProperties: {
 *      properties: ["address", "phone"],
 *      namePlaceholder: "i",
 *      patternProperties: [
 *          {regex: /[nN]ame/, expression: {i: {$_internalSchemaType: "string"}}},
 *          {regex: /[aA]ddress/, expression: {i: {$_internalSchemaMinLength: 30}}}
 *      ],
 *      otherwise: {i: {$_internalSchemaType: 'number'}}
 *
 * Then, given the object
 *
 *   {
 *      firstName: "juan",
 *      lastName: "de la cruz",
 *      middleName: ["daniel", "marcos"],
 *      phone: 1234567890,
 *      address: "new york, ny",
 *      hobbies: ["programming", "c++"],
 *      socialSecurityNumber: 123456789
 *   }
 *
 * we have that
 *
 *  - "firstName" and "lastName" are valid, because they satisfy the pattern schema corresponding to
 *    to the regular expression /[nN]ame/.
 *  - "middleName" is invalid, because it doesn't satisfy the schema associated with /[nN]ame/.
 *  - "phone" is valid, because it is listed in the "properties" section.
 *  - "address" is invalid even though it is listed in "properties", because it fails to validate
 *    against the pattern schema for /[aA]ddress/.
 *  - "hobbies" is invalid because it is not contained in "properties", not matched by a regex in
 *    "patternProperties", and does not validate against the "otherwise" schema.
 *  - "socialSecurityNumber" is valid because it matches the schema for "otherwise".
 *
 *  Because there exists at least one field that is invalid, the entire document would fail to
 *  match.
 */
class InternalSchemaAllowedPropertiesMatchExpression final : public MatchExpression {
public:
    /**
     * A container for regular expression data. Holds a regex object, as well as the original
     * string pattern, which is used for comparisons and serialization.
     */
    struct Pattern {
        explicit Pattern(StringData pattern)
            : rawRegex(pattern), regex(std::make_unique<pcre::Regex>(std::string{rawRegex})) {}

        StringData rawRegex;
        std::unique_ptr<pcre::Regex> regex;
    };

    /**
     * A PatternSchema is a regular expression paired with an associated match expression, and
     * represents a constraint in JSON Schema's "patternProperties" keyword.
     */
    using PatternSchema = std::pair<Pattern, std::unique_ptr<ExpressionWithPlaceholder>>;

    static constexpr StringData kName = "$_internalSchemaAllowedProperties"_sd;

    explicit InternalSchemaAllowedPropertiesMatchExpression(
        StringDataSet properties,
        StringData namePlaceholder,
        std::vector<PatternSchema> patternProperties,
        std::unique_ptr<ExpressionWithPlaceholder> otherwise,
        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    bool equivalent(const MatchExpression* expr) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    /**
     * The input matches if:
     *
     *  - it is a document;
     *  - each field that matches a regular expression in '_patternProperties' also matches the
     *    corresponding match expression; and
     *  - any field not contained in '_properties' nor matching a pattern in '_patternProperties'
     *    matches the '_otherwise' match expression.
     */
    bool matches(const MatchableDocument* doc, MatchDetails* details) const final;
    bool matchesSingleElement(const BSONElement& element, MatchDetails* details) const final;

    void serialize(BSONObjBuilder* builder, bool includePath) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return _patternProperties.size() + 1;
    }

    MatchExpression* getChild(size_t i) const final {
        invariant(i < numChildren());

        if (i == 0) {
            return _otherwise->getFilter();
        }

        return _patternProperties[i - 1].second->getFilter();
    }

    virtual void resetChild(size_t i, MatchExpression* other) {
        tassert(6329408, "Out-of-bounds access to child of MatchExpression.", i < numChildren());

        if (i == 0) {
            _otherwise->resetFilter(other);
        } else {
            _patternProperties[i - 1].second->resetFilter(other);
        }
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const StringDataSet& getProperties() const {
        return _properties;
    }

    const std::vector<PatternSchema>& getPatternProperties() const {
        return _patternProperties;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    /**
     * Helper function for matches() and matchesSingleElement().
     */
    bool _matchesBSONObj(const BSONObj& obj) const;

    void _doAddDependencies(DepsTracker* deps) const final {
        deps->needWholeDocument = true;
    }

    // The names of the properties are owned by the BSONObj used to create this match expression.
    // Since that BSONObj must outlive this object, we can safely store StringData.
    StringDataSet _properties;

    // The placeholder used in both '_patternProperties' and '_otherwise'.
    StringData _namePlaceholder;

    std::vector<PatternSchema> _patternProperties;

    std::unique_ptr<ExpressionWithPlaceholder> _otherwise;
};

}  // namespace mongo
