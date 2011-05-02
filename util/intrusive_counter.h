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

namespace mongo {

    class IntrusiveCounter :
        boost::noncopyable {
    public:
	virtual ~IntrusiveCounter() {};

	IntrusiveCounter();

	friend void intrusive_ptr_add_ref(IntrusiveCounter *pIC) {
	    pIC->addRef(); };
	friend void intrusive_ptr_release(IntrusiveCounter *pIC) {
	    pIC->release(); };

	void addRef();
	void release();

    private:
	unsigned counter;
    };
};

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline void IntrusiveCounter::addRef() {
	++counter;
    }

    inline void IntrusiveCounter::release() {
	if (!--counter)
	    delete this;
    }

    inline IntrusiveCounter::IntrusiveCounter():
	counter(0) {
    }

};
