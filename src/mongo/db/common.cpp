/** @file common.cpp 
    Common code for server binaries (mongos, mongod, test).  
    Nothing used by driver should be here. 
 */

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

//#include "pch.h"
//#include "concurrency.h"
#include "jsobjmanipulator.h"

/**
 * this just has globals
 */
namespace mongo {

    /** called by mongos, mongod, test. do not call from clients and such. 
        invoked before about everything except global var construction.
     */
    void doPreServerStartupInits() { 
#if defined(RLIMIT_NPROC) && defined(RLIMIT_NOFILE)
      //Check that # of files rlmit > 1000 , and # of processes > # of files/2
      const unsigned int minNumFiles = 1000;
      const double filesToProcsRatio = 2.0;
      struct rlimit rlnproc;
      struct rlimit rlnofile;

      if(!getrlimit(RLIMIT_NPROC,&rlnproc) && !getrlimit(RLIMIT_NOFILE,&rlnofile)){
        if(rlnofile.rlim_cur < minNumFiles){
          log() << "Warning: soft rlimits too low. Number of files is " << rlnofile.rlim_cur << ", should be at least " << minNumFiles << endl;
        }
        if(rlnproc.rlim_cur < rlnofile.rlim_cur/filesToProcsRatio){
          log() << "Warning: soft rlimits too low. " << rlnproc.rlim_cur << " processes, " << rlnofile.rlim_cur << " files. Number of processes should be at least "<< 1/filesToProcsRatio << " times number of files." << endl;
        }
      }
      else{
        log() << "Warning: getrlimit failed" << endl;
      }
#endif
    }

    NOINLINE_DECL OpTime OpTime::skewed() {
        bool toLog = false;
        ONCE toLog = true;
        RARELY toLog = true;
        last.i++;
        if ( last.i & 0x80000000 )
            toLog = true;
        if ( toLog ) {
            log() << "clock skew detected  prev: " << last.secs << " now: " << (unsigned) time(0) << endl;
        }
        if ( last.i & 0x80000000 ) {
            log() << "error large clock skew detected, shutting down" << endl;
            throw ClockSkewException();
        }
        return last;
    }

}
