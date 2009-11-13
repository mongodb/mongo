// quorum.h

#include "../stdafx.h"
#include "dbclient.h"

namespace mongo {

    /**
     * this is a connection to a cluster of servers that operate as one
     * for super high durability
     */
    class QuorumConnection : public DBClientWithCommands {
    public:
        QuorumConnection( string a , string b , string c );
        ~QuorumConnection();
        
        
        /**
         * @return true if all servers are up and ready for writes
         */
        bool prepare( string& errmsg );

        /**
         * runs fsync on all servers
         */
        bool fsync( string& errmsg );
        
    private:
        
        void _connect( string host );
        vector<DBClientConnection*> _conns;
    };

};
