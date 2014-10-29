// wiredtiger_size_storer.h

#pragma once

#include <map>
#include <string>

#include <boost/thread/mutex.hpp>

#include "mongo/base/string_data.h"

namespace mongo {

    class WiredTigerSession;

    class WiredTigerSizeStorer {
    public:
        WiredTigerSizeStorer();
        ~WiredTigerSizeStorer();

        void store( const StringData& uri,
                    long long numRecords, long long dataSize );

        void load( const StringData& uri,
                   long long* numRecords, long long* dataSize ) const;

        void loadFrom( WiredTigerSession* cursor, const std::string& uri );
        void storeInto( WiredTigerSession* cursor, const std::string& uri ) const;

    private:
        void _checkMagic() const;

        struct Entry {
            long long numRecords;
            long long dataSize;
            bool dirty;
        };

        int _magic;

        typedef std::map<std::string,Entry> Map;
        Map _entries;
        mutable boost::mutex _entriesMutex;
    };

}
