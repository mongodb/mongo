/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/cascades/interfaces.h"

namespace mongo {

namespace optimizer {
namespace cascades {

// Forward declaration.
class CEInterface;

}  // namespace cascades
}  // namespace optimizer

namespace ce {

const double kMaxCEError = 0.01;

/**
 * A helpful macro for asserting that the CE of a $match predicate is approximately what we were
 * expecting.
 */
#define ASSERT_MATCH_CE(ce, predicate, expectedCE) \
    ASSERT_APPROX_EQUAL(expectedCE, ce.getCE(predicate), kMaxCEError)

/**
 * A test utility class for helping verify the cardinality of CE transports on a given $match
 * predicate.
 */
class CETester {
public:
    CETester(std::string collName, double numRecords);

    /**
     * Returns the estimated cardinality of a given 'matchPredicate'.
     */
    double getCE(const std::string& matchPredicate);

protected:
    /**
     * Subclasses need to override this method to initialize the transports they are testing.
     */
    virtual std::unique_ptr<optimizer::cascades::CEInterface> getCETransport(
        OperationContext* opCtx) = 0;

    std::string _collName;

    // The number of records in the collection we are testing.
    double _numRecords;
};

}  // namespace ce
}  // namespace mongo
