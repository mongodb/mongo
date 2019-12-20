/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_clock_test_fixture.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/clock_source_mock.h"

/**
 * Unit tests of the ReadWriteConcernDefaults type.
 */

namespace mongo {
namespace {

class ReadWriteConcernDefaultsTest : public ServiceContextTest {
public:
    virtual ~ReadWriteConcernDefaultsTest() {
        _rwcd.invalidate();
    }

protected:
    auto* operationContext() {
        return _opCtx.get();
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;
    ReadWriteConcernDefaults& _rwcd{[&]() -> ReadWriteConcernDefaults& {
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getLookupFn());
        return ReadWriteConcernDefaults::get(getServiceContext());
    }()};

private:
    ServiceContext::UniqueOperationContext _opCtx{makeOperationContext()};
};

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentDefaults) {
    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getEpoch());
    ASSERT(!defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithDefaultsNeverSet) {
    _lookupMock.setLookupCallReturnValue({});

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getEpoch());
    ASSERT(!defaults.getSetTime());
    ASSERT(defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithUnsetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultLookupFailure) {
    _lookupMock.setLookupCallFailure(Status{ErrorCodes::Error(1234), "foobar"});
    ASSERT_THROWS_CODE_AND_WHAT(
        _rwcd.getDefault(operationContext()), AssertionException, 1234, "foobar");
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithoutInvalidateDoesNotCallLookup) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults.getLocalSetTime());

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setEpoch(Timestamp(3, 4));
    newDefaults2.setSetTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    auto defaults2 = _rwcd.getDefault(operationContext());
    ASSERT(!defaults2.getDefaultReadConcern());
    ASSERT(!defaults2.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults2.getEpoch());
    ASSERT_EQ(1234, defaults2.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults2.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestInvalidate) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults.getLocalSetTime());

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setEpoch(Timestamp(3, 4));
    newDefaults2.setSetTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.invalidate();
    auto defaults2 = _rwcd.getDefault(operationContext());
    ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(4, defaults2.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getEpoch());
    ASSERT_EQ(5678, defaults2.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults2.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndAbsentDefaults) {
    _rwcd.refreshIfNecessary(operationContext());

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getEpoch());
    ASSERT(!defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndSetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    _rwcd.refreshIfNecessary(operationContext());

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());
    ASSERT(defaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithHigherEpoch) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(1, 2));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getEpoch());
    ASSERT_EQ(1234, defaults.getSetTime()->toMillisSinceEpoch());

    RWConcernDefault newDefaults2;
    newDefaults2.setEpoch(Timestamp(3, 4));
    newDefaults2.setSetTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.refreshIfNecessary(operationContext());

    auto defaults2 = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getEpoch());
    ASSERT_EQ(5678, defaults2.getSetTime()->toMillisSinceEpoch());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithLowerEpoch) {
    RWConcernDefault newDefaults;
    newDefaults.setEpoch(Timestamp(10, 20));
    newDefaults.setSetTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(operationContext());
    ASSERT(defaults.getEpoch());
    ASSERT(defaults.getSetTime());

    RWConcernDefault newDefaults2;
    newDefaults2.setEpoch(Timestamp(5, 6));
    newDefaults2.setSetTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.refreshIfNecessary(operationContext());

    auto defaults2 = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(10, 20), *defaults2.getEpoch());
    ASSERT_EQ(1234, defaults2.getSetTime()->toMillisSinceEpoch());
}

/**
 * ReadWriteConcernDefaults::generateNewConcerns() uses the current clusterTime and wall clock time
 * (for epoch and setTime/localSetTime), so testing it requires a fixture with a logical clock.
 */
class ReadWriteConcernDefaultsTestWithClusterTime : public LogicalClockTestFixture {
public:
    virtual ~ReadWriteConcernDefaultsTestWithClusterTime() {
        _rwcd.invalidate();
    }

protected:
    auto setupOldDefaults() {
        auto defaults = _rwcd.generateNewConcerns(
            operationContext(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 4))));
        ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
               repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
        ASSERT(defaults.getEpoch());
        ASSERT(defaults.getSetTime());
        ASSERT(!defaults.getLocalSetTime());

        _lookupMock.setLookupCallReturnValue(std::move(defaults));
        auto oldDefaults = _rwcd.getDefault(operationContext());

        getClock()->reserveTicks(1);
        getMockClockSource()->advance(Milliseconds(1));

        return oldDefaults;
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;
    ReadWriteConcernDefaults& _rwcd{[&]() -> ReadWriteConcernDefaults& {
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getLookupFn());
        return ReadWriteConcernDefaults::get(getServiceContext());
    }()};
};

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime, TestGenerateNewConcernsInvalidNeither) {
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(operationContext(), boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsInvalidReadConcernLevel) {
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(
                           operationContext(),
                           repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(
                           operationContext(),
                           repl::ReadConcernArgs(repl::ReadConcernLevel::kLinearizableReadConcern),
                           boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsInvalidReadConcernFields) {
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(operationContext(),
                                                 repl::ReadConcernArgs::fromBSONThrows(
                                                     BSON("level"
                                                          << "local"
                                                          << "afterOpTime" << repl::OpTime())),
                                                 boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(operationContext(),
                                                 repl::ReadConcernArgs::fromBSONThrows(BSON(
                                                     "level"
                                                     << "local"
                                                     << "afterClusterTime" << Timestamp(1, 2))),
                                                 boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(_rwcd.generateNewConcerns(operationContext(),
                                                 repl::ReadConcernArgs::fromBSONThrows(
                                                     BSON("level"
                                                          << "snapshot"
                                                          << "atClusterTime" << Timestamp(1, 2))),
                                                 boost::none),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime, TestGenerateNewConcernsInvalidWriteConcern) {
    ASSERT_THROWS_CODE(
        _rwcd.generateNewConcerns(operationContext(),
                                  boost::none,
                                  uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 0)))),
        AssertionException,
        ErrorCodes::BadValue);
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidSetReadConcernAndWriteConcerns) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewConcerns(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT_EQ(5, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getEpoch(), *defaults.getEpoch());
    ASSERT_LT(*oldDefaults.getSetTime(), *defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(*oldDefaults.getLocalSetTime(), *newDefaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidSetReadConcernOnly) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewConcerns(
        operationContext(),
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern),
        boost::none);
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT_EQ(oldDefaults.getDefaultWriteConcern()->wNumNodes,
              defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getEpoch(), *defaults.getEpoch());
    ASSERT_LT(*oldDefaults.getSetTime(), *defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(*oldDefaults.getLocalSetTime(), *newDefaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidSetWriteConcernOnly) {
    auto oldDefaults = setupOldDefaults();
    auto defaults =
        _rwcd.generateNewConcerns(operationContext(),
                                  boost::none,
                                  uassertStatusOK(WriteConcernOptions::parse(BSON("w" << 5))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT_EQ(5, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_LT(*oldDefaults.getEpoch(), *defaults.getEpoch());
    ASSERT_LT(*oldDefaults.getSetTime(), *defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(*oldDefaults.getLocalSetTime(), *newDefaults.getLocalSetTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidUnsetReadConcernAndWriteConcern) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewConcerns(
        operationContext(), repl::ReadConcernArgs(), WriteConcernOptions());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_LT(*oldDefaults.getEpoch(), *defaults.getEpoch());
    ASSERT_LT(*oldDefaults.getSetTime(), *defaults.getSetTime());
    ASSERT(!defaults.getLocalSetTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(*oldDefaults.getLocalSetTime(), *newDefaults.getLocalSetTime());
}

}  // namespace
}  // namespace mongo
