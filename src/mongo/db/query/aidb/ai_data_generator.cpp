/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/aidb/ai_data_generator.h"

#include <sstream>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo::ai {
DataGenerator::DataGenerator() {
    _alphabet.reserve(26);
    for (char c = 97; c < 123; ++c) {
        _alphabet.push_back(c);
    }

    std::random_device rd;
    _rnd = std::mt19937{rd()};
    _charDistribution = std::uniform_int_distribution<size_t>{0, _alphabet.size() - 1};

    _words = {
        {"A", "The", "One", "Some", "My"},
        {"boy", "girl", "dog", "cat", "person"},
        {"drove", "jumped", "ran", "walked", "striked"},
        {"to", "from", "over", "across", "on"},
        {"town", "cafe", "shop", "store", "house"},
    };

    _wordsDistributions.reserve(_words.size());

    for (const auto& ws : _words) {
        _wordsDistributions.emplace_back(0, ws.size() - 1);
    }
}

std::vector<BSONObj> DataGenerator::generateDocuments(size_t minLength,
                                                      size_t maxLength,
                                                      size_t size) {
    auto data = randomStrings(5, 25, size);
    auto sentences = randomSentences(size);

    std::vector<BSONObj> docs{};
    docs.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        BSONObj doc =
            BSON(GENOID << "index" << (int)i << "data" << data[i] << "sentence" << sentences[i]);
        docs.push_back(doc);
    }

    return docs;
}

std::vector<std::string> DataGenerator::randomStrings(size_t minLength,
                                                      size_t maxLength,
                                                      size_t count) {
    std::vector<std::string> result;
    result.reserve(count);
    std::uniform_int_distribution<size_t> lengthDistribution(minLength, maxLength);

    for (size_t i = 0; i < count; ++i) {
        size_t length = lengthDistribution(_rnd);
        result.emplace_back(randomString(length));
    }

    return result;
}

std::string DataGenerator::randomString(size_t length) {
    std::string result{};
    result.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        result.push_back(randomChar());
    }

    return result;
}

std::vector<std::string> DataGenerator::randomSentences(size_t count) {
    std::vector<std::string> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        result.emplace_back(randomSentence());
    }

    return result;
}

std::string DataGenerator::randomSentence() {
    std::ostringstream oss{};

    for (size_t i = 0; i < _words.size(); ++i) {
        if (i != 0) {
            oss << ' ';
        }

        oss << _words[i][_wordsDistributions[i](_rnd)];
    }

    return oss.str();
}

char DataGenerator::randomChar() {
    return _alphabet[_charDistribution(_rnd)];
}
}  // namespace mongo::ai
