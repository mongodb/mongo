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
        /**
         * @param commaSeperated should be 3 hosts comma seperated
         */
        QuorumConnection( string commaSeperated );
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

        // --- from DBClientInterface

        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn, int nToSkip,
                                               const BSONObj *fieldsToReturn, int queryOptions);

        virtual auto_ptr<DBClientCursor> getMore( const string &ns, long long cursorId, int nToReturn, int options );
        
        virtual void insert( const string &ns, BSONObj obj );
        
        virtual void insert( const string &ns, const vector< BSONObj >& v );

        virtual void remove( const string &ns , Query query, bool justOne );

        virtual void update( const string &ns , Query query , BSONObj obj , bool upsert , bool multi );

        virtual string toString();
    private:
        
        void _checkLast();
        
        void _connect( string host );
        vector<DBClientConnection*> _conns;
    };
    

};
