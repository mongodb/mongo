// UUID.cpp

#include "UUID.h"

#include <iostream>

using namespace std;

namespace mongo {
    
    UUID::UUID(){
        uuid_generate( _data );
    }
    
    UUID::UUID( std::string s ){
        uuid_parse( s.c_str() , _data );
    }
    
    UUID::~UUID(){
        uuid_clear( _data );
    }
    
    string UUID::string() const{
        char buf[33];
        uuid_unparse( _data , buf );
        
        std::string s;
        s += buf;
        return s;
    }
    
    bool UUID::operator==( const UUID& other) const{
        return uuid_compare( _data , other._data ) == 0;
    }


    bool UUID::operator!=( const UUID& other) const{
        return uuid_compare( _data , other._data );
    }
    
    ostream& operator<<( ostream &out , const UUID &id ){
        out << id.string();
        return out;
    }
    
}

