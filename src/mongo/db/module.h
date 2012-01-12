// module.h

/**
*    Copyright (C) 2008 10gen Inc.info
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

#include "../pch.h"
#include <boost/program_options.hpp>
#include <list>

namespace mongo {

    /**
     * Module is the base class for adding modules to MongoDB
     * modules allow adding hooks and features to mongo
     * the idea is to add hooks into the main code for module support where needed
     * some ideas are: monitoring, indexes, full text search
     */
    class Module {
    public:
        Module( const string& name );
        virtual ~Module();

        boost::program_options::options_description_easy_init add_options() {
            return _options.add_options();
        }

        /**
         * read config from command line
         */
        virtual void config( boost::program_options::variables_map& params ) = 0;

        /**
         * called after configuration when the server is ready start
         */
        virtual void init() = 0;

        /**
         * called when the database is about to shutdown
         */
        virtual void shutdown() = 0;

        const string& getName() { return _name; }

        // --- static things

        static void addOptions( boost::program_options::options_description& options );
        static void configAll( boost::program_options::variables_map& params );
        static void initAll();

    private:
        static std::list<Module*> * _all;
        string _name;
        boost::program_options::options_description _options;
    };
}
