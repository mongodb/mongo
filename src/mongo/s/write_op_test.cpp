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
 */

#include "mongo/s/write_op.h"

#include "mongo/s/batched_command_request.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;

    TEST(WriteOpTests, Basic) {
        WriteOp( BatchItemRef( NULL, 0 ) );
        ASSERT( true );
    }

} // unnamed namespace
