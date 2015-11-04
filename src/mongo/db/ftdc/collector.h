/**
 * Copyright (C) 2015 MongoDB Inc.
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

#include <memory>
#include <tuple>
#include <vector>

#include "mongo/base/disallow_copying.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Date_t;
class Client;
class OperationContext;

/**
 * BSON Collector interface
 *
 * Provides an interface to collect BSONObjs from system providers
 */
class FTDCCollectorInterface {
    MONGO_DISALLOW_COPYING(FTDCCollectorInterface);

public:
    virtual ~FTDCCollectorInterface() = default;

    /**
     * Name of the collector
     *
     * Used to stamp before and after dates to measure time to collect.
     */
    virtual std::string name() const = 0;

    /**
     * Collect a sample.
     *
     * If a collector fails to collect data, it should update builder with the result of the
     * failure.
     */
    virtual void collect(OperationContext* txn, BSONObjBuilder& builder) = 0;

protected:
    FTDCCollectorInterface() = default;
};

/**
 * Manages the set of BSON collectors
 *
 * Not Thread-Safe. Locking is owner's responsibility.
 */
class FTDCCollectorCollection {
    MONGO_DISALLOW_COPYING(FTDCCollectorCollection);

public:
    FTDCCollectorCollection() = default;

    /**
     * Add a metric collector to the collection.
     * Must be called before collect. Cannot be called after collect is called.
     */
    void add(std::unique_ptr<FTDCCollectorInterface> collector);

    /**
     * Collect a sample from all collectors. Called after all adding is complete.
     * Returns a tuple of a sample, and the time at which collecting started.
     *
     * Sample schema:
     * {
     *    "start" : Date_t,    <- Time at which all collecting started
     *    "name" : {           <- name is from name() in FTDCCollectorInterface
     *       "start" : Date_t, <- Time at which name() collection started
     *       "data" : { ... }  <- data comes from collect() in FTDCCollectorInterface
     *       "end" : Date_t,   <- Time at which name() collection ended
     *    },
     *    ...
     *    "end" : Date_t,      <- Time at which all collecting ended
     * }
     */
    std::tuple<BSONObj, Date_t> collect(Client* client);

private:
    // collection of collectors
    std::vector<std::unique_ptr<FTDCCollectorInterface>> _collectors;
};

}  // namespace mongo
