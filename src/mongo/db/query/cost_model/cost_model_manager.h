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

#include <shared_mutex>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/stdx/mutex.h"

namespace mongo::cost_model {

/**
 * This class is main access point to Cost Model Coefficients, it rerieves them and applies
 * overrides.
 */
class CostModelManager {
public:
    CostModelManager();

    /**
     * Returns the current cost model coefficients. They may not be the default ones as the
     * coefficients can be changed at runtime. See the IDL definition of 'CostModelCoefficients' for
     * the names of the fields.
     */
    CostModelCoefficients getCoefficients() const;

    /**
     * This update function will be called when the cost model coefficients are changed at runtime.
     */
    void updateCostModelCoefficients(const BSONObj& overrides);

    /**
     * Returns the default version of Cost Model Coefficients no matter whether there are
     * user-defined coefficients or not.
     */
    static CostModelCoefficients getDefaultCoefficients();

private:
    CostModelCoefficients _coefficients;
    mutable std::shared_mutex _mutex;  // NOLINT
};
}  // namespace mongo::cost_model
