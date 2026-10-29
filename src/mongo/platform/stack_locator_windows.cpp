// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/platform/stack_locator.h"
#include "mongo/util/assert_util.h"

namespace mongo {

StackLocator::StackLocator(const void* capturedStackPointer)
    : _capturedStackPointer(capturedStackPointer) {
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
    _begin = static_cast<const char*>(committedMbi.BaseAddress) + committedMbi.RegionSize;

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
