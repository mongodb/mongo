/**
 *    Copyright (C) 2013 10gen Inc.
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

/**
 * This file contains a sample usage of TEST_F for benchmarking
 */


#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#include <unordered_map>

#define TRIALS 100000
#define STR_LEN 32
#define OUTFILE "benchmarks.json"

namespace mongo {

using std::string;

std::string gen_random(const int);

PseudoRandom* rand = new PseudoRandom((uint32_t)time(NULL));

const auto stdStringList = ([]() -> std::vector<std::string> {
    std::vector<std::string> strings;
    for (int i = 0; i < TRIALS * 2; i++) {
        strings.push_back(gen_random(STR_LEN));
    }
    return strings;
})();

const auto cStringList = ([]() -> std::vector<const char*> {
    std::vector<const char*> strings;
    for (int i = 0; i < TRIALS * 2; i++) {
        strings.push_back(stdStringList[i].c_str());
    }
    return strings;
})();

// http://stackoverflow.com/questions/440133/how-do-i-create-a-random-alpha-numeric-string-in-c
std::string gen_random(int length) {
    static string charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
    string result;
    result.resize(length);
    for (int i = 0; i < length; i++) {
        result[i] = charset[rand->nextInt32(charset.length())];
    }

    return result;
}

class BenchmarkMapInsertFixture : public mongo::unittest::Benchmark {
protected:
    std::map<std::string, int> ints;
    std::map<std::string, int>::const_iterator it;

    void setUp() {}
};

class BenchmarkMapLookupFixture : public mongo::unittest::Benchmark {
protected:
    std::map<std::string, int> ints;
    std::map<std::string, int>::const_iterator it;

    void setUp() {
        // Insert a bunch of strings into the map
        int i = 0;
        for (std::string str : stdStringList) {
            ints[str] = i++;
        }
    }
};

class BenchmarkUnorderedMapInsertFixture : public mongo::unittest::Benchmark {
protected:
    std::unordered_map<std::string, int> ints;
    std::unordered_map<std::string, int>::const_iterator it;

    void setUp() {}
};

class BenchmarkUnorderedMapLookupFixture : public mongo::unittest::Benchmark {
protected:
    std::unordered_map<std::string, int> ints;
    std::unordered_map<std::string, int>::const_iterator it;

    void setUp() {
        // Insert a bunch of strings into the map
        int i = 0;
        for (std::string str : stdStringList) {
            ints[str] = i++;
        }
    }
};

class BenchmarkStringMapInsertFixture : public mongo::unittest::Benchmark {
protected:
    mongo::StringMap<int> ints;
    mongo::StringMap<int>::const_iterator it;

    void setUp() {}
};

class BenchmarkStringMapLookupFixture : public mongo::unittest::Benchmark {
protected:
    mongo::StringMap<int> ints;
    mongo::StringMap<int>::const_iterator it;

    void setUp() {
        // Insert a bunch of strings into the map
        int i = 0;
        for (std::string str : stdStringList) {
            ints[str] = i++;
        }
    }
};

// Map
BENCHMARK(BenchmarkMapInsertFixture, addMapStdStr, TRIALS, OUTFILE) {
    ints[stdStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkMapInsertFixture, addMapCStr, TRIALS, OUTFILE) {
    ints[cStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapExistingStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[trial_num]);
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num]);
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapNotExistingStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapNotExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapSingleStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[TRIALS - 1]);
}

BENCHMARK(BenchmarkMapLookupFixture, lookupMapSingleCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[TRIALS - 1]);
}

// UnorderedMap
BENCHMARK(BenchmarkUnorderedMapInsertFixture, addUnorderedMapStdStr, TRIALS, OUTFILE) {
    ints[stdStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkUnorderedMapInsertFixture, addUnorderedMapCStr, TRIALS, OUTFILE) {
    ints[cStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture, lookupUnorderedMapExistingStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[trial_num]);
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture, lookupUnorderedMapExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num]);
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture,
          lookupUnorderedMapNotExistingStdStr,
          TRIALS,
          OUTFILE) {
    it = ints.find(stdStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture, lookupUnorderedMapNotExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture, lookupUnorderedMapSingleStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[TRIALS - 1]);
}

BENCHMARK(BenchmarkUnorderedMapLookupFixture, lookupUnorderedMapSingleCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[TRIALS - 1]);
}

// String Map
BENCHMARK(BenchmarkStringMapInsertFixture, addStringMapStdStr, TRIALS, OUTFILE) {
    ints[stdStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkStringMapInsertFixture, addStringMapCStr, TRIALS, OUTFILE) {
    ints[cStringList[trial_num]] = trial_num;
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapExistingStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[trial_num]);
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num]);
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapNotExistingStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapNotExistingCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[trial_num + TRIALS]);
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapSingleStdStr, TRIALS, OUTFILE) {
    it = ints.find(stdStringList[TRIALS - 1]);
}

BENCHMARK(BenchmarkStringMapLookupFixture, lookupStringMapSingleCStr, TRIALS, OUTFILE) {
    it = ints.find(cStringList[TRIALS - 1]);
}

}  // namespace
