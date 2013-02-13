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

        ServerParameter( ServerParameterSet* sps, const std::string& name,
                         bool allowedToChangeAtStartup, bool allowedToChangeAtRuntime );
        ServerParameter( ServerParameterSet* sps, const std::string& name );
        virtual ~ServerParameter();

        std::string name() const { return _name; }

        /**
         * @return if you can set on command line or config file
         */
        bool allowedToChangeAtStartup() const { return _allowedToChangeAtStartup; }

        /**
         * @param if you can use (get|set)Parameter
         */
        bool allowedToChangeAtRuntime() const { return _allowedToChangeAtRuntime; }


        virtual void append( BSONObjBuilder& b, const string& name ) = 0;

        virtual Status set( const BSONElement& newValueElement ) = 0;

        virtual Status setFromString( const string& str ) = 0;

    private:
        string _name;
        bool _allowedToChangeAtStartup;
        bool _allowedToChangeAtRuntime;
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

    /**
     * Implementation of ServerParameter for reading and writing a server parameter with a given
     * name and type into a specific C++ variable.
     */
    template<typename T>
    class ExportedServerParameter : public ServerParameter {
    public:

        /**
         * Construct an ExportedServerParameter in parameter set "sps", named "name", whose storage
         * is at "value".
         *
         * If allowedToChangeAtStartup is true, the parameter may be set at the command line,
         * e.g. via the --setParameter switch.  If allowedToChangeAtRuntime is true, the parameter
         * may be set at runtime, e.g.  via the setParameter command.
         */
        ExportedServerParameter( ServerParameterSet* sps, const std::string& name, T* value,
                                 bool allowedToChangeAtStartup, bool allowedToChangeAtRuntime)
            : ServerParameter( sps, name, allowedToChangeAtStartup, allowedToChangeAtRuntime ),
              _value( value ) {}
        virtual ~ExportedServerParameter() {}

        virtual void append( BSONObjBuilder& b, const string& name ) {
            b.append( name, *_value );
        }

        virtual Status set( const BSONElement& newValueElement );
        virtual Status set( const T& newValue );

        virtual const T& get() const { return *_value; }

        virtual Status setFromString( const string& str );

    protected:

        virtual Status validate( const T& potentialNewValue ){ return Status::OK(); }

        T* _value; // owned elsewhere
    };
}

#define MONGO_EXPORT_SERVER_PARAMETER_IMPL( NAME, TYPE, INITIAL_VALUE, \
                                            CHANGE_AT_STARTUP, CHANGE_AT_RUNTIME ) \
    TYPE NAME = INITIAL_VALUE;                                          \
    ExportedServerParameter<TYPE> _##NAME(\
            ServerParameterSet::getGlobal(), #NAME, &NAME, CHANGE_AT_STARTUP, CHANGE_AT_RUNTIME )

/**
 * Create a global variable of type "TYPE" named "NAME" with the given INITIAL_VALUE.  The
 * value may be set at startup or at runtime.
 */
#define MONGO_EXPORT_SERVER_PARAMETER( NAME, TYPE, INITIAL_VALUE ) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL( NAME, TYPE, INITIAL_VALUE, true, true )

/**
 * Like MONGO_EXPORT_SERVER_PARAMETER, but the value may only be set at startup.
 */
#define MONGO_EXPORT_STARTUP_SERVER_PARAMETER( NAME, TYPE, INITIAL_VALUE ) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL( NAME, TYPE, INITIAL_VALUE, true, false )

/**
 * Like MONGO_EXPORT_SERVER_PARAMETER, but the value may only be set at runtime.
 */
#define MONGO_EXPORT_RUNTIME_SERVER_PARAMETER( NAME, TYPE, INITIAL_VALUE ) \
    MONGO_EXPORT_SERVER_PARAMETER_IMPL( NAME, TYPE, INITIAL_VALUE, false, true )

#include "server_parameters_inline.h"
