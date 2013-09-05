// module.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/util/options_parser/option_section.h"

#include "mongo/db/module.h"

namespace mongo {

    namespace moe = mongo::optionenvironment;

    std::list<Module*> * Module::_all;

    Module::Module( const string& name )
        : _name( name ) {
        if ( ! _all )
            _all = new list<Module*>();
        _all->push_back( this );
    }

    Module::~Module() {}

    void Module::addAllOptions(moe::OptionSection* options) {
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ) {
            Module* m = *i;
            m->addOptions(options);
        }
    }

    void Module::configAll(moe::Environment& params) {
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ) {
            Module* m = *i;
            m->config( params );
        }

    }


    void Module::initAll() {
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ) {
            Module* m = *i;
            m->init();
        }

    }

}
