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

#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/vector_clock_test_fixture.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class ReadWriteConcernDefaultsTest : public ServiceContextTest {
protected:
    ReadWriteConcernDefaultsLookupMock _lookupMock;

    ReadWriteConcernDefaults& _rwcd{[&]() -> ReadWriteConcernDefaults& {
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        return ReadWriteConcernDefaults::get(getServiceContext());
    }()};

    ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* const _opCtx{_opCtxHolder.get()};
};

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithAbsentDefaults) {
    // By not calling _lookupMock.setLookupCallReturnValue(), tests _defaults.lookup() returning
    // boost::none.
    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithDefaultsNeverSet) {
    _lookupMock.setLookupCallReturnValue({});

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithUnsetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithSetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults.setDefaultWriteConcern(wc);
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(defaults.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(4, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultLookupFailure) {
    _lookupMock.setLookupCallFailure(Status{ErrorCodes::Error(1234), "foobar"});
    ASSERT_THROWS_CODE_AND_WHAT(_rwcd.getDefault(_opCtx), AssertionException, 1234, "foobar");
}

TEST_F(ReadWriteConcernDefaultsTest, TestGetDefaultWithoutInvalidateDoesNotCallLookup) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    auto defaults2 = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults2.getDefaultReadConcern());
    ASSERT(!defaults2.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestInvalidate) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());

    RWConcernDefault newDefaults2;
    newDefaults2.setDefaultReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern));
    WriteConcernOptions wc;
    wc.wNumNodes = 4;
    newDefaults2.setDefaultWriteConcern(wc);
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.invalidate();
    auto defaults2 = _rwcd.getDefault(_opCtx);
    ASSERT(defaults2.getDefaultReadConcern()->getLevel() ==
           repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(4, defaults2.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults2.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndAbsentDefaults) {
    _rwcd.refreshIfNecessary(_opCtx);

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT(!defaults.getUpdateOpTime());
    ASSERT(!defaults.getUpdateWallClockTime());
    ASSERT_EQ(Date_t(), defaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithEmptyCacheAndSetDefaults) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    _rwcd.refreshIfNecessary(_opCtx);

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());
    ASSERT_GT(defaults.localUpdateWallClockTime(), Date_t());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithHigherEpoch) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(1, 2));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT_EQ(Timestamp(1, 2), *defaults.getUpdateOpTime());
    ASSERT_EQ(1234, defaults.getUpdateWallClockTime()->toMillisSinceEpoch());

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(3, 4));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.refreshIfNecessary(_opCtx);

    auto defaults2 = _rwcd.getDefault(_opCtx);
    ASSERT_EQ(Timestamp(3, 4), *defaults2.getUpdateOpTime());
    ASSERT_EQ(5678, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
}

TEST_F(ReadWriteConcernDefaultsTest, TestRefreshDefaultsWithLowerEpoch) {
    RWConcernDefault newDefaults;
    newDefaults.setUpdateOpTime(Timestamp(10, 20));
    newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

    auto defaults = _rwcd.getDefault(_opCtx);
    ASSERT(defaults.getUpdateOpTime());
    ASSERT(defaults.getUpdateWallClockTime());

    RWConcernDefault newDefaults2;
    newDefaults2.setUpdateOpTime(Timestamp(5, 6));
    newDefaults2.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
    _lookupMock.setLookupCallReturnValue(std::move(newDefaults2));

    _rwcd.refreshIfNecessary(_opCtx);

    auto defaults2 = _rwcd.getDefault(_opCtx);
    ASSERT_EQ(Timestamp(10, 20), *defaults2.getUpdateOpTime());
    ASSERT_EQ(1234, defaults2.getUpdateWallClockTime()->toMillisSinceEpoch());
}

/**
 * ReadWriteConcernDefaults::generateNewConcerns() uses the current clusterTime and wall clock time
 * (for epoch and setTime/localSetTime), so testing it requires a fixture with a logical clock.
 */
class ReadWriteConcernDefaultsTestWithClusterTime : public VectorClockTestFixture {
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
        ASSERT(defaults.getUpdateOpTime());
        ASSERT(defaults.getUpdateWallClockTime());

        _lookupMock.setLookupCallReturnValue(std::move(defaults));
        auto oldDefaults = _rwcd.getDefault(operationContext());

        VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
        getMockClockSource()->advance(Milliseconds(1));

        return oldDefaults;
    }

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    ReadWriteConcernDefaults& _rwcd{[&]() -> ReadWriteConcernDefaults& {
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
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
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
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
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
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
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidUnsetReadConcernAndWriteConcern) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewConcerns(
        operationContext(), repl::ReadConcernArgs(), WriteConcernOptions());
    ASSERT(!defaults.getDefaultReadConcern());
    ASSERT(!defaults.getDefaultWriteConcern());
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidSetWriteConcernWithOnlyJ) {
    auto oldDefaults = setupOldDefaults();
    auto defaults =
        _rwcd.generateNewConcerns(operationContext(),
                                  boost::none,
                                  uassertStatusOK(WriteConcernOptions::parse(BSON("j" << true))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT_EQ(1, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(0, defaults.getDefaultWriteConcern()->wTimeout);
    ASSERT(WriteConcernOptions::SyncMode::JOURNAL == defaults.getDefaultWriteConcern()->syncMode);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime,
       TestGenerateNewConcernsValidSetWriteConcernWithOnlyWtimeout) {
    auto oldDefaults = setupOldDefaults();
    auto defaults = _rwcd.generateNewConcerns(
        operationContext(),
        boost::none,
        uassertStatusOK(WriteConcernOptions::parse(BSON("wtimeout" << 12345))));
    ASSERT(oldDefaults.getDefaultReadConcern()->getLevel() ==
           defaults.getDefaultReadConcern()->getLevel());
    ASSERT_EQ(1, defaults.getDefaultWriteConcern()->wNumNodes);
    ASSERT_EQ(12345, defaults.getDefaultWriteConcern()->wTimeout);
    ASSERT(WriteConcernOptions::SyncMode::UNSET == defaults.getDefaultWriteConcern()->syncMode);
    ASSERT_LT(*oldDefaults.getUpdateOpTime(), *defaults.getUpdateOpTime());
    ASSERT_LT(*oldDefaults.getUpdateWallClockTime(), *defaults.getUpdateWallClockTime());

    _lookupMock.setLookupCallReturnValue(std::move(defaults));
    _rwcd.refreshIfNecessary(operationContext());
    auto newDefaults = _rwcd.getDefault(operationContext());
    ASSERT_LT(oldDefaults.localUpdateWallClockTime(), newDefaults.localUpdateWallClockTime());
}

TEST_F(ReadWriteConcernDefaultsTestWithClusterTime, TestRefreshDefaultsWithDeletedDefaults) {
    RWConcernDefault origDefaults;
    origDefaults.setUpdateOpTime(Timestamp(10, 20));
    origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
    _lookupMock.setLookupCallReturnValue(std::move(origDefaults));

    auto origCachedDefaults = _rwcd.getDefault(operationContext());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());

    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    getMockClockSource()->advance(Milliseconds(1));

    _lookupMock.setLookupCallReturnValue(RWConcernDefault());

    _rwcd.refreshIfNecessary(operationContext());

    // The cache should now contain default constructed defaults.
    auto newCachedDefaults = _rwcd.getDefault(operationContext());
    ASSERT(!newCachedDefaults.getUpdateOpTime());
    ASSERT(!newCachedDefaults.getUpdateWallClockTime());
    ASSERT_LT(origCachedDefaults.localUpdateWallClockTime(),
              newCachedDefaults.localUpdateWallClockTime());
}

}  // namespace
}  // namespace mongo
