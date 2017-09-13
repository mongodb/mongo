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

#include "mongo/util/assert_util.h"

namespace mongo {

StackLocator::StackLocator() {
    // Please see
    //
    // http://stackoverflow.com/questions/1740888/determining-stack-space-with-visual-studio/1747499#1747499
    //
    // for notes on the following arcana.

    // TODO: _WIN32_WINNT >= 0x0602 (windows 8 / 2012 server) may be
    // able to use GetCurrentThreadStackLimits

    // Put something on the stack, convieniently the variable we are
    // going to write into, and ask the VM system for information
    // about the memory region it inhabits, which is the committed
    // part of the stack.
    MEMORY_BASIC_INFORMATION committedMbi = {};
    invariant(VirtualQuery(&committedMbi, &committedMbi, sizeof(committedMbi)) != 0);
    invariant(committedMbi.State == MEM_COMMIT);

    // Now committedMbi.AllocationBase points to the reserved stack
    // memory base address (the real bottom of the stack), and
    // committedMbi.BaseAddress points to base address of the
    // committed region, and committedMbi.RegionSize is the size of
    // the commit region. Since the stack grows downward, the top of
    // the stack is at the base address for the commit region plus the
    // region size.
    _begin = static_cast<char*>(committedMbi.BaseAddress) + committedMbi.RegionSize;

    // Now, we skip down to the bottom, where the uncommitted memory
    // is, and get its size. So, we ask for the region at the
    // allocation base (the real bottom of the stack), after which
    // uncommitedMbi will have a BaseAddress and a RegionSize that
    // describes the uncommited area. The memory should have the
    // RESERVE state set.
    MEMORY_BASIC_INFORMATION uncommittedMbi = {};
    invariant(VirtualQuery(committedMbi.AllocationBase, &uncommittedMbi, sizeof(uncommittedMbi)) !=
              0);

    invariant(committedMbi.AllocationBase == uncommittedMbi.AllocationBase);
    invariant(uncommittedMbi.RegionSize > 0);

    // If the stack was created with CreateThread with dwStackSize
    // non-zero and without the STACK_SIZE_PARAM_IS_A_RESERVATION
    // flag, then the whole AllocationBase is our stack base, and
    // there is no guard page.
    if (uncommittedMbi.State == MEM_COMMIT) {
        // NOTE: Originally, it seemed to make sense that what you would get back
        // here would be the same information as in committedMbi. After all, the whole
        // stack is committed. So, querying AllocationBase should give you back
        // the same region.
        //
        // Bizzarely, that doesn't seem to be the case. Even though
        // it has the same protections, state, etc., VirtualQuery seems
        // to consider the stack that has been used distinct from the part
        // that hasn't.
        //
        // invariant(uncommittedMbi.BaseAddress == committedMbi.BaseAddress);
        // invariant(uncommittedMbi.RegionSize == committedMbi.RegionSize);
        //

        _end = static_cast<char*>(committedMbi.AllocationBase);
        return;
    }
    invariant(uncommittedMbi.State == MEM_RESERVE);

    if (kDebugBuild) {
        // Locate the guard page, which sits bewteen the uncommitted
        // region (which we know is not empty!), and the committed
        // region. We can count the guard page as usable stack space,
        // but it is good to find it so we can validate that we walked
        // the stack correctly.

        // The end of the guard page is right after the uncommitted
        // area. Form a pointer into the guard page by skipping over the
        // committed region.
        const auto guard =
            static_cast<char*>(uncommittedMbi.BaseAddress) + uncommittedMbi.RegionSize;

        // With that pointer in hand, get the info about the guard
        // region. We really only care about its size here (and we
        // validate that it has the right bits).
        MEMORY_BASIC_INFORMATION guardMbi = {};
        invariant(VirtualQuery(guard, &guardMbi, sizeof(guardMbi)) != 0);

        invariant(committedMbi.AllocationBase == guardMbi.AllocationBase);
        invariant((guardMbi.Protect & PAGE_GUARD) != 0);
        invariant(guardMbi.RegionSize > 0);
    }

    // The end of our stack is the allocation base for the whole stack.
    _end = static_cast<char*>(committedMbi.AllocationBase);
}

}  // namespace mongo
