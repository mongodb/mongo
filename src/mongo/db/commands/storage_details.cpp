/*    Copyright 2012 10gen Inc.
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

#include "mongo/pch.h"

#include <ctime>
#include <string>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/namespace_details.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

    // Helper classes and functions

    /**
     * Available subcommands.
     */
    enum SubCommand {
        SUBCMD_DISK_STORAGE,
        SUBCMD_PAGES_IN_RAM
    };

    /**
     * Simple struct to store various operation parameters to be passed around during analysis.
     */
    struct AnalyzeParams {
        // startOfs and endOfs are extent-relative
        int startOfs;
        int endOfs;
        int length;
        int numberOfSlices;
        int granularity;
        int lastSliceLength;
        string characteristicField;
        bool processDeletedRecords;
        bool showRecords;
        time_t startTime;

        AnalyzeParams() : startOfs(0), endOfs(INT_MAX), length(INT_MAX), numberOfSlices(0),
                          granularity(0), lastSliceLength(0), characteristicField("_id"),
                          processDeletedRecords(true), showRecords(false), startTime(time(NULL)) {
        }
    };

    /**
     * Aggregated information per slice / extent.
     */
    struct DiskStorageData {
        double numEntries;
        long long bsonBytes;
        long long recBytes;
        long long onDiskBytes;
        double characteristicSum;
        double characteristicCount;
        double outOfOrderRecs;
        double freeRecords[mongo::Buckets];

        DiskStorageData(long long diskBytes) : numEntries(0), bsonBytes(0), recBytes(0),
                                               onDiskBytes(diskBytes), characteristicSum(0),
                                               characteristicCount(0), outOfOrderRecs(0),
                                               freeRecords() /* initialize array with zeroes */ {
        }

        const DiskStorageData& operator += (const DiskStorageData& rhs) {
            this->numEntries += rhs.numEntries;
            this->recBytes += rhs.recBytes;
            this->bsonBytes += rhs.bsonBytes;
            this->onDiskBytes += rhs.onDiskBytes;
            this->characteristicSum += rhs.characteristicSum;
            this->characteristicCount += rhs.characteristicCount;
            this->outOfOrderRecs += rhs.outOfOrderRecs;
            for (int i = 0; i < mongo::Buckets; i++) {
                this->freeRecords[i] += rhs.freeRecords[i];
            }
            return *this;
        }

        void appendToBSONObjBuilder(BSONObjBuilder& b, bool includeFreeRecords) const {
            b.append("numEntries", numEntries);
            b.append("bsonBytes", bsonBytes);
            b.append("recBytes", recBytes);
            b.append("onDiskBytes", onDiskBytes);
            b.append("outOfOrderRecs", outOfOrderRecs);
            if (characteristicCount > 0) {
                b.append("characteristicCount", characteristicCount);
                b.append("characteristicAvg", characteristicSum / characteristicCount);
            }
            if (includeFreeRecords) {
                BSONArrayBuilder freeRecsPerBucketArrBuilder(b.subarrayStart("freeRecsPerBucket"));
                for (int i = 0; i < mongo::Buckets; i++) {
                    freeRecsPerBucketArrBuilder.append(freeRecords[i]);
                }
                freeRecsPerBucketArrBuilder.doneFast();
            }
        }
    };

    /**
     * Helper to calculate which slices the current record overlaps and how much of the record
     * is in each of them.
     * E.g.
     *                 3.5M      4M     4.5M      5M      5.5M       6M
     *     slices ->    |   12   |   13   |   14   |   15   |   16   |
     *     record ->         [-------- 1.35M --------]
     *
     * results in something like:
     *     firstSliceNum = 12
     *     lastSliceNum = 15
     *     sizeInFirstSlice = 0.25M
     *     sizeInLastSlice = 0.10M
     *     sizeInMiddleSlice = 0.5M (== size of slice)
     *     inFirstSliceRatio = 0.25M / 1.35M = 0.185...
     *     inLastSliceRatio = 0.10M / 1.35M = 0.074...
     *     inMiddleSliceRatio = 0.5M / 1.35M = 0.37...
     *
     * The quasi-iterator SliceIterator is available to easily iterate over the slices spanned
     * by the record and to obtain how much of the records belongs to each.
     *
     *    for (RecPos::SliceIterator it = pos.iterateSlices(); !it.end(); ++it) {
     *        RecPos::SliceInfo res = *it;
     *        // res contains the current slice number, the number of bytes belonging to the current
     *        // slice, and the ratio with the full size of the record
     *    }
     *
     */
    struct RecPos {
        bool outOfRange;
        int firstSliceNum;
        int lastSliceNum;
        int sizeInFirstSlice;
        int sizeInLastSlice;
        int sizeInMiddleSlice;
        double inFirstSliceRatio;
        double inLastSliceRatio;
        double inMiddleSliceRatio;
        int numberOfSlices;

        /**
         * Calculate position of record among slices.
         * @param recOfs record offset as reported by DiskLoc
         * @param recLen record on-disk size with headers included
         * @param extentOfs extent offset as reported by DiskLoc
         * @param params operation parameters (see AnalyzeParams for details)
         */
        static RecPos from(int recOfs, int recLen, int extentOfs, const AnalyzeParams& params) {
            RecPos res;
            res.numberOfSlices = params.numberOfSlices;
            // startsAt and endsAt are extent-relative
            int startsAt = recOfs - extentOfs;
            int endsAt = startsAt + recLen;
            if (endsAt < params.startOfs || startsAt >= params.endOfs) {
                res.outOfRange = true;
                return res;
            }
            else {
                res.outOfRange = false;
            }
            res.firstSliceNum = (startsAt - params.startOfs) / params.granularity;
            res.lastSliceNum = (endsAt - params.startOfs) / params.granularity;

            // extent-relative
            int endOfFirstSlice = (res.firstSliceNum + 1) * params.granularity + params.startOfs;
            res.sizeInFirstSlice = min(endOfFirstSlice - startsAt, recLen);
            res.sizeInMiddleSlice = params.granularity;
            res.sizeInLastSlice = recLen - res.sizeInFirstSlice -
                                  params.granularity * (res.lastSliceNum - res.firstSliceNum
                                                        - 1);
            if (res.sizeInLastSlice < 0) {
                res.sizeInLastSlice = 0;
            }
            res.inFirstSliceRatio = static_cast<double>(res.sizeInFirstSlice) / recLen;
            res.inMiddleSliceRatio = static_cast<double>(res.sizeInMiddleSlice) / recLen;
            res.inLastSliceRatio = static_cast<double>(res.sizeInLastSlice) / recLen;
            return res;
        }

        // See RecPos class description
        struct SliceInfo {
            int sliceNum;
            int sizeHere;
            double ratioHere;
        };

        /**
         * Iterates over slices spanned by the record.
         */
        class SliceIterator {
        public:
            SliceIterator(RecPos& pos) : _pos(pos), _valid(false) {
                _curSlice.sliceNum = pos.firstSliceNum >= 0 ? _pos.firstSliceNum : 0;
            }

            bool end() const {
                return _pos.outOfRange 
                    || _curSlice.sliceNum >= _pos.numberOfSlices
                    || _curSlice.sliceNum > _pos.lastSliceNum;
            }

            SliceInfo* operator->() {
                return get();
            }

            SliceInfo& operator*() {
                return *(get());
            }

            // preincrement
            SliceIterator& operator++() {
                _curSlice.sliceNum++;
                _valid = false;
                return *this;
            }

        private:
            SliceInfo* get() {
                verify(!end());
                if (!_valid) {
                    if (_curSlice.sliceNum == _pos.firstSliceNum) {
                        _curSlice.sizeHere = _pos.sizeInFirstSlice;
                        _curSlice.ratioHere = _pos.inFirstSliceRatio;
                    }
                    else if (_curSlice.sliceNum == _pos.lastSliceNum) {
                        _curSlice.sizeHere = _pos.sizeInLastSlice;
                        _curSlice.ratioHere = _pos.inLastSliceRatio;
                    }
                    else {
                        DEV verify(_pos.firstSliceNum < _curSlice.sliceNum &&
                                   _curSlice.sliceNum < _pos.lastSliceNum);
                        _curSlice.sizeHere = _pos.sizeInMiddleSlice;
                        _curSlice.ratioHere = _pos.inMiddleSliceRatio;
                    }
                    verify(_curSlice.sizeHere >= 0 && _curSlice.ratioHere >= 0);
                    _valid = true;
                }
                return &_curSlice;
            }

            RecPos& _pos;
            SliceInfo _curSlice;

            // if _valid, data in _curSlice refers to the current slice, otherwise it needs
            // to be computed
            bool _valid;
        };

        SliceIterator iterateSlices() {
            return SliceIterator(*this);
        }
    };

    inline unsigned ceilingDiv(unsigned dividend, unsigned divisor) {
        return (dividend + divisor - 1) / divisor;
    }

    // Command

    /**
     * This command provides detailed and aggreate information regarding record and deleted record
     * layout in storage files and in memory.
     */
    class StorageDetailsCmd : public Command {
    public:
        StorageDetailsCmd() : Command( "storageDetails" ) {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help(stringstream& h) const {
            h << "EXPERIMENTAL (UNSUPPORTED). "
              << "Provides detailed and aggregate information regarding record and deleted record "
              << "layout in storage files ({analyze: 'diskStorage'}) and percentage of pages "
              << "currently in RAM ({analyze: 'pagesInRAM'}). Slow if run on large collections. "
              << "Select the desired subcommand with {analyze: 'diskStorage' | 'pagesInRAM'}; "
              << "specify {extent: num_} and, optionally, {range: [start, end]} to restrict "
              << "processing to a single extent (start and end are offsets from the beginning of "
              << "the extent. {granularity: bytes} or {numberOfSlices: num_} enable aggregation of "
              << "statistic per-slice: the extent(s) will either be subdivided in about "
              << "'numberOfSlices' slices or in slices of size 'granularity'. "
              << "{characteristicField: dotted_path} enables collection of a field to make "
              << "it possible to identify which kind of record belong to each slice/extent. "
              << "{showRecords: true} enables a dump of all records and deleted records "
              << "encountered. Example: "
              << "{storageDetails: 'collectionName', analyze: 'diskStorage', granularity: 1 << 20}";
        }

        virtual LockType locktype() const { return READ; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::storageDetails);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }

    private:
        /**
         * Entry point, parses command parameters and invokes runInternal.
         */
        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg,
                 BSONObjBuilder& result, bool fromRepl);

    };

    MONGO_INITIALIZER(StorageDetailsCmd)(InitializerContext* context) {
        if (cmdLine.experimental.storageDetailsCmdEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new StorageDetailsCmd();
        }
        return Status::OK();
    }

    /**
     * Extracts the characteristic field from the document, if present and of the type ObjectId,
     * Date or numeric.
     * @param obj the document
     * @param value out: characteristic field value, only valid if true is returned
     * @return true if field was correctly extracted, false otherwise (missing or of wrong type)
     */
    bool extractCharacteristicFieldValue(BSONObj& obj, const AnalyzeParams& params, double& value) {
        BSONElement elem = obj.getFieldDotted(params.characteristicField);
        if (elem.eoo()) {
            return false;
        }
        bool hasValue = false;
        if (elem.type() == jstOID) {
            value = static_cast<double>(elem.OID().asTimeT());
            hasValue = true;
        }
        else if (elem.isNumber()) {
            value = elem.numberDouble();
            hasValue = true;
        }
        else if (elem.type() == mongo::Date) {
            value = static_cast<double>(elem.date().toTimeT());
            hasValue = true;
        }
        return hasValue;
    }

    /**
     * @return the requested extent if it exists, otherwise NULL
     */
    const Extent* getNthExtent(int extentNum, const NamespaceDetails* nsd) {
        int curExtent = 0;
        for (Extent* ex = DataFileMgr::getExtent(nsd->firstExtent());
             ex != NULL;
             ex = ex->getNextExtent()) {

            if (curExtent == extentNum) return ex;
            curExtent++;
        }
        return NULL;
    }

    /**
     * analyzeDiskStorage helper which processes a single record.
     */
    void processDeletedRecord(const DiskLoc& dl, const DeletedRecord* dr, const Extent* ex,
                              const AnalyzeParams& params, int bucketNum,
                              vector<DiskStorageData>& sliceData,
                              BSONArrayBuilder* deletedRecordsArrayBuilder) {

        killCurrentOp.checkForInterrupt();

        int extentOfs = ex->myLoc.getOfs();

        if (! (dl.a() == ex->myLoc.a() &&
               dl.getOfs() + dr->lengthWithHeaders() > extentOfs &&
               dl.getOfs() < extentOfs + ex->length) ) {

            return;
        }

        RecPos pos = RecPos::from(dl.getOfs(), dr->lengthWithHeaders(), extentOfs, params);
        bool spansRequestedArea = false;
        for (RecPos::SliceIterator it = pos.iterateSlices(); !it.end(); ++it) {

            DiskStorageData& slice = sliceData[it->sliceNum];
            slice.freeRecords[bucketNum] += it->ratioHere;
            spansRequestedArea = true;
        }

        if (deletedRecordsArrayBuilder != NULL && spansRequestedArea) {
            BSONObjBuilder(deletedRecordsArrayBuilder->subobjStart())
                .append("ofs", dl.getOfs() - extentOfs)
                .append("recBytes", dr->lengthWithHeaders());
        }
    }

    /**
     * analyzeDiskStorage helper which processes a single record.
     */
    void processRecord(const DiskLoc& dl, const DiskLoc& prevDl, const Record* r, int extentOfs,
                       const AnalyzeParams& params, vector<DiskStorageData>& sliceData,
                       BSONArrayBuilder* recordsArrayBuilder) {
        killCurrentOp.checkForInterrupt();

        BSONObj obj = dl.obj();
        int recBytes = r->lengthWithHeaders();
        double characteristicFieldValue = 0;
        bool hasCharacteristicField = extractCharacteristicFieldValue(obj, params,
                                                                      characteristicFieldValue);
        bool isLocatedBeforePrevious = dl.a() < prevDl.a();

        RecPos pos = RecPos::from(dl.getOfs(), recBytes, extentOfs, params);
        bool spansRequestedArea = false;
        for (RecPos::SliceIterator it = pos.iterateSlices(); !it.end(); ++it) {
            spansRequestedArea = true;
            DiskStorageData& slice = sliceData[it->sliceNum];
            slice.numEntries += it->ratioHere;
            slice.recBytes += it->sizeHere;
            slice.bsonBytes += static_cast<long long>(it->ratioHere * obj.objsize());
            if (hasCharacteristicField) {
                slice.characteristicCount += it->ratioHere;
                slice.characteristicSum += it->ratioHere * characteristicFieldValue;
            }
            if (isLocatedBeforePrevious) {
                slice.outOfOrderRecs += it->ratioHere;
            }
        }

        if (recordsArrayBuilder != NULL && spansRequestedArea) {
            DEV {
                int startsAt = dl.getOfs() - extentOfs;
                int endsAt = startsAt + recBytes;
                verify((startsAt < params.startOfs && endsAt > params.startOfs) ||
                       (startsAt < params.endOfs && endsAt >= params.endOfs) ||
                       (startsAt >= params.startOfs && endsAt < params.endOfs));
            }
            BSONObjBuilder recordBuilder(recordsArrayBuilder->subobjStart());
            recordBuilder.append("ofs", dl.getOfs() - extentOfs);
            recordBuilder.append("recBytes", recBytes);
            recordBuilder.append("bsonBytes", obj.objsize());
            recordBuilder.append("_id", obj["_id"]);
            if (hasCharacteristicField) {
                recordBuilder.append("characteristic", characteristicFieldValue);
            }
            recordBuilder.doneFast();
        }
    }

    // Top-level analysis functions

    /**
     * Provides aggregate and (if requested) detailed information regarding the layout of
     * records and deleted records in the extent.
     * The extent is split in params.numberOfSlices slices of params.granularity bytes each
     * (except the last one which could be shorter).
     * Iteration is performed over all records and deleted records in the specified (part of)
     * extent and the output contains aggregate information for the entire record and per-slice.
     * The typical output has the form:
     *
     *     { extentHeaderBytes: <size>,
     *       recordHeaderBytes: <size>,
     *       range: [startOfs, endOfs],     // extent-relative
     *       numEntries: <number of records>,
     *       bsonBytes: <total size of the bson objects>,
     *       recBytes: <total size of the valid records>,
     *       onDiskBytes: <length of the extent or range>,
     * (opt) characteristicCount: <number of records containing the field used to tell them apart>
     *       characteristicAvg: <average value of the characteristic field>
     *       outOfOrderRecs: <number of records that follow - in the record linked list -
     *                        a record that is located further in the extent>
     * (opt) freeRecsPerBucket: [ ... ],
     * The nth element in the freeRecsPerBucket array is the count of deleted records in the
     * nth bucket of the deletedList.
     * The characteristic field dotted path is specified in params.characteristicField.
     * If its value is an OID or Date, the timestamp (as seconds since epoch) will be extracted;
     * numeric values are converted to double and other bson types are ignored.
     *
     * The list of slices follows, with similar information aggregated per-slice:
     *       slices: [
     *           { numEntries: <number of records>,
     *             ...
     *             freeRecsPerBucket: [ ... ]
     *           },
     *           ...
     *       ]
     *     }
     *
     * If params.showRecords is set two additional fields are added to the outer document:
     *       records: [
     *           { ofs: <record offset from start of extent>,
     *             recBytes: <record size>,
     *             bsonBytes: <bson document size>,
     *  (optional) characteristic: <value of the characteristic field>
     *           }, 
     *           ... (one element per record)
     *       ],
     *  (optional) deletedRecords: [
     *           { ofs: <offset from start of extent>,
     *             recBytes: <deleted record size>
     *           },
     *           ... (one element per deleted record)
     *       ]
     *
     * @return true on success, false on failure (partial output may still be present)
     */
    bool analyzeDiskStorage(const NamespaceDetails* nsd, const Extent* ex,
                                               const AnalyzeParams& params, string& errmsg,
                                               BSONObjBuilder& result) {
        bool isCapped = nsd->isCapped();

        result.append("extentHeaderBytes", Extent::HeaderSize());
        result.append("recordHeaderBytes", Record::HeaderSize);
        result.append("range", BSON_ARRAY(params.startOfs << params.endOfs));
        result.append("isCapped", isCapped);

        vector<DiskStorageData> sliceData(params.numberOfSlices,
                                          DiskStorageData(params.granularity));
        sliceData[params.numberOfSlices - 1].onDiskBytes = params.lastSliceLength;
        Record* r;
        int extentOfs = ex->myLoc.getOfs();

        scoped_ptr<BSONArrayBuilder> recordsArrayBuilder;
        if (params.showRecords) {
            recordsArrayBuilder.reset(new BSONArrayBuilder(result.subarrayStart("records")));
        }

        Database* db = cc().database();
        ExtentManager& extentManager = db->getExtentManager();

        DiskLoc prevDl = ex->firstRecord;
        for (DiskLoc dl = ex->firstRecord; !dl.isNull(); dl = extentManager.getNextRecordInExtent(dl)) {
            r = dl.rec();
            processRecord(dl, prevDl, r, extentOfs, params, sliceData,
                          recordsArrayBuilder.get());
            prevDl = dl;
        }
        if (recordsArrayBuilder.get() != NULL) {
            recordsArrayBuilder->doneFast();
        }

        bool processingDeletedRecords = !isCapped && params.processDeletedRecords;

        scoped_ptr<BSONArrayBuilder> deletedRecordsArrayBuilder;
        if (params.showRecords) {
            deletedRecordsArrayBuilder.reset(
                    new BSONArrayBuilder(result.subarrayStart("deletedRecords")));
        }

        if (processingDeletedRecords) {
            for (int bucketNum = 0; bucketNum < mongo::Buckets; bucketNum++) {
                DiskLoc dl = nsd->deletedListEntry(bucketNum);
                while (!dl.isNull()) {
                    DeletedRecord* dr = dl.drec();
                    processDeletedRecord(dl, dr, ex, params, bucketNum, sliceData,
                                         deletedRecordsArrayBuilder.get());
                    dl = dr->nextDeleted();
                }
            }
        }
        if (deletedRecordsArrayBuilder.get() != NULL) {
            deletedRecordsArrayBuilder->doneFast();
        }

        DiskStorageData extentData(0);
        if (sliceData.size() > 1) {
            BSONArrayBuilder sliceArrayBuilder (result.subarrayStart("slices"));
            for (vector<DiskStorageData>::iterator it = sliceData.begin();
                 it != sliceData.end(); ++it) {

                killCurrentOp.checkForInterrupt();
                extentData += *it;
                BSONObjBuilder sliceBuilder;
                it->appendToBSONObjBuilder(sliceBuilder, processingDeletedRecords);
                sliceArrayBuilder.append(sliceBuilder.obj());
            }
            sliceArrayBuilder.doneFast();
            extentData.appendToBSONObjBuilder(result, processingDeletedRecords);
        } else {
            sliceData[0].appendToBSONObjBuilder(result, processingDeletedRecords);
        }
        return true;
    }

    /**
     * Outputs which percentage of pages are in memory for the entire extent and per-slice.
     * Refer to analyzeDiskStorage for a description of what slices are.
     *
     * The output has the form:
     *     { pageBytes: <system page size>,
     *       onDiskBytes: <size of the extent>,
     *       inMem: <ratio of pages in memory for the entire extent>,
     * (opt) slices: [ ... ] (only present if either params.granularity or numberOfSlices is not
     *                        zero and there exist more than one slice for this extent)
     * (opt) sliceBytes: <size of each slice>
     *     }
     *
     * The nth element in the slices array is the ratio of pages in memory for the nth slice.
     *
     * @return true on success, false on failure (partial output may still be present)
     */
    bool analyzePagesInRAM(const Extent* ex, const AnalyzeParams& params, string& errmsg,
                           BSONObjBuilder& result) {

        verify(sizeof(char) == 1);
        int pageBytes = static_cast<int>(ProcessInfo::getPageSize());
        result.append("pageBytes", pageBytes);
        result.append("onDiskBytes", ex->length - Extent::HeaderSize());
        char* startAddr = (char*) ex + params.startOfs;

        int extentPages = ceilingDiv(params.endOfs - params.startOfs, pageBytes);
        int extentInMemCount = 0; // number of pages in memory for the entire extent

        scoped_ptr<BSONArrayBuilder> arr;
        int sliceBytes = params.granularity;

        if (params.numberOfSlices > 1) {
            result.append("sliceBytes", sliceBytes);
            arr.reset(new BSONArrayBuilder(result.subarrayStart("slices")));
        }

        for (int slice = 0; slice < params.numberOfSlices; slice++) {
            if (slice == params.numberOfSlices - 1) {
                sliceBytes = params.lastSliceLength;
            }
            int pagesInSlice = ceilingDiv(sliceBytes, pageBytes);

            const char* firstPageAddr = startAddr + (slice * params.granularity);
            vector<char> isInMem;
            if (! ProcessInfo::pagesInMemory(firstPageAddr, pagesInSlice, &isInMem)) {
                errmsg = "system call failed";
                return false;
            }

            int inMemCount = 0;
            for (int page = 0; page < pagesInSlice; page++) {
                if (isInMem[page]) {
                    inMemCount++;
                    extentInMemCount++;
                }
            }

            if (arr.get() != NULL) {
                arr->append(static_cast<double>(inMemCount) / pagesInSlice);
            }
        }
        if (arr.get() != NULL) {
            arr->doneFast();
        }

        result.append("inMem", static_cast<double>(extentInMemCount) / extentPages);

        return true;
    }

    /**
     * Analyze a single extent.
     * @param params analysis parameters, will be updated with computed number of slices or
     *               granularity
     */
    bool analyzeExtent(const NamespaceDetails* nsd, const Extent* ex, SubCommand subCommand,
                       AnalyzeParams& params, string& errmsg, BSONObjBuilder& outputBuilder) {

        params.startOfs = max(0, params.startOfs);
        params.endOfs = min(params.endOfs, ex->length);
        params.length = params.endOfs - params.startOfs;
        if (params.numberOfSlices != 0) {
            params.granularity = (params.endOfs - params.startOfs + params.numberOfSlices - 1) /
                                 params.numberOfSlices;
        }
        else if (params.granularity != 0) {
            params.numberOfSlices = ceilingDiv(params.length, params.granularity);
        }
        else {
            params.numberOfSlices = 1;
            params.granularity = params.length;
        }
        params.lastSliceLength = params.length -
                (params.granularity * (params.numberOfSlices - 1));
        switch (subCommand) {
            case SUBCMD_DISK_STORAGE:
                return analyzeDiskStorage(nsd, ex, params, errmsg, outputBuilder);
            case SUBCMD_PAGES_IN_RAM:
                return analyzePagesInRAM(ex, params, errmsg, outputBuilder);
        }
        verify(false && "unreachable");
    }

    /**
     * @param ex requested extent; if NULL analyze entire namespace
     */ 
    bool runInternal(const NamespaceDetails* nsd, const Extent* ex, SubCommand subCommand,
                     AnalyzeParams& globalParams, string& errmsg, BSONObjBuilder& result) {

        BSONObjBuilder outputBuilder; // temporary builder to avoid output corruption in case of
                                      // failure
        bool success = false;
        if (ex != NULL) {
            success = analyzeExtent(nsd, ex, subCommand, globalParams, errmsg, outputBuilder);
        }
        else {
            const DiskLoc dl = nsd->firstExtent();
            if (dl.isNull()) {
                errmsg = "no extents in namespace";
                return false;
            }

            long long storageSize = nsd->storageSize(NULL, NULL);

            if (globalParams.numberOfSlices != 0) {
                globalParams.granularity = ceilingDiv(storageSize, globalParams.numberOfSlices);
            }

            BSONArrayBuilder extentsArrayBuilder(outputBuilder.subarrayStart("extents"));
            for (Extent* curExtent = dl.ext();
                 curExtent != NULL;
                 curExtent = curExtent->getNextExtent()) {

                AnalyzeParams extentParams(globalParams);
                extentParams.numberOfSlices = 0; // use the specified or calculated granularity;
                                                 // globalParams.numberOfSlices refers to the
                                                 // total number of slices across all the
                                                 // extents
                BSONObjBuilder extentBuilder(extentsArrayBuilder.subobjStart());
                success = analyzeExtent(nsd, curExtent, subCommand, extentParams, errmsg,
                                        extentBuilder);
                extentBuilder.doneFast();
            }
            extentsArrayBuilder.doneFast();
        }
        if (!success) return false;
        result.appendElements(outputBuilder.obj());
        return true;
    }

    static const char* USE_ANALYZE_STR = "use {analyze: 'diskStorage' | 'pagesInRAM'}";

    bool StorageDetailsCmd::run(const string& dbname, BSONObj& cmdObj, int, string& errmsg,
                                BSONObjBuilder& result, bool fromRepl) {

        // { analyze: subcommand }
        BSONElement analyzeElm = cmdObj["analyze"];
        if (analyzeElm.eoo()) {
            errmsg = str::stream() << "no subcommand specified, " << USE_ANALYZE_STR;
            return false;
        }

        const char* subCommandStr = analyzeElm.valuestrsafe();
        SubCommand subCommand;
        if (str::equals(subCommandStr, "diskStorage")) {
            subCommand = SUBCMD_DISK_STORAGE;
        }
        else if (str::equals(subCommandStr, "pagesInRAM")) {
            subCommand = SUBCMD_PAGES_IN_RAM;
        }
        else {
            errmsg = str::stream() << subCommandStr << " is not a valid subcommand, "
                                                    << USE_ANALYZE_STR;
            return false;
        }

        const string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();
        const NamespaceDetails* nsd = nsdetails(ns);
        if (!cmdLine.quiet) {
            MONGO_TLOG(0) << "CMD: storageDetails " << ns << ", analyze " << subCommandStr << endl;
        }
        if (!nsd) {
            errmsg = "ns not found";
            return false;
        }

        const Extent* extent = NULL;

        // { extent: num }
        BSONElement extentElm = cmdObj["extent"];
        if (extentElm.ok()) {
            if (!extentElm.isNumber()) {
                errmsg = "extent number must be a number, e.g. {..., extent: 3, ...}";
                return false;
            }
            int extentNum = extentElm.numberInt();
            extent = getNthExtent(extentNum, nsd);
            if (extent == NULL) {
                errmsg = str::stream() << "extent " << extentNum << " does not exist";
                return false;
            }
        }

        AnalyzeParams params;

        // { range: [from, to] }, extent-relative
        BSONElement rangeElm = cmdObj["range"];
        if (rangeElm.ok()) {
            if (extent == NULL) {
                errmsg = "a range is only allowed when a single extent is requested, "
                         "use {..., extent: _num, range: [_a, _b], ...}";
                return false;
            }
            if (!(rangeElm.type() == mongo::Array
               && rangeElm["0"].isNumber()
               && rangeElm["1"].isNumber()
               && rangeElm["2"].eoo())) {
                errmsg = "range must be an array with exactly two numeric elements";
                return false;
            }
            params.startOfs = rangeElm["0"].numberInt();
            params.endOfs = rangeElm["1"].numberInt();
        }

        // { granularity: bytes }
        BSONElement granularityElm = cmdObj["granularity"];
        if (granularityElm.ok() && !granularityElm.isNumber()) {
            errmsg = "granularity must be a number";
            return false;
        }
        params.granularity = granularityElm.numberInt();

        // { numberOfSlices: num }
        BSONElement numberOfSlicesElm = cmdObj["numberOfSlices"];
        if (numberOfSlicesElm.ok() && !numberOfSlicesElm.isNumber()) {
            errmsg = "numberOfSlices must be a number";
            return false;
        }
        params.numberOfSlices = numberOfSlicesElm.numberInt();

        BSONElement characteristicFieldElm = cmdObj["characteristicField"];
        if (characteristicFieldElm.ok()) {
            params.characteristicField = characteristicFieldElm.valuestrsafe();
        }

        BSONElement processDeletedRecordsElm = cmdObj["processDeletedRecords"];
        if (processDeletedRecordsElm.ok()) {
            params.processDeletedRecords = processDeletedRecordsElm.trueValue();
        }

        params.showRecords = cmdObj["showRecords"].trueValue();

        return runInternal(nsd, extent, subCommand, params, errmsg, result);
    }

}  // namespace

}  // namespace mongo

