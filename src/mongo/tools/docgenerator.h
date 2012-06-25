/** @file docgenerator.h

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

/**
*   This is a simple document generator. It generates documents of the format
*   { _id: xxx, blob: "xxx", nestedDoc: {xxx}, list: [xxx,yyy,zzz], counter: xxx }
*/

#pragma once

#include <string>
#include <vector>

#include "mongo/db/jsobj.h"

namespace mongo {

    struct DocGeneratorOptions {
        DocGeneratorOptions() :
            hostname(""),
            dbSize( 0.0 ),
            numdbs( 0 ),
            prefix("")
         { }
        std::string hostname;
        double dbSize;
        int numdbs;
        std::string prefix;
    };

    struct DocConfig {
        DocConfig() :
            id( 0 ),
            blob( "" ),
            counter( 0 )
        { }
        long long id;
        std::string blob;
        BSONObj nestedDoc;
        std::vector<std::string> list;
        long long counter;
    };

    // Creates a documentGenerator that can be used to create sample documents
    class DocumentGenerator {
        public:
        DocumentGenerator( ) { }
        ~DocumentGenerator() { }

        void init( BSONObj& args );
        BSONObj createDocument();

        // caller is responsible for managing this raw pointer
        static DocumentGenerator* makeDocumentGenerator( BSONObj args ) {
            DocumentGenerator* runner =  new DocumentGenerator() ;
            runner->init( args );
            return runner;
        }
        DocConfig config;
    };

} // end namespace




