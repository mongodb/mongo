/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/local_catalog/lock_manager/exception_util.h"

#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/lock_manager/exception_util_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ExceptionUtilTest : public ServiceContextTest {};

TEST_F(ExceptionUtilTest, RecordWriteConflictIncreasesWriteConflictMetric) {
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(0, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
    recordWriteConflict(opCtx.get());
    ASSERT_EQUALS(1LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
    recordWriteConflict(opCtx.get(), 4);
    ASSERT_EQUALS(5LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
}

TEST_F(ExceptionUtilTest,
       RecordTemporarilyUnavailableErrorsIncreasesTemporarilyUnavailableErrorsMetric) {
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(
        0, CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
    recordTemporarilyUnavailableErrors(opCtx.get());
    ASSERT_EQUALS(
        1LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
    recordTemporarilyUnavailableErrors(opCtx.get(), 4);
    ASSERT_EQUALS(
        5LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
}

TEST_F(ExceptionUtilTest, WriteConflictRetryInstantiatesOK) {
    auto opCtx = makeOperationContext();
    writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [] {});
}

TEST_F(ExceptionUtilTest, WriteConflictRetryWriteConflictException) {
    // WriteConflictRetry retries function on WriteConflictException.
    {
        auto opCtx = makeOperationContext();
        ASSERT_EQUALS(0, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
        ASSERT_EQUALS(
            100, writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [&opCtx] {
                if (0 == CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts) {
                    throwWriteConflictException(
                        str::stream()
                        << "Verify that we retry the WriteConflictRetry function when we "
                           "encounter a WriteConflictException.");
                }
                return 100;
            }));
        ASSERT_EQUALS(1LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
    }

    // If already in a WriteUnitOfWork, WriteConflictRetry propagates WriteConflictException.
    {
        auto opCtx = makeOperationContext();
        Lock::GlobalWrite globalWrite(opCtx.get());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_THROWS(
            writeConflictRetry(opCtx.get(),
                               "",
                               NamespaceString::kEmpty,
                               [] {
                                   throwWriteConflictException(
                                       str::stream()
                                       << "Verify that WriteConflictExceptions are propogated "
                                          "if we are already in a WriteUnitOfWork.");
                               }),
            WriteConflictException);
    }
}

TEST_F(ExceptionUtilTest, WriteConflictRetryTemporarilyUnavailableException) {
    // WriteConflictRetry retries function on TemporarilyUnavailableException in user connection
    // until max retries.
    {
        // Mock a client with a user connection.
        auto serviceContext = getServiceContext();
        transport::TransportLayerMock transportLayer;
        auto clientUserConn =
            serviceContext->getService()->makeClient("userconn", transportLayer.createSession());
        auto opCtx = clientUserConn->makeOperationContext();

        // Lower sleep time to retry immediately.
        gTemporarilyUnavailableExceptionRetryBackoffBaseMs.store(0);

        // WriteConflictRetry retries function on TemporarilyUnavailableException.
        {
            ASSERT_EQUALS(
                0,
                CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
            ASSERT_EQUALS(
                100, writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [&opCtx] {
                    if (0 ==
                        CurOp::get(opCtx.get())
                            ->getOperationStorageMetrics()
                            .temporarilyUnavailableErrors) {
                        throwTemporarilyUnavailableException(
                            str::stream()
                            << "Verify that we retry the WriteConflictRetry function when we "
                               "encounter a TemporarilyUnavailableException in a user connection.");
                    }
                    return 100;
                }));
            ASSERT_EQUALS(
                1LL,
                CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
            // Confirm TemporarilyUnavailableException is not converted to WCE.
            ASSERT_EQUALS(0, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
        }

        // WriteConflictRetry propogates TemporarilyUnavailableException for user connections when
        // max retries exceeded.
        {
            ASSERT_EQUALS(
                1LL,
                CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
            ASSERT_THROWS(
                writeConflictRetry(
                    opCtx.get(),
                    "",
                    NamespaceString::kEmpty,
                    [] {
                        throwTemporarilyUnavailableException(
                            str::stream()
                            << "Verify that TemporarilyUnavailableExceptions are propogated when "
                               "max retries exceeded in a user connection.");
                    }),
                TemporarilyUnavailableException);
            // Total temporarilyUnavailableErrors exceeds max retries by 2 because we already
            // accumulated 1 error prior to this writeConflictRetry.
            ASSERT_EQUALS(
                2 + gTemporarilyUnavailableExceptionMaxRetryAttempts.load(),
                CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
        }
    }

    // WriteConflictRetry converts TemporarilyUnavailableException to WriteConflictException and
    // retries on internal operations.
    {
        auto opCtx = makeOperationContext();
        ASSERT_EQUALS(0, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
        ASSERT_EQUALS(
            0, CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
        ASSERT_EQUALS(
            100, writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [&opCtx] {
                if (0 == CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts) {
                    throwTemporarilyUnavailableException(
                        str::stream()
                        << "Verify that we retry the WriteConflictRetry function when we "
                           "encounter a TemporarilyUnavailableException.");
                }
                return 100;
            }));
        ASSERT_EQUALS(1LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
        ASSERT_EQUALS(
            1LL,
            CurOp::get(opCtx.get())->getOperationStorageMetrics().temporarilyUnavailableErrors);
    }

    // If already in a WriteUnitOfWork, WriteConflictRetry propagates
    // TemporarilyUnavailableException.
    {
        auto opCtx = makeOperationContext();
        Lock::GlobalWrite globalWrite(opCtx.get());
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_THROWS(writeConflictRetry(
                          opCtx.get(),
                          "",
                          NamespaceString::kEmpty,
                          [] {
                              throwTemporarilyUnavailableException(
                                  str::stream()
                                  << "Verify that TemporarilyUnavailableExceptions are propogated "
                                     "if we are already in a WriteUnitOfWork.");
                          }),
                      TemporarilyUnavailableException);
    }

    // If in a WriteUnitOfWork for a multidocument transaction, WriteConflictRetry propogates
    // TemporarilyUnavailableException as a WriteConflictException.
    {
        auto opCtx = makeOperationContext();
        Lock::GlobalWrite globalWrite(opCtx.get());
        opCtx->setInMultiDocumentTransaction();
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_THROWS(
            writeConflictRetry(opCtx.get(),
                               "",
                               NamespaceString::kEmpty,
                               [] {
                                   throwTemporarilyUnavailableException(
                                       str::stream()
                                       << "Verify that TemporarilyUnavailableExceptions are "
                                          "propogated as WriteConflictExceptions if we are a "
                                          "multidoc transaction in a WriteUnitOfWork.");
                               }),
            WriteConflictException);
    }
}

TEST_F(ExceptionUtilTest, WriteConflictRetryTransactionTooLargeForCacheException) {
    // If writes aren't replicated, WriteConflictRetry retries function on
    // TransactionTooLargeForCacheException.
    {
        auto opCtx = makeOperationContext();
        repl::UnreplicatedWritesBlock uwb(opCtx.get());
        ASSERT_EQUALS(0, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
        ASSERT_EQUALS(
            100, writeConflictRetry(opCtx.get(), "", NamespaceString::kEmpty, [&opCtx] {
                if (0 == CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts) {
                    throwTransactionTooLargeForCache(
                        str::stream()
                        << "Verify that we retry the WriteConflictRetry function when we "
                           "encounter a TransactionTooLargeForCache.");
                }
                return 100;
            }));
        ASSERT_EQUALS(1LL, CurOp::get(opCtx.get())->getOperationStorageMetrics().writeConflicts);
    }

    // If writes are replicated, WriteConflictRetry propagates TransactionTooLargeForCacheException.
    {
        auto opCtx = makeOperationContext();
        ASSERT_THROWS(writeConflictRetry(
                          opCtx.get(),
                          "",
                          NamespaceString::kEmpty,
                          [] {
                              throwTransactionTooLargeForCache(
                                  str::stream()
                                  << "Verify that we retry the WriteConflictRetry function when we "
                                     "encounter a TransactionTooLargeForCache.");
                          }),
                      TransactionTooLargeForCacheException);
    }
}

TEST_F(ExceptionUtilTest, WriteConflictRetryPropagatesNonWriteConflictException) {
    auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(writeConflictRetry(opCtx.get(),
                                          "",
                                          NamespaceString::kEmpty,
                                          [] {
                                              uassert(ErrorCodes::OperationFailed, "", false);
                                              MONGO_UNREACHABLE;
                                          }),
                       AssertionException,
                       ErrorCodes::OperationFailed);
}

}  // namespace
}  // namespace mongo
