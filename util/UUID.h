// UUID.h

#pragma once

#include <uuid.h>
#include <string>


namespace mongo {

    class UUID {
    public:
        UUID();
        UUID( std::string s );
        ~UUID();
        
        std::string string() const;
        
        bool operator==( const UUID& other) const;
        bool operator!=( const UUID& other) const;

    private:
        unsigned char _data[16];
    };
    
    std::ostream& operator<<( std::ostream &s, const UUID &id );

}
