// distlock_test.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "../pch.h"
#include "dbclient.h"
#include "distlock.h"
#include "../db/commands.h"

namespace mongo {

    class TestDistLockWithSync : public Command {
    public:
        TestDistLockWithSync() : Command( "_testDistLockWithSyncCluster" ) {}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }

        static void runThread() {
            while ( keepGoing ) {
                if ( current->lock_try( "test" ) ) {
                    count++;
                    int before = count;
                    sleepmillis( 3 );
                    int after = count;
                    
                    if ( after != before ) {
                        error() << " before: " << before << " after: " << after << endl;
                    }
                    
                    current->unlock();
                }
            }
        }
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            Timer t;
            DistributedLock lk( ConnectionString( cmdObj["host"].String() , ConnectionString::SYNC ), "testdistlockwithsync" );
            current = &lk;
            count = 0;
            gotit = 0;
            errors = 0;
            keepGoing = true;
            
            vector<shared_ptr<boost::thread> > l;
            for ( int i=0; i<4; i++ ) {
                l.push_back( shared_ptr<boost::thread>( new boost::thread( runThread ) ) );
            }
            
            int secs = 10;
            if ( cmdObj["secs"].isNumber() )
                secs = cmdObj["secs"].numberInt();
            sleepsecs( secs );
            keepGoing = false;

            for ( unsigned i=0; i<l.size(); i++ )
                l[i]->join();

            current = 0;

            result.append( "count" , count );
            result.append( "gotit" , gotit );
            result.append( "errors" , errors );
            result.append( "timeMS" , t.millis() );

            return errors == 0;
        }
        
        // variables for test
        static DistributedLock * current;
        static int gotit;
        static int errors;
        static AtomicUInt count;
        
        static bool keepGoing;

    } testDistLockWithSyncCmd;


    DistributedLock * TestDistLockWithSync::current;
    AtomicUInt TestDistLockWithSync::count;
    int TestDistLockWithSync::gotit;
    int TestDistLockWithSync::errors;
    bool TestDistLockWithSync::keepGoing;


}
