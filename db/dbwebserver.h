/** @file dbwebserver.h
 */

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"

namespace mongo {
    
    class DbWebHandler {
    public:
        DbWebHandler( const string& name , double priority , bool requiresREST );
        virtual ~DbWebHandler(){}

        virtual bool handles( const string& url ) const { return url == _defaultUrl; }
                
        virtual double priority() const { return _priority; }
        virtual bool requiresREST( const string& url ) const { return _requiresREST; }

        virtual void handle( const char *rq, // the full request
                             string url,
                             // set these and return them:
                             string& responseMsg,
                             int& responseCode,
                             vector<string>& headers, // if completely empty, content-type: text/html will be added
                             const SockAddr &from
                             ) = 0;
        
        bool operator<( const DbWebHandler& other ) const { return priority() < other.priority(); }
        
        string toString() const { return _toString; }
        static DbWebHandler * findHandler( const string& url );
    private:
        string _name;
        double _priority;
        bool _requiresREST;
        
        string _defaultUrl;
        string _toString;

        static vector<DbWebHandler*> * _handlers;
    };

    void webServerThread();
    string prettyHostName();
};


