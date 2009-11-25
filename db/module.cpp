// module.cpp

#include "stdafx.h"
#include "module.h"

namespace mongo {

    std::list<Module*> * Module::_all;

    Module::Module( const string& name )
        : _name( name ) , _options( (string)"Module " + name + " options" ){
        if ( ! _all )
            _all = new list<Module*>();
        _all->push_back( this );
    }

    Module::~Module(){}

    void Module::addOptions( program_options::options_description& options ){
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ){
            Module* m = *i;
            options.add( m->_options );
        }
    }

    void Module::configAll( program_options::variables_map& params ){
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ){
            Module* m = *i;
            m->config( params );
        }

    }


    void Module::initAll(){
        if ( ! _all ) {
            return;
        }
        for ( list<Module*>::iterator i=_all->begin(); i!=_all->end(); i++ ){
            Module* m = *i;
            m->init();
        }

    }

}
