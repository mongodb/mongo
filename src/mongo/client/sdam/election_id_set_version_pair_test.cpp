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

#include "mongo/client/sdam/election_id_set_version_pair.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sdam {
namespace {

class ElectionIdSetVersionPairTest : public unittest::Test {
public:
    static inline const OID kOidOne{"000000000000000000000001"};
    static inline const OID kOidTwo{"000000000000000000000002"};

    static inline const boost::optional<OID> kNullOid;
    static inline const boost::optional<int> kNullSet;

    enum class Compare {
        kEquals,
        kLess,
        kGreater,
        // When any compare operator returns false.
        kNotComparable
    };

    struct TestCase {
        // Curent topology max fields.
        const boost::optional<OID> kTerm1;
        const boost::optional<int> kSet1;
        // Incoming primary fields.
        const boost::optional<OID> kTerm2;
        const boost::optional<int> kSet2;
        const Compare compare;
    };

    std::string log(const TestCase& t, int testNum) {
        str::stream s;
        s << "[" << t.kTerm1 << ", " << t.kSet1 << "][" << t.kTerm2 << ", " << t.kSet2 << "]";
        s << ", test=" << testNum;
        return s;
    }

    void test(const TestCase& t, int testNum) {
        ElectionIdSetVersionPair p1{t.kTerm1, t.kSet1};
        ElectionIdSetVersionPair p2{t.kTerm2, t.kSet2};

        switch (t.compare) {
            case Compare::kEquals:
                ASSERT_TRUE(p1 == p2) << log(t, testNum);
                break;
            case Compare::kLess:
                ASSERT_TRUE(p1 < p2) << log(t, testNum);
                break;
            case Compare::kGreater:
                ASSERT_TRUE(p1 > p2) << log(t, testNum);
                break;
            case Compare::kNotComparable:
                ASSERT_FALSE(p1 > p2) << log(t, testNum);
                ASSERT_FALSE(p1 < p2) << log(t, testNum);
                ASSERT_FALSE(p1 == p2) << log(t, testNum);
                break;
        }
    }
};

TEST_F(ElectionIdSetVersionPairTest, ExpectedOutcome) {
    std::vector<TestCase> tests = {
        // At startup, both current fields are not set. Field set > unset.
        {kNullOid, kNullSet, kOidOne, 1, Compare::kLess},

        {kOidOne, 1, kOidOne, 1, Compare::kEquals},

        // One field is not set. This should never happen however added for better
        // coverage for malformed protocol.
        {kNullOid, 1, kOidOne, 1, Compare::kLess},
        {kOidOne, kNullSet, kOidOne, 1, Compare::kLess},
        {kOidOne, 1, kNullOid, 1, Compare::kGreater},
        {kOidOne, 1, kOidOne, kNullSet, Compare::kGreater},

        // Primary advanced one way or another. "Less" means current < incoming.
        {kOidOne, 1, kOidTwo, 1, Compare::kLess},
        {kOidOne, 1, kOidOne, 2, Compare::kLess},

        // Primary advanced but current state is incomplete.
        {kNullOid, 1, kOidTwo, 1, Compare::kLess},
        {kNullOid, 1, kOidOne, 2, Compare::kLess},
        {kOidOne, kNullSet, kOidTwo, 1, Compare::kLess},
        {kOidOne, kNullSet, kOidOne, 2, Compare::kLess},

        // Primary went backwards one way or another.
        // Inconsistent because Set version went backwards without Term being changed (same
        // primary).
        {kOidTwo, 2, kOidTwo, 1, Compare::kGreater},
        {kOidTwo, 2, kOidOne, 2, Compare::kGreater},

        // Primary went backwards with current state incomplete.
        {kNullOid, 2, kOidTwo, 1, Compare::kLess},
        {kOidTwo, kNullSet, kOidOne, 1, Compare::kGreater},

        // Stale primary case when Term went backwards but Set version advanced.
        // This case is 'consistent' because it's normal for stale primary. The
        // important part is that 'current' > 'incoming', which makes the node 'Unknown'.
        {kOidTwo, 1, kOidOne, 2, Compare::kGreater},

        // Previous primary was unable to replicate the Set version before failover,
        // new primary forces it to be rolled back.
        {kOidOne, 2, kOidTwo, 1, Compare::kLess},
    };

    int testNum = 0;
    for (const auto& t : tests) {
        test(t, testNum++);
    }
}

TEST_F(ElectionIdSetVersionPairTest, TestLessThanRelation) {
    ElectionIdSetVersionPair hasNone{boost::none, boost::none};
    ElectionIdSetVersionPair hasBoth{kOidOne, 1};
    ElectionIdSetVersionPair hasBoth2{kOidOne, 2};
    ElectionIdSetVersionPair noEid{boost::none, 1};
    ElectionIdSetVersionPair noEid2{boost::none, 2};
    ElectionIdSetVersionPair noSet{kOidOne, boost::none};
    ElectionIdSetVersionPair noSet2{kOidTwo, boost::none};

    ASSERT_TRUE(hasNone < noEid);
    ASSERT(hasNone == hasNone);

    ASSERT_TRUE(hasNone < noEid2);
    ASSERT_TRUE(noEid < noEid2);
    ASSERT(noEid == noEid);

    ASSERT_TRUE(hasNone < noSet);
    ASSERT_TRUE(noEid < noSet);
    ASSERT_TRUE(noEid2 < noSet);
    ASSERT(noEid2 == noEid2);

    ASSERT_TRUE(hasNone < noSet2);
    ASSERT_TRUE(noEid < noSet2);
    ASSERT_TRUE(noEid2 < noSet2);
    ASSERT_TRUE(noSet < noSet2);
    ASSERT(noSet == noSet);

    ASSERT_TRUE(hasNone < hasBoth);
    ASSERT_TRUE(noEid < hasBoth);
    ASSERT_TRUE(noEid2 < hasBoth);
    ASSERT_TRUE(noSet < hasBoth);
    ASSERT_TRUE(noSet2 < hasBoth);
    ASSERT(noSet2 == noSet2);

    ASSERT_TRUE(hasNone < hasBoth2);
    ASSERT_TRUE(noEid < hasBoth2);
    ASSERT_TRUE(noEid2 < hasBoth2);
    ASSERT_TRUE(noSet < hasBoth2);
    ASSERT_TRUE(noSet2 < hasBoth2);
    ASSERT_TRUE(hasBoth < hasBoth2);
    ASSERT(hasBoth == hasBoth);
    ASSERT(hasBoth2 == hasBoth2);
}

}  // namespace
}  // namespace mongo::sdam
