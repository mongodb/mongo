// server_parameters.h

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
#include <map>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class ServerParameterSet;

    /**
     * Lets you make server level settings easily configurable.
     * Hooks into (set|get)Paramter, as well as command line processing
     */
    class ServerParameter {
    public:
        typedef std::map< std::string, ServerParameter* > Map;

        ServerParameter( ServerParameterSet* sps, const std::string& name );
        virtual ~ServerParameter();

        std::string name() const { return _name; }

        /**
         * @return if you can set on command line or config file
         */
        virtual bool allowedToChangeAtStartup() const { return true; }

        /**
         * @param if you can use (get|set)Parameter
         */
        virtual bool allowedToChangeAtRuntime() const { return true; }


        virtual void append( BSONObjBuilder& b ) = 0;

        virtual Status set( const BSONElement& newValueElement ) = 0;

        virtual Status setFromString( const string& str ) = 0;

    private:
        string _name;
    };

    class ServerParameterSet {
    public:
        typedef std::map< std::string, ServerParameter* > Map;

        void add( ServerParameter* sp );

        const Map& getMap() const { return _map; }

        static ServerParameterSet* getGlobal();

    private:
        Map _map;
    };

    template<typename T>
    class ExportedServerParameter : public ServerParameter {
    public:
        ExportedServerParameter( ServerParameterSet* sps, const std::string& name, T* value )
            : ServerParameter( sps, name ), _value( value ) {}
        virtual ~ExportedServerParameter() {}

        virtual void append( BSONObjBuilder& b ) {
            b.append( name(), *_value );
        }

        virtual Status set( const BSONElement& newValueElement );
        virtual Status set( const T& newValue );

        virtual const T& get() const { return *_value; }

        virtual Status setFromString( const string& str );

    protected:

        virtual Status validate( const T& potentialNewValue ){ return Status::OK(); }

        T* _value; // owned elsewhere
    };

#define MONGO_EXPORT_SERVER_PARAMETER( NAME, TYPE, INITIAL_VALUE ) \
    TYPE NAME = INITIAL_VALUE; \
    ExportedServerParameter<TYPE> _##NAME( ServerParameterSet::getGlobal(), #NAME, &NAME )

}

#include "server_parameters_inline.h"
