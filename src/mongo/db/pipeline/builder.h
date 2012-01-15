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

namespace mongo {

    class BSONArrayBuilder;
    class BSONObjBuilder;

    /*
      Generic Builder.

      The methods to append items to an object (on BSONObjBuilder) and an array
      (on BSONArrayBuilder) differ only by their inclusion of a field name. 
      For more complicated implementations of addToBsonObj() and
      addToBsonArray(), it makes sense to abstract that out and use
      this generic builder that always looks the same, and then implement
      addToBsonObj() and addToBsonArray() by using a common method.
    */
    class Builder :
        boost::noncopyable {
    public:
        virtual ~Builder() {};

        virtual void append() = 0; // append a null
        virtual void append(bool b) = 0;
        virtual void append(int i) = 0;
        virtual void append(long long ll) = 0;
        virtual void append(double d) = 0;
        virtual void append(string s) = 0;
        virtual void append(const OID &o) = 0;
        virtual void append(const Date_t &d) = 0;
        virtual void append(BSONObjBuilder *pDone) = 0;
        virtual void append(BSONArrayBuilder *pDone) = 0;
    };

    class BuilderObj :
        public Builder {
    public:
        // virtuals from Builder
        virtual void append();
        virtual void append(bool b);
        virtual void append(int i);
        virtual void append(long long ll);
        virtual void append(double d);
        virtual void append(string s);
        virtual void append(const OID &o);
        virtual void append(const Date_t &d);
        virtual void append(BSONObjBuilder *pDone);
        virtual void append(BSONArrayBuilder *pDone);

        BuilderObj(BSONObjBuilder *pBuilder, string fieldName);

    private:
        BSONObjBuilder *pBuilder;
        string fieldName;
    };

    class BuilderArray :
        public Builder {
    public:
        // virtuals from Builder
        virtual void append();
        virtual void append(bool b);
        virtual void append(int i);
        virtual void append(long long ll);
        virtual void append(double d);
        virtual void append(string s);
        virtual void append(const OID &o);
        virtual void append(const Date_t &d);
        virtual void append(BSONObjBuilder *pDone);
        virtual void append(BSONArrayBuilder *pDone);

        BuilderArray(BSONArrayBuilder *pBuilder);

    private:
        BSONArrayBuilder *pBuilder;
    };
}
