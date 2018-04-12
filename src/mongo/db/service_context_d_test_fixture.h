/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * Test fixture class for tests that use the "ephemeralForTest" storage engine.
 */
class ServiceContextMongoDTest : public unittest::Test {
public:
    /**
     * Initializes global storage engine.
     */
    void setUp() override;

    /**
     * Clear all databases.
     */
    void tearDown() override;

    /**
     * Returns a service context, which is only valid for this instance of the test.
     * Must not be called before setUp or after tearDown.
     */
    ServiceContext* getServiceContext();

private:
    /**
     * Unused implementation of test function. This allows us to instantiate
     * ServiceContextMongoDTest on its own without the need to inherit from it in a test.
     * This supports using ServiceContextMongoDTest inside another test fixture and works around the
     * limitation that tests cannot inherit from multiple test fixtures.
     *
     * It is an error to call this implementation of _doTest() directly.
     */
    void _doTest() override;

    /**
     * Drops all databases. Call this before global ReplicationCoordinator is destroyed -- it is
     * used to drop the databases.
     */
    void _dropAllDBs(OperationContext* opCtx);
};

}  // namespace mongo
