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

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/repl/idempotency_scalar_generator.h"
#include "mongo/platform/random.h"

namespace mongo {

class DocumentStructureEnumerator;
class BSONObj;
struct BSONArray;
class BSONObjBuilder;

struct UpdateSequenceGeneratorConfig {
    UpdateSequenceGeneratorConfig(std::set<StringData> fields_,
                                  std::size_t depth_,
                                  std::size_t length_,
                                  double scalarProbability_ = 0.250,
                                  double docProbability_ = 0.250,
                                  double arrProbability_ = 0.250);

    const std::set<StringData> fields = {};
    const std::size_t depth = 0;
    const std::size_t length = 0;
    const double scalarProbability = 0.250;
    const double docProbability = 0.250;
    const double arrProbability = 0.250;
};

class UpdateSequenceGenerator {

public:
    UpdateSequenceGenerator(UpdateSequenceGeneratorConfig config,
                            PseudoRandom random,
                            ScalarGenerator* scalarGenerator);

    BSONObj generateUpdate() const;

    std::vector<std::string> getPaths() const;

    friend std::vector<std::string> eliminatePrefixPaths_forTest(
        const std::string& path, const std::vector<std::string>& paths);

    friend std::size_t getPathDepth_forTest(const std::string& path);

private:
    enum class SetChoice : int { kSetScalar, kSetArr, kSetDoc, kNumTotalSetChoices = 3 };

    static std::size_t _getPathDepth(const std::string& path);

    /**
     * Given a path parameter, removes all paths from a copy of the given path vector that are:
     * 1) A prefix of the given path
     * 2) Prefixable by the given path.
     *
     * This function also removes the given path itself from the given path vector, if it exists
     * inside, since a path can prefix itself and therefore qualifies for both #1 and #2 above.
     *
     * A copy of the given path vector is returned after this pruning finishes.
     */
    static std::vector<std::string> _eliminatePrefixPaths(const std::string& path,
                                                          const std::vector<std::string>& paths);

    void _generatePaths(const UpdateSequenceGeneratorConfig& config, const std::string& path);

    std::set<StringData> _getRemainingFields(const std::string& path) const;

    DocumentStructureEnumerator _getValidEnumeratorForPath(const std::string& path) const;

    std::vector<std::string> _getRandomPaths() const;

    BSONObj _generateSet() const;

    SetChoice _determineWhatToSet(const std::string& setPath) const;

    void _appendSetArgToBuilder(const std::string& setPath, BSONObjBuilder* setArgBuilder) const;

    BSONObj _generateUnset() const;

    double _generateNumericToSet() const;

    bool _generateBoolToSet() const;

    BSONArray _generateArrToSet(const std::string& setPath) const;

    BSONObj _generateDocToSet(const std::string& setPath) const;

    std::vector<std::string> _paths;
    const UpdateSequenceGeneratorConfig _config;
    mutable PseudoRandom _random;
    const ScalarGenerator* _scalarGenerator;
};

}  // namespace mongo
