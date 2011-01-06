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
            for ( int i=0; i<1000; i++ ) {
                if ( current->lock_try( "test" ) ) {
                    gotit++;
                    for ( int j=0; j<2000; j++ ) {
                        count++;
                    }
                    current->unlock();
                }
            }
        }

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
            DistributedLock lk( ConnectionString( cmdObj["host"].String() , ConnectionString::SYNC ), "testdistlockwithsync" );
            current = &lk;
            count = 0;
            gotit = 0;

            vector<shared_ptr<boost::thread> > l;
            for ( int i=0; i<4; i++ ) {
                l.push_back( shared_ptr<boost::thread>( new boost::thread( runThread ) ) );
            }

            for ( unsigned i=0; i<l.size(); i++ )
                l[i]->join();

            result.append( "count" , count );
            result.append( "gotit" , gotit );
            current = 0;
            return count == gotit * 2000;
        }

        static DistributedLock * current;
        static int count;
        static int gotit;

    } testDistLockWithSyncCmd;


    DistributedLock * TestDistLockWithSync::current;
    int TestDistLockWithSync::count;
    int TestDistLockWithSync::gotit;


}
