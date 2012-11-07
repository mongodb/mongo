// server_status.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#pragma once

#include <string>
#include "mongo/db/jsobj.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

    class ServerStatusSection {
    public:
        ServerStatusSection( const string& sectionName );
        virtual ~ServerStatusSection(){}

        const string& getSectionName() const { return _sectionName; }

        /**
         * if this returns true, if the user doesn't mention this section
         * it will be included in the result
         * if they do : 1, it will be included
         * if they do : 0, it will not
         * 
         * examples (section 'foo')
         *  includeByDefault returning true
         *     foo : 0 = not included
         *     foo : 1 = included
         *     foo missing = included
         *  includeByDefault returning false
         *     foo : 0 = not included
         *     foo : 1 = included
         *     foo missing = false
         */
        virtual bool includeByDefault() const = 0;
        
        /**
         * if only admins can view this section
         * API will change to better auth version
         */
        virtual bool adminOnly() const = 0;

        /**
         * actually generate the result
         * @param configElement the element from the actual command related to this section
         *                      so if the section is 'foo', this is cmdObj['foo']
         */
        virtual BSONObj generateSection( const BSONElement& configElement, bool userIsAdmin ) const = 0;

    private:
        const string _sectionName;
    };

    class OpCounterServerStatusSection : public ServerStatusSection {
    public:
        OpCounterServerStatusSection( const string& sectionName, OpCounters* counters );
        virtual bool includeByDefault() const { return true; }
        virtual bool adminOnly() const { return false; }
        
        virtual BSONObj generateSection( const BSONElement& configElement, bool userIsAdmin ) const;

    private:
        const OpCounters* _counters;
    };

    class ServerStatusMetric {
    public:
        /**
         * @param name is a dotted path of a counter name
         *             if name starts with . its treated as a path from the serverStatus root
         *             otherwise it will live under the "counters" namespace
         *             so foo.bar would be serverStatus().counters.foo.bar
         */
        ServerStatusMetric( const string& name, bool adminOnly );
        virtual ~ServerStatusMetric(){}

        string getMetricName() const { return _name; }

        virtual bool adminOnly() const { return _adminOnly; }

        virtual void appendAtLeaf( BSONObjBuilder& b ) const = 0;

    protected:
        static string _parseLeafName( const string& name );

        const string _name;
        const bool _adminOnly;
        const string _leafName;
    };

    /**
     * usage
     * 
     * declared once
     *    Counter counter;
     *    ServerStatusMetricField myAwesomeCounterDisplay( "path.to.counter", false, &counter );
     * 
     * call
     *    counter.hit();
     * 
     * will show up in db.serverStatus().metrics.path.to.counter
     */
    template< typename T >
    class ServerStatusMetricField : public ServerStatusMetric {
    public:
        ServerStatusMetricField( const string& name, bool adminOnly, const T* t ) 
            : ServerStatusMetric( name, adminOnly ), _t(t) {
        }
        
        const T* get() { return _t; }

        virtual void appendAtLeaf( BSONObjBuilder& b ) const {
            b.append( _leafName, *_t );
        }

    private:
        const T* _t;
    };

}

