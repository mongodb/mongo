/**
 * Copyright (c) 2011 10gen Inc.
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

#include "util/intrusive_counter.h"

namespace mongo {

    class InterruptStatus;

    class ExpressionContext :
        public IntrusiveCounterUnsigned {
    public:
        virtual ~ExpressionContext();

        void setInShard(bool b);
        void setInRouter(bool b);

        bool getInShard() const;
        bool getInRouter() const;

        /**
           Used by a pipeline to check for interrupts so that killOp() works.

           @throws if the operation has been interrupted
         */
        void checkForInterrupt();

        static ExpressionContext *create(InterruptStatus *pStatus);

    private:
        ExpressionContext(InterruptStatus *pStatus);
        
        bool inShard;
        bool inRouter;
        unsigned intCheckCounter; // interrupt check counter
        InterruptStatus *const pStatus;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline void ExpressionContext::setInShard(bool b) {
        inShard = b;
    }
    
    inline void ExpressionContext::setInRouter(bool b) {
        inRouter = b;
    }

    inline bool ExpressionContext::getInShard() const {
        return inShard;
    }

    inline bool ExpressionContext::getInRouter() const {
        return inRouter;
    }

};
