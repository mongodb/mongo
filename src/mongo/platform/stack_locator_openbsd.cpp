/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/platform/stack_locator.h"

#include <pthread_np.h>
#include <sys/signal.h>

#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

StackLocator::StackLocator() {
    stack_t stack_info;

    pthread_t self = pthread_self();

    invariant(pthread_stackseg_np(self, &stack_info) == 0);

    _begin = stack_info.ss_sp;

    // TODO: Assumes stack grows downward on OpenBSD
    _end = static_cast<char*>(_begin) - stack_info.ss_size;
}

}  // namespace mongo
