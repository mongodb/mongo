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

#include <boost/intrusive_ptr.hpp>

namespace mongo {

/*
  IntrusiveCounter is a sharable implementation of a reference counter that
  objects can use to be compatible with boost::intrusive_ptr<>.

  Some objects that use IntrusiveCounter are immutable, and only have
  const methods.  This may require their pointers to be declared as
  intrusive_ptr<const ClassName> .  In order to be able to share pointers to
  these immutables, the methods associated with IntrusiveCounter are declared
  as const, and the counter itself is marked as mutable.
 */
    class IntrusiveCounter :
        boost::noncopyable {
    public:
	virtual ~IntrusiveCounter() {};

	IntrusiveCounter();

	friend inline void intrusive_ptr_add_ref(const IntrusiveCounter *pIC) {
	    pIC->addRef(); };
	friend inline void intrusive_ptr_release(const IntrusiveCounter *pIC) {
	    pIC->release(); };

	void addRef() const;
	void release() const;

    private:
	mutable unsigned counter;
    };
};

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline void IntrusiveCounter::addRef() const {
	++counter;
    }

    inline void IntrusiveCounter::release() const {
	if (!--counter)
	    delete this;
    }

    inline IntrusiveCounter::IntrusiveCounter():
	counter(0) {
    }

};
