// -------------------------------------------------------------------
//
// perf_count.h:  performance counters LevelDB
//
// Copyright (c) 2012-2013 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#ifndef STORAGE_LEVELDB_INCLUDE_PERF_COUNT_H_
#define STORAGE_LEVELDB_INCLUDE_PERF_COUNT_H_

#include "leveldb_wt_config.h"

#include <stdint.h>
#include <string>
#include "status.h"

namespace leveldb {

enum SstCountEnum
{
    //
    // array index values/names
    //
    eSstCountKeys=0,           //!< how many keys in this sst
    eSstCountBlocks=1,         //!< how many blocks in this sst
    eSstCountCompressAborted=2,//!< how many blocks attempted compression and aborted use
    eSstCountKeySize=3,        //!< byte count of all keys
    eSstCountValueSize=4,      //!< byte count of all values
    eSstCountBlockSize=5,      //!< byte count of all blocks (pre-compression)
    eSstCountBlockWriteSize=6, //!< post-compression size, or BlockSize if no compression
    eSstCountIndexKeys=7,      //!< how many keys in the index block
    eSstCountKeyLargest=8,     //!< largest key in sst
    eSstCountKeySmallest=9,    //!< smallest key in sst
    eSstCountValueLargest=10,  //!< largest value in sst
    eSstCountValueSmallest=11, //!< smallest value in sst
    eSstCountDeleteKey=12,     //!< tombstone count
    eSstCountBlockSizeUsed=13, //!< Options::block_size used with this file
    eSstCountUserDataSize=14,  //!< post-compression size of non-metadata (user keys/values/block overhead)

    // must follow last index name to represent size of array
    eSstCountEnumSize,          //!< size of the array described by the enum values

    eSstCountVersion=1

};  // enum SstCountEnum


class SstCounters
{
protected:
    bool m_IsReadOnly;         //!< set when data decoded from a file
    uint32_t m_Version;        //!< object revision identification
    uint32_t m_CounterSize;    //!< number of objects in m_Counter

    uint64_t m_Counter[eSstCountEnumSize];

public:
    // constructors / destructor
    SstCounters();

    // Put data into disk form
    void EncodeTo(std::string & Dst) const;

    // Populate member data from prior EncodeTo block
    Status DecodeFrom(const Slice& src);

    // increment the counter
    uint64_t Inc(unsigned Index);

    // add value to the counter
    uint64_t Add(unsigned Index, uint64_t Amount);

    // return value of a counter
    uint64_t Value(unsigned Index) const;

    // set a value
    void Set(unsigned Index, uint64_t);

    // return number of counters
    uint32_t Size() const {return(m_CounterSize);};

    // printf all values
    void Dump() const;

};  // class SstCounters


extern struct PerformanceCounters * gPerfCounters;


enum PerformanceCountersEnum
{
    //
    // array index values/names
    //  (enum explicitly numbered to allow future edits / moves / inserts)
    //
    ePerfROFileOpen=0,      //!< PosixMmapReadableFile open
    ePerfROFileClose=1,     //!<  closed
    ePerfROFileUnmap=2,     //!<  unmap without close

    ePerfRWFileOpen=3,      //!< PosixMmapFile open
    ePerfRWFileClose=4,     //!<  closed
    ePerfRWFileUnmap=5,     //!<  unmap without close

    ePerfApiOpen=6,         //!< Count of DB::Open completions
    ePerfApiGet=7,          //!< Count of DBImpl::Get completions
    ePerfApiWrite=8,        //!< Count of DBImpl::Get completions

    ePerfWriteSleep=9,      //!< DBImpl::MakeRoomForWrite called sleep
    ePerfWriteWaitImm=10,   //!< DBImpl::MakeRoomForWrite called Wait on Imm compact
    ePerfWriteWaitLevel0=11,//!< DBImpl::MakeRoomForWrite called Wait on Level0 compact
    ePerfWriteNewMem=12,    //!< DBImpl::MakeRoomForWrite created new memory log
    ePerfWriteError=13,     //!< DBImpl::MakeRoomForWrite saw bg_error_
    ePerfWriteNoWait=14,    //!< DBImpl::MakeRoomForWrite took no action

    ePerfGetMem=15,         //!< DBImpl::Get read from memory log
    ePerfGetImm=16,         //!< DBImpl::Get read from previous memory log
    ePerfGetVersion=17,     //!< DBImpl::Get read from Version object

    // code ASSUMES the levels are in numerical order,
    //  i.e. based off of ePerfSearchLevel0
    ePerfSearchLevel0=18,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel1=19,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel2=20,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel3=21,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel4=22,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel5=23,   //!< Version::Get read searched one or more files here
    ePerfSearchLevel6=24,   //!< Version::Get read searched one or more files here

    ePerfTableCached=25,    //!< TableCache::FindTable found table in cache
    ePerfTableOpened=26,    //!< TableCache::FindTable had to open table file
    ePerfTableGet=27,       //!< TableCache::Get used to retrieve a key

    ePerfBGCloseUnmap=28,   //!< PosixEnv::BGThreaed started Unmap/Close job
    ePerfBGCompactImm=29,   //!< PosixEnv::BGThreaed started compaction of Imm
    ePerfBGNormal=30,       //!< PosixEnv::BGThreaed started normal compaction job
    ePerfBGCompactLevel0=31,//!< PosixEnv::BGThreaed started compaction of Level0

    ePerfBlockFiltered=32,  //!< Table::BlockReader search stopped due to filter
    ePerfBlockFilterFalse=33,//!< Table::BlockReader gave a false positive for match
    ePerfBlockCached=34,    //!< Table::BlockReader found block in cache
    ePerfBlockRead=35,      //!< Table::BlockReader read block from disk
    ePerfBlockFilterRead=36,//!< Table::ReadMeta filter loaded from file
    ePerfBlockValidGet=37,  //!< Table::InternalGet has valid iterator

    ePerfDebug0=38,         //!< Developer debug counters, moveable
    ePerfDebug1=39,         //!< Developer debug counters, moveable
    ePerfDebug2=40,         //!< Developer debug counters, moveable
    ePerfDebug3=41,         //!< Developer debug counters, moveable
    ePerfDebug4=42,         //!< Developer debug counters, moveable

    ePerfReadBlockError=43, //!< crc or compression error in ReadBlock (format.cc)

    ePerfIterNew=44,        //!< Count of DBImpl::NewDBIterator calls
    ePerfIterNext=45,       //!< Count of DBIter::Next calls
    ePerfIterPrev=46,       //!< Count of DBIter::Prev calls
    ePerfIterSeek=47,       //!< Count of DBIter::Seek calls
    ePerfIterSeekFirst=48,  //!< Count of DBIter::SeekFirst calls
    ePerfIterSeekLast=49,   //!< Count of DBIter::SeekLast calls
    ePerfIterDelete=50,     //!< Count of DBIter::~DBIter

    ePerfElevelDirect=51,   //!< eleveldb's FindWaitingThread went direct to thread
    ePerfElevelQueued=52,   //!< eleveldb's FindWaitingThread queued work item
    ePerfElevelDequeued=53, //!< eleveldb's worker took item from backlog queue

    ePerfElevelRefCreate=54,//!< eleveldb RefObject constructed
    ePerfElevelRefDelete=55,//!< eleveldb RefObject destructed

    ePerfThrottleGauge=56,  //!< current throttle value
    ePerfThrottleCounter=57,//!< running throttle by seconds

    ePerfThrottleMicros0=58,//!< level 0 micros spent compacting
    ePerfThrottleKeys0=59,  //!< level 0 keys processed
    ePerfThrottleBacklog0=60,//!< backlog at time of posting (level0)
    ePerfThrottleCompacts0=61,//!< number of level 0 compactions

    ePerfThrottleMicros1=62,//!< level 1+ micros spent compacting
    ePerfThrottleKeys1=63,  //!< level 1+ keys processed
    ePerfThrottleBacklog1=64,//!< backlog at time of posting (level1+)
    ePerfThrottleCompacts1=65,//!< number of level 1+ compactions

    ePerfBGWriteError=66,   //!< error in write/close, see syslog

    ePerfThrottleWait=67,   //!< milliseconds of throttle wait
    ePerfThreadError=68,    //!< system error on thread related call, no LOG access

    ePerfBGImmDirect=69,    //!< count Imm compactions happened directly
    ePerfBGImmQueued=70,    //!< count Imm compactions placed on queue
    ePerfBGImmDequeued=71,  //!< count Imm compactions removed from queue
    ePerfBGImmWeighted=72,  //!< total microseconds item spent on queue

    ePerfBGUnmapDirect=73,  //!< count Unmap operations happened directly
    ePerfBGUnmapQueued=74,  //!< count Unmap operations placed on queue
    ePerfBGUnmapDequeued=75,//!< count Unmap operations removed from queue
    ePerfBGUnmapWeighted=76,//!< total microseconds item spent on queue

    ePerfBGLevel0Direct=77,  //!< count Level0 compactions happened directly
    ePerfBGLevel0Queued=78,  //!< count Level0 compactions placed on queue
    ePerfBGLevel0Dequeued=79,//!< count Level0 compactions removed from queue
    ePerfBGLevel0Weighted=80,//!< total microseconds item spent on queue

    ePerfBGCompactDirect=81,  //!< count generic compactions happened directly
    ePerfBGCompactQueued=82,  //!< count generic compactions placed on queue
    ePerfBGCompactDequeued=83,//!< count generic compactions removed from queue
    ePerfBGCompactWeighted=84,//!< total microseconds item spent on queue

    ePerfFileCacheInsert=85,  //!< total bytes inserted into file cache
    ePerfFileCacheRemove=86,  //!< total bytes removed from file cache

    ePerfBlockCacheInsert=87, //!< total bytes inserted into block cache
    ePerfBlockCacheRemove=88, //!< total bytes removed from block cache

    ePerfApiDelete=89,        //!< Count of DB::Delete

    // must follow last index name to represent size of array
    //  (ASSUMES previous enum is highest value)
    ePerfCountEnumSize,     //!< size of the array described by the enum values

    ePerfVersion=1,         //!< structure versioning
    ePerfKey=41207          //!< random number as shared memory identifier
};

//
// Do NOT use virtual functions.  This structure will be aligned at different
//  locations in multiple processes.  Things can get messy with virtuals.

struct PerformanceCounters
{
public:
    static int m_LastError;

protected:
    uint32_t m_Version;        //!< object revision identification
    uint32_t m_CounterSize;    //!< number of objects in m_Counter

    volatile uint64_t m_Counter[ePerfCountEnumSize];

    static const char * m_PerfCounterNames[];
    static int m_PerfSharedId;
    static volatile uint64_t m_BogusCounter;  //!< for out of range GetPtr calls

public:
    // only called for local object, not for shared memory
    PerformanceCounters();

    //!< does executable's idea of version match shared object?
    bool VersionTest()
        {return(ePerfCountEnumSize<=m_CounterSize && ePerfVersion==m_Version);};

    //!< mostly for perf_count_test.cc
    void SetVersion(uint32_t Version, uint32_t CounterSize)
    {m_Version=Version; m_CounterSize=CounterSize;};

    static PerformanceCounters * Init(bool IsReadOnly);
    static int Close(PerformanceCounters * Counts);

    uint64_t Inc(unsigned Index);
    uint64_t Dec(unsigned Index);

    // add value to the counter
    uint64_t Add(unsigned Index, uint64_t Amount);

    // return value of a counter
    uint64_t Value(unsigned Index) const;

    // set a value
    void Set(unsigned Index, uint64_t);

    volatile const uint64_t * GetPtr(unsigned Index) const;

    static const char * GetNamePtr(unsigned Index);

    int LookupCounter(const char * Name);

    void Dump();

};  // struct PerformanceCounters

extern PerformanceCounters * gPerfCounters;

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_PERF_COUNT_H_
