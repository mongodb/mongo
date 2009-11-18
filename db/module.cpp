// module.cpp

#include "stdafx.h"
#include "module.h"

namespace mongo {

    std::list<Module*> Module::ALL;

    Module::Module( const string& name ) 
        : _name( name ) , _options( (string)"Module " + name + " options" ){
        ALL.push_back( this );
    }

    Module::~Module(){}
    
    void Module::addOptions( program_options::options_description& options ){
        for ( list<Module*>::iterator i=ALL.begin(); i!=ALL.end(); i++ ){
            Module* m = *i;
            options.add( m->_options );
        }
    }

    void Module::configAll( program_options::variables_map& params ){
        for ( list<Module*>::iterator i=ALL.begin(); i!=ALL.end(); i++ ){
            Module* m = *i;
            m->config( params );
        }

    }


    void Module::initAll(){
        for ( list<Module*>::iterator i=ALL.begin(); i!=ALL.end(); i++ ){
            Module* m = *i;
            m->init();
        }

    }

}
