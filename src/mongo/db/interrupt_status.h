/**
 * Copyright (c) 2012 10gen Inc.
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
 */

#pragma once

#include "pch.h"

namespace mongo {

    /**
       Abstraction for checking on interrupt status.

       The killCurrentOp object (currently declared in curop.h, and defined
       in instance.cpp) can only be referenced within mongod.  This abstraction
       serves to isolate that so that code that runs in both mongod and mongos
       can be linked without unresolved symbols.  There is a concrete
       implementation of this class for mongod which references killCurrentOp,
       and another implementation for mongos which does not (currently, it
       does nothing, but that should be fixed).
     */
    class InterruptStatus {
    public:
        /**
           Check for interrupt.

           @throws a uassert if the process has received an interrupt (SIGINT)
         */
        virtual void checkForInterrupt() = 0;

        /**
           Check for interrupt.

           @returns a pointer to a string with additional information; will be
             "" if there hasn't been an interrupt.  These strings are static
             and don't need to be freed.
         */
        virtual const char *checkForInterruptNoAssert() = 0;

    protected:
        /**
           This interface is meant to be used in the implementation of static
           objects, so the destructor is made protected so that they can't be
           accidentally deleted.  It can't be private, or the compiler
           generates warnings about being unable to generate the derived
           classes' destructors.
         */
        virtual ~InterruptStatus() {};
    };

};
