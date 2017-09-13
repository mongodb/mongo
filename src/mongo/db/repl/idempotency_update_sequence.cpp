/**
 * Copyright (C) 2017 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/idempotency_update_sequence.h"

#include <algorithm>
#include <memory>

#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/repl/idempotency_document_structure.h"

namespace mongo {

UpdateSequenceGeneratorConfig::UpdateSequenceGeneratorConfig(std::set<StringData> fields_,
                                                             std::size_t depth_,
                                                             std::size_t length_,
                                                             double scalarProbability_,
                                                             double docProbability_,
                                                             double arrProbability_)
    : fields(std::move(fields_)),
      depth(depth_),
      length(length_),
      scalarProbability(scalarProbability_),
      docProbability(docProbability_),
      arrProbability(arrProbability_) {}

std::size_t UpdateSequenceGenerator::_getPathDepth(const std::string& path) {
    // Our depth is -1 because we count at 0, but numParts just counts the number of fields.
    return path == "" ? 0 : FieldRef(path).numParts() - 1;
}

std::vector<std::string> UpdateSequenceGenerator::_eliminatePrefixPaths(
    const std::string& path, const std::vector<std::string>& paths) {
    std::vector<std::string> remainingPaths;
    for (auto oldPath : paths) {
        if (!FieldRef(oldPath).isPrefixOf(FieldRef(path)) &&
            !FieldRef(path).isPrefixOf(FieldRef(oldPath)) && path != path) {
            remainingPaths.push_back(oldPath);
        }
    }

    return remainingPaths;
}

void UpdateSequenceGenerator::_generatePaths(const UpdateSequenceGeneratorConfig& config,
                                             const std::string& path) {
    if (UpdateSequenceGenerator::_getPathDepth(path) == config.depth) {
        return;
    }

    if (!path.empty()) {
        for (std::size_t i = 0; i < config.length; i++) {
            FieldRef arrPathRef(path);
            arrPathRef.appendPart(std::to_string(i));
            auto arrPath = arrPathRef.dottedField().toString();
            _paths.push_back(arrPath);
            _generatePaths(config, arrPath);
        }
    }

    if (config.fields.empty()) {
        return;
    }

    std::set<StringData> remainingFields(config.fields);
    for (auto field : config.fields) {
        remainingFields.erase(remainingFields.begin());
        FieldRef docPathRef(path);
        docPathRef.appendPart(field);
        auto docPath = docPathRef.dottedField().toString();
        _paths.push_back(docPath);
        UpdateSequenceGeneratorConfig remainingConfig = {remainingFields,
                                                         config.depth,
                                                         config.length,
                                                         config.scalarProbability,
                                                         config.docProbability,
                                                         config.arrProbability};
        _generatePaths(remainingConfig, docPath);
    }
}

std::vector<std::string> UpdateSequenceGenerator::_getRandomPaths() const {
    std::size_t randomAmountOfArgs = this->_random.nextInt32(this->_paths.size()) + 1;
    std::vector<std::string> randomPaths;
    std::vector<std::string> validPaths(this->_paths);

    for (std::size_t i = 0; i < randomAmountOfArgs; i++) {
        int randomIndex = UpdateSequenceGenerator::_random.nextInt32(validPaths.size());
        std::string randomPath = validPaths[randomIndex];
        randomPaths.push_back(randomPath);
        validPaths = UpdateSequenceGenerator::_eliminatePrefixPaths(randomPath, validPaths);
        if (validPaths.empty()) {
            break;
        }
    }

    return randomPaths;
}

BSONObj UpdateSequenceGenerator::generateUpdate() const {
    double setSum = this->_config.scalarProbability + this->_config.arrProbability +
        this->_config.docProbability;
    double generateSetUpdate = this->_random.nextCanonicalDouble();
    if (generateSetUpdate <= setSum) {
        return _generateSet();
    } else {
        return _generateUnset();
    }
}

BSONObj UpdateSequenceGenerator::_generateSet() const {
    BSONObjBuilder setBuilder;
    {
        BSONObjBuilder setArgBuilder(setBuilder.subobjStart("$set"));

        for (auto randomPath : _getRandomPaths()) {
            _appendSetArgToBuilder(randomPath, &setArgBuilder);
        }
    }
    return setBuilder.obj();
}

UpdateSequenceGenerator::SetChoice UpdateSequenceGenerator::_determineWhatToSet(
    const std::string& setPath) const {
    if (UpdateSequenceGenerator::_getPathDepth(setPath) == this->_config.depth) {
        // If we have hit the max depth, we don't have a choice anyways.
        return SetChoice::kSetScalar;
    } else {
        double setSum = this->_config.scalarProbability + this->_config.arrProbability +
            this->_config.docProbability;
        double choice = this->_random.nextCanonicalDouble() * setSum;
        if (choice <= this->_config.scalarProbability) {
            return SetChoice::kSetScalar;
        } else if (choice <= setSum - this->_config.docProbability) {
            return SetChoice::kSetArr;
        } else {
            return SetChoice::kSetDoc;
        }
    }
}

void UpdateSequenceGenerator::_appendSetArgToBuilder(const std::string& setPath,
                                                     BSONObjBuilder* setArgBuilder) const {
    auto setChoice = _determineWhatToSet(setPath);
    switch (setChoice) {
        case SetChoice::kSetScalar:
            this->_scalarGenerator->generateScalar().addToBsonObj(setArgBuilder, setPath);
            return;
        case SetChoice::kSetArr:
            setArgBuilder->append(setPath, _generateArrToSet(setPath));
            return;
        case SetChoice::kSetDoc:
            setArgBuilder->append(setPath, _generateDocToSet(setPath));
            return;
        case SetChoice::kNumTotalSetChoices:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

BSONObj UpdateSequenceGenerator::_generateUnset() const {
    BSONObjBuilder unsetBuilder;
    {
        BSONObjBuilder unsetArgBuilder(unsetBuilder.subobjStart("$unset"));

        for (auto randomPath : _getRandomPaths()) {
            unsetArgBuilder.appendNull(randomPath);
        }
    }

    return unsetBuilder.obj();
}

double UpdateSequenceGenerator::_generateNumericToSet() const {
    return UpdateSequenceGenerator::_random.nextCanonicalDouble() * INT_MAX;
}

bool UpdateSequenceGenerator::_generateBoolToSet() const {
    return this->_random.nextInt32(2) == 1;
}

BSONArray UpdateSequenceGenerator::_generateArrToSet(const std::string& setPath) const {
    auto enumerator = _getValidEnumeratorForPath(setPath);

    auto possibleArrs = enumerator.enumerateArrs();
    std::size_t randomIndex = this->_random.nextInt32(possibleArrs.size());
    auto chosenArr = possibleArrs[randomIndex];

    return chosenArr;
}

BSONObj UpdateSequenceGenerator::_generateDocToSet(const std::string& setPath) const {
    auto enumerator = _getValidEnumeratorForPath(setPath);
    std::size_t randomIndex = this->_random.nextInt32(enumerator.getDocs().size());
    return enumerator.getDocs()[randomIndex];
}

std::set<StringData> UpdateSequenceGenerator::_getRemainingFields(const std::string& path) const {
    std::set<StringData> remainingFields(this->_config.fields);

    FieldRef pathRef(path);
    StringData lastField;
    // This is guaranteed to terminate with a value for lastField, since no valid path contains only
    // array positions (numbers).
    for (int i = pathRef.numParts() - 1; i >= 0; i--) {
        auto field = pathRef.getPart(i);
        if (this->_config.fields.find(field) != this->_config.fields.end()) {
            lastField = field;
            break;
        }
    }

    // The last alphabetic field used must be after all other alphabetic fields that could ever be
    // used, since the fields that are used are selected in the order that they pop off from a
    // std::set.
    for (auto field : this->_config.fields) {
        remainingFields.erase(field);
        if (field == lastField) {
            break;
        }
    }

    return remainingFields;
}

DocumentStructureEnumerator UpdateSequenceGenerator::_getValidEnumeratorForPath(
    const std::string& path) const {
    auto remainingFields = _getRemainingFields(path);
    std::size_t remainingDepth = this->_config.depth - UpdateSequenceGenerator::_getPathDepth(path);
    if (remainingDepth > 0) {
        remainingDepth -= 1;
    }

    DocumentStructureEnumerator enumerator({remainingFields, remainingDepth, this->_config.length},
                                           this->_scalarGenerator);
    return enumerator;
}

std::vector<std::string> UpdateSequenceGenerator::getPaths() const {
    return this->_paths;
}

UpdateSequenceGenerator::UpdateSequenceGenerator(UpdateSequenceGeneratorConfig config,
                                                 PseudoRandom random,
                                                 ScalarGenerator* scalarGenerator)
    : _config(std::move(config)), _random(random), _scalarGenerator(scalarGenerator) {
    auto path = "";
    _generatePaths(config, path);
    // Creates the same shuffle each time, but we don't care. We want to mess up the DFS ordering.
    std::random_shuffle(this->_paths.begin(), this->_paths.end(), this->_random);
}

}  // namespace mongo
