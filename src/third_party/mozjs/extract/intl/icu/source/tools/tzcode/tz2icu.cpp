// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
**********************************************************************
* Copyright (c) 2003-2014, International Business Machines
* Corporation and others.  All Rights Reserved.
**********************************************************************
* Author: Alan Liu
* Created: July 10 2003
* Since: ICU 2.8
**********************************************************************
*/
#include "tzfile.h" // from Olson tzcode archive, copied to this dir

#ifdef WIN32

 #include <windows.h>
 #undef min // windows.h/STL conflict
 #undef max // windows.h/STL conflict
 // "identifier was truncated to 'number' characters" warning
 #pragma warning(disable: 4786)

#else

 #include <unistd.h>
 #include <stdio.h>
 #include <dirent.h>
 #include <string.h>
 #include <sys/stat.h>

#endif

#include <algorithm>
#include <cassert>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tz2icu.h"
#include "unicode/uversion.h"

using namespace std;

bool ICU44PLUS = true;
string TZ_RESOURCE_NAME = ICU_TZ_RESOURCE;

//--------------------------------------------------------------------
// Time utilities
//--------------------------------------------------------------------

const int64_t SECS_PER_YEAR      = 31536000; // 365 days
const int64_t SECS_PER_LEAP_YEAR = 31622400; // 366 days
const int64_t LOWEST_TIME32    = (int64_t)((int32_t)0x80000000);
const int64_t HIGHEST_TIME32    = (int64_t)((int32_t)0x7fffffff);

bool isLeap(int32_t y) {
    return (y%4 == 0) && ((y%100 != 0) || (y%400 == 0)); // Gregorian
}

int64_t secsPerYear(int32_t y) {
    return isLeap(y) ? SECS_PER_LEAP_YEAR : SECS_PER_YEAR;
}

/**
 * Given a calendar year, return the GMT epoch seconds for midnight
 * GMT of January 1 of that year.  yearToSeconds(1970) == 0.
 */
int64_t yearToSeconds(int32_t year) {
    // inefficient but foolproof
    int64_t s = 0;
    int32_t y = 1970;
    while (y < year) {
        s += secsPerYear(y++);
    }
    while (y > year) {
        s -= secsPerYear(--y);
    }
    return s;
}

/**
 * Given 1970 GMT epoch seconds, return the calendar year containing
 * that time.  secondsToYear(0) == 1970.
 */
int32_t secondsToYear(int64_t seconds) {
    // inefficient but foolproof
    int32_t y = 1970;
    int64_t s = 0;
    if (seconds >= 0) {
        for (;;) {
            s += secsPerYear(y++);
            if (s > seconds) break;
        }
        --y;
    } else {
        for (;;) {
            s -= secsPerYear(--y);
            if (s <= seconds) break;
        }
    }
    return y;
}

//--------------------------------------------------------------------
// Types
//--------------------------------------------------------------------

struct FinalZone;
struct FinalRule;
struct SimplifiedZoneType;

// A transition from one ZoneType to another
// Minimal size = 5 bytes (4+1)
struct Transition {
    int64_t time;  // seconds, 1970 epoch
    int32_t  type;  // index into 'ZoneInfo.types' 0..255
    Transition(int64_t _time, int32_t _type) {
        time = _time;
        type = _type;
    }
};

// A behavior mode (what zic calls a 'type') of a time zone.
// Minimal size = 6 bytes (4+1+3bits)
// SEE: SimplifiedZoneType
struct ZoneType {
    int64_t rawoffset; // raw seconds offset from GMT
    int64_t dstoffset; // dst seconds offset from GMT

    // We don't really need any of the following, but they are
    // retained for possible future use.  See SimplifiedZoneType.
    int32_t  abbr;      // index into ZoneInfo.abbrs 0..n-1
    bool isdst;
    bool isstd;
    bool isgmt;
    
    ZoneType(const SimplifiedZoneType&); // used by optimizeTypeList

    ZoneType() : rawoffset(-1), dstoffset(-1), abbr(-1) {}

    // A restricted equality, of just the raw and dst offset
    bool matches(const ZoneType& other) {
        return rawoffset == other.rawoffset &&
            dstoffset == other.dstoffset;
    }
};

// A collection of transitions from one ZoneType to another, together
// with a list of the ZoneTypes.  A ZoneInfo object may have a long
// list of transitions between a smaller list of ZoneTypes.
//
// This object represents the contents of a single zic-created
// zoneinfo file.
struct ZoneInfo {
    vector<Transition> transitions;
    vector<ZoneType>   types;
    vector<string>     abbrs;

    string finalRuleID;
    int32_t finalOffset;
    int32_t finalYear; // -1 if none

    // If this is an alias, then all other fields are meaningless, and
    // this field will point to the "real" zone 0..n-1.
    int32_t aliasTo; // -1 if this is a "real" zone

    // If there are aliases TO this zone, then the following set will
    // contain their index numbers (each index >= 0).
    set<int32_t> aliases;

    ZoneInfo() : finalYear(-1), aliasTo(-1) {}

    void mergeFinalData(const FinalZone& fz);

    void optimizeTypeList();

    // Set this zone to be an alias TO another zone.
    void setAliasTo(int32_t index);

    // Clear the list of aliases OF this zone.
    void clearAliases();

    // Add an alias to the list of aliases OF this zone.
    void addAlias(int32_t index);

    // Is this an alias to another zone?
    bool isAlias() const {
        return aliasTo >= 0;
    }

    // Retrieve alias list
    const set<int32_t>& getAliases() const {
        return aliases;
    }

    void print(ostream& os, const string& id) const;
};

void ZoneInfo::clearAliases() {
    assert(aliasTo < 0);
    aliases.clear();
}

void ZoneInfo::addAlias(int32_t index) {
    assert(aliasTo < 0 && index >= 0 && aliases.find(index) == aliases.end());
    aliases.insert(index);
}

void ZoneInfo::setAliasTo(int32_t index) {
    assert(index >= 0);
    assert(aliases.size() == 0);
    aliasTo = index;
}

typedef map<string, ZoneInfo> ZoneMap;

typedef ZoneMap::const_iterator ZoneMapIter;

//--------------------------------------------------------------------
// ZONEINFO
//--------------------------------------------------------------------

// Global map holding all our ZoneInfo objects, indexed by id.
ZoneMap ZONEINFO;

//--------------------------------------------------------------------
// zoneinfo file parsing
//--------------------------------------------------------------------

// Read zic-coded 32-bit integer from file
int64_t readcoded(ifstream& file, int64_t minv=numeric_limits<int64_t>::min(),
                               int64_t maxv=numeric_limits<int64_t>::max()) {
    unsigned char buf[4]; // must be UNSIGNED
    int64_t val=0;
    file.read((char*)buf, 4);
    for(int32_t i=0,shift=24;i<4;++i,shift-=8) {
        val |= buf[i] << shift;
    }
    if (val < minv || val > maxv) {
        ostringstream os;
        os << "coded value out-of-range: " << val << ", expected ["
           << minv << ", " << maxv << "]";
        throw out_of_range(os.str());
    }
    return val;
}

// Read zic-coded 64-bit integer from file
int64_t readcoded64(ifstream& file, int64_t minv=numeric_limits<int64_t>::min(),
                               int64_t maxv=numeric_limits<int64_t>::max()) {
    unsigned char buf[8]; // must be UNSIGNED
    int64_t val=0;
    file.read((char*)buf, 8);
    for(int32_t i=0,shift=56;i<8;++i,shift-=8) {
        val |= (int64_t)buf[i] << shift;
    }
    if (val < minv || val > maxv) {
        ostringstream os;
        os << "coded value out-of-range: " << val << ", expected ["
           << minv << ", " << maxv << "]";
        throw out_of_range(os.str());
    }
    return val;
}

// Read a boolean value
bool readbool(ifstream& file) {
    char c;
    file.read(&c, 1);
    if (c!=0 && c!=1) {
        ostringstream os;
        os << "boolean value out-of-range: " << (int32_t)c;
        throw out_of_range(os.str());
    }
    return (c!=0);
}

/**
 * Read the zoneinfo file structure (see tzfile.h) into a ZoneInfo
 * @param file an already-open file stream
 */
void readzoneinfo(ifstream& file, ZoneInfo& info, bool is64bitData) {
    int32_t i;

    // Check for TZ_ICU_MAGIC signature at file start.  If we get a
    // signature mismatch, it means we're trying to read a file which
    // isn't a ICU-modified-zic-created zoneinfo file.  Typically this
    // means the user is passing in a "normal" zoneinfo directory, or
    // a zoneinfo directory that is polluted with other files, or that
    // the user passed in the wrong directory.
    char buf[32];
    file.read(buf, 4);
    if (strncmp(buf, TZ_ICU_MAGIC, 4) != 0) {
        throw invalid_argument("TZ_ICU_MAGIC signature missing");
    }
    // skip additional Olson byte version
    file.read(buf, 1);
    // if '\0', we have just one copy of data, if '2' or '3', there is additional
    // 64 bit version at the end.
    if(buf[0]!=0 && buf[0]!='2' && buf[0]!='3') {
      throw invalid_argument("Bad Olson version info");
    }

    // Read reserved bytes.  The first of these will be a version byte.
    file.read(buf, 15);
    if (*(ICUZoneinfoVersion*)&buf != TZ_ICU_VERSION) {
        throw invalid_argument("File version mismatch");
    }

    // Read array sizes
    int64_t isgmtcnt = readcoded(file, 0);
    int64_t isdstcnt = readcoded(file, 0);
    int64_t leapcnt  = readcoded(file, 0);
    int64_t timecnt  = readcoded(file, 0);
    int64_t typecnt  = readcoded(file, 0);
    int64_t charcnt  = readcoded(file, 0);

    // Confirm sizes that we assume to be equal.  These assumptions
    // are drawn from a reading of the zic source (2003a), so they
    // should hold unless the zic source changes.
    if (isgmtcnt != typecnt || isdstcnt != typecnt) {
        throw invalid_argument("count mismatch between tzh_ttisgmtcnt, tzh_ttisdstcnt, tth_typecnt");
    }

    // Used temporarily to store transition times and types.  We need
    // to do this because the times and types are stored in two
    // separate arrays.
    vector<int64_t> transitionTimes(timecnt, -1); // temporary
    vector<int32_t>  transitionTypes(timecnt, -1); // temporary

    // Read transition times
    for (i=0; i<timecnt; ++i) {
        if (is64bitData) {
            transitionTimes[i] = readcoded64(file);
        } else {
            transitionTimes[i] = readcoded(file);
        }
    }

    // Read transition types
    for (i=0; i<timecnt; ++i) {
        unsigned char c;
        file.read((char*) &c, 1);
        int32_t t = (int32_t) c;
        if (t < 0 || t >= typecnt) {
            ostringstream os;
            os << "illegal type: " << t << ", expected [0, " << (typecnt-1) << "]";
            throw out_of_range(os.str());
        }
        transitionTypes[i] = t;
    }

    // Build transitions vector out of corresponding times and types.
    bool insertInitial = false;
    if (is64bitData && !ICU44PLUS) {
        if (timecnt > 0) {
            int32_t minidx = -1;
            for (i=0; i<timecnt; ++i) {
                if (transitionTimes[i] < LOWEST_TIME32) {
                    if (minidx == -1 || transitionTimes[i] > transitionTimes[minidx]) {
                        // Preserve the latest transition before the 32bit minimum time
                        minidx = i;
                    }
                } else if (transitionTimes[i] > HIGHEST_TIME32) {
                    // Skipping the rest of the transition data.  We cannot put such
                    // transitions into zoneinfo.res, because data is limited to singed
                    // 32bit int by the ICU resource bundle.
                    break;
                } else {
                    info.transitions.push_back(Transition(transitionTimes[i], transitionTypes[i]));
                }
            }
    
            if (minidx != -1) {
                // If there are any transitions before the 32bit minimum time,
                // put the type information with the 32bit minimum time
                vector<Transition>::iterator itr = info.transitions.begin();
                info.transitions.insert(itr, Transition(LOWEST_TIME32, transitionTypes[minidx]));
            } else {
                // Otherwise, we need insert the initial type later
                insertInitial = true;
            }
        }
    } else {
        for (i=0; i<timecnt; ++i) {
            info.transitions.push_back(Transition(transitionTimes[i], transitionTypes[i]));
        }
    }

    // Read types (except for the isdst and isgmt flags, which come later (why??))
    for (i=0; i<typecnt; ++i) { 
        ZoneType type;

        type.rawoffset = readcoded(file);
        type.dstoffset = readcoded(file);
        type.isdst = readbool(file);

        unsigned char c;
        file.read((char*) &c, 1);
        type.abbr = (int32_t) c;

        if (type.isdst != (type.dstoffset != 0)) {
            throw invalid_argument("isdst does not reflect dstoffset");
        }

        info.types.push_back(type);
    }

    assert(info.types.size() == (unsigned) typecnt);

    if (insertInitial) {
        assert(timecnt > 0);
        assert(typecnt > 0);

        int32_t initialTypeIdx = -1;

        // Check if the first type is not dst
        if (info.types.at(0).dstoffset != 0) {
            // Initial type's rawoffset is same with the rawoffset after the
            // first transition, but no DST is observed.
            int64_t rawoffset0 = (info.types.at(info.transitions.at(0).type)).rawoffset;    
            // Look for matching type
            for (i=0; i<(int32_t)info.types.size(); ++i) {
                if (info.types.at(i).rawoffset == rawoffset0
                        && info.types.at(i).dstoffset == 0) {
                    initialTypeIdx = i;
                    break;
                }
            }
        } else {
            initialTypeIdx = 0;
        }
        assert(initialTypeIdx >= 0);
        // Add the initial type associated with the lowest int32 time
        vector<Transition>::iterator itr = info.transitions.begin();
        info.transitions.insert(itr, Transition(LOWEST_TIME32, initialTypeIdx));
    }


    // Read the abbreviation string
    if (charcnt) {
        // All abbreviations are concatenated together, with a 0 at
        // the end of each abbr.
        char* str = new char[charcnt + 8];
        file.read(str, charcnt);

        // Split abbreviations apart into individual strings.  Record
        // offset of each abbr in a vector.
        vector<int32_t> abbroffset;
        char *limit=str+charcnt;
        for (char* p=str; p<limit; ++p) {
            char* start = p;
            while (*p != 0) ++p;
            info.abbrs.push_back(string(start, p-start));
            abbroffset.push_back(start-str);
        }

        // Remap all the abbrs.  Old value is offset into concatenated
        // raw abbr strings.  New value is index into vector of
        // strings.  E.g., 0,5,10,14 => 0,1,2,3.

        // Keep track of which abbreviations get used.
        vector<bool> abbrseen(abbroffset.size(), false);

        for (vector<ZoneType>::iterator it=info.types.begin();
             it!=info.types.end();
             ++it) {
            vector<int32_t>::const_iterator x=
                find(abbroffset.begin(), abbroffset.end(), it->abbr);
            if (x==abbroffset.end()) {
                // TODO: Modify code to add a new string to the end of
                // the abbr list when a middle offset is given, e.g.,
                // "abc*def*" where * == '\0', take offset of 1 and
                // make the array "abc", "def", "bc", and translate 1
                // => 2.  NOT CRITICAL since we don't even use the
                // abbr at this time.
#if 0                
                // TODO: Re-enable this warning if we start using
                // the Olson abbr data, or if the above TODO is completed.
                ostringstream os;
                os << "Warning: unusual abbr offset " << it->abbr
                   << ", expected one of";
                for (vector<int32_t>::const_iterator y=abbroffset.begin();
                     y!=abbroffset.end(); ++y) {
                    os << ' ' << *y;
                }
                cerr << os.str() << "; using 0" << endl;
#endif
                it->abbr = 0;
            } else {
                int32_t index = x - abbroffset.begin();
                it->abbr = index;
                abbrseen[index] = true;
            }
        }

        for (int32_t ii=0;ii<(int32_t) abbrseen.size();++ii) {
            if (!abbrseen[ii]) {
                cerr << "Warning: unused abbreviation: " << ii << endl;
            }
        }
    }

    // Read leap second info, if any.
    // *** We discard leap second data. ***
    for (i=0; i<leapcnt; ++i) {
        readcoded(file); // transition time
        readcoded(file); // total correction after above
    }

    // Read isstd flags
    for (i=0; i<typecnt; ++i) info.types[i].isstd = readbool(file);

    // Read isgmt flags
    for (i=0; i<typecnt; ++i) info.types[i].isgmt = readbool(file);
}

//--------------------------------------------------------------------
// Directory and file reading
//--------------------------------------------------------------------

/**
 * Process a single zoneinfo file, adding the data to ZONEINFO
 * @param path the full path to the file, e.g., ".\zoneinfo\America\Los_Angeles"
 * @param id the zone ID, e.g., "America/Los_Angeles"
 */
void handleFile(string path, string id) {
    // Check for duplicate id
    if (ZONEINFO.find(id) != ZONEINFO.end()) {
        ostringstream os;
        os << "duplicate zone ID: " << id;
        throw invalid_argument(os.str());
    }

    ifstream file(path.c_str(), ios::in | ios::binary);
    if (!file) {
        throw invalid_argument("can't open file");
    }

    // eat 32bit data part
    ZoneInfo info;
    readzoneinfo(file, info, false);

    // Check for errors
    if (!file) {
        throw invalid_argument("read error");
    }

    // we only use 64bit part
    ZoneInfo info64;
    readzoneinfo(file, info64, true);

    bool alldone = false;
    int64_t eofPos = (int64_t) file.tellg();

    // '\n' + <envvar string> + '\n' after the 64bit version data
    char ch = file.get();
    if (ch == 0x0a) {
        bool invalidchar = false;
        while (file.get(ch)) {
            if (ch == 0x0a) {
                break;
            }
            if (ch < 0x20) {
                // must be printable ascii
                invalidchar = true;
                break;
            }
        }
        if (!invalidchar) {
            eofPos = (int64_t) file.tellg();
            file.seekg(0, ios::end);
            eofPos = eofPos - (int64_t) file.tellg();
            if (eofPos == 0) {
                alldone = true;
            }
        }
    }
    if (!alldone) {
        ostringstream os;
        os << (-eofPos) << " unprocessed bytes at end";
        throw invalid_argument(os.str());
    }

    ZONEINFO[id] = info64;
}

/**
 * Recursively scan the given directory, calling handleFile() for each
 * file in the tree.  The user should call with the root directory and
 * a prefix of "".  The function will call itself with non-empty
 * prefix values.
 */
#ifdef WIN32

void scandir(string dirname, string prefix="") {
    HANDLE          hList;
    WIN32_FIND_DATA FileData;
    
    // Get the first file
    hList = FindFirstFile((dirname + "\\*").c_str(), &FileData);
    if (hList == INVALID_HANDLE_VALUE) {
        cerr << "Error: Invalid directory: " << dirname << endl;
        exit(1);
    }
    for (;;) {
        string name(FileData.cFileName);
        string path(dirname + "\\" + name);
        if (FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name != "." && name != "..") {
                scandir(path, prefix + name + "/");
            }
        } else {
            try {
                string id = prefix + name;
                handleFile(path, id);
            } catch (const exception& e) {
                cerr << "Error: While processing \"" << path << "\", "
                     << e.what() << endl;
                exit(1);
            }
        }
        
        if (!FindNextFile(hList, &FileData)) {
            if (GetLastError() == ERROR_NO_MORE_FILES) {
                break;
            } // else...?
        }
    }
    FindClose(hList);
}

#else

void scandir(string dir, string prefix="") {
    DIR *dp;
    struct dirent *dir_entry;
    struct stat stat_info;
    char pwd[512];
    vector<string> subdirs;
    vector<string> subfiles;

    if ((dp = opendir(dir.c_str())) == NULL) {
        cerr << "Error: Invalid directory: " << dir << endl;
        exit(1);
    }
    if (!getcwd(pwd, sizeof(pwd))) {
        cerr << "Error: Directory name too long" << endl;
        exit(1);
    }
    chdir(dir.c_str());
    while ((dir_entry = readdir(dp)) != NULL) {
        string name = dir_entry->d_name;
        string path = dir + "/" + name;
        lstat(dir_entry->d_name,&stat_info);
        if (S_ISDIR(stat_info.st_mode)) {
            if (name != "." && name != "..") {
                subdirs.push_back(path);
                subdirs.push_back(prefix + name + "/");
                // scandir(path, prefix + name + "/");
            }
        } else {
            try {
                string id = prefix + name;
                subfiles.push_back(path);
                subfiles.push_back(id);
                // handleFile(path, id);
            } catch (const exception& e) {
                cerr << "Error: While processing \"" << path << "\", "
                     << e.what() << endl;
                exit(1);
            }
        }
    }
    closedir(dp);
    chdir(pwd);

    for(int32_t i=0;i<(int32_t)subfiles.size();i+=2) {
        try {
            handleFile(subfiles[i], subfiles[i+1]);
        } catch (const exception& e) {
            cerr << "Error: While processing \"" << subfiles[i] << "\", "
                 << e.what() << endl;
            exit(1);
        }
    }
    for(int32_t i=0;i<(int32_t)subdirs.size();i+=2) {
        scandir(subdirs[i], subdirs[i+1]);
    }
}

#endif

//--------------------------------------------------------------------
// Final zone and rule info
//--------------------------------------------------------------------

/**
 * Read and discard the current line.
 */
void consumeLine(istream& in) {
    int32_t c;
    do {
        c = in.get();
    } while (c != EOF && c != '\n');
}

enum {
    DOM = 0,
    DOWGEQ = 1,
    DOWLEQ = 2
};

const char* TIME_MODE[] = {"w", "s", "u"};

// Allow 29 days in February because zic outputs February 29
// for rules like "last Sunday in February".
const int32_t MONTH_LEN[] = {31,29,31,30,31,30,31,31,30,31,30,31};

const int32_t HOUR = 3600;

struct FinalZone {
    int32_t offset; // raw offset
    int32_t year; // takes effect for y >= year
    string ruleid;
    set<string> aliases;
    FinalZone(int32_t _offset, int32_t _year, const string& _ruleid) :
        offset(_offset), year(_year), ruleid(_ruleid)  {
        if (offset <= -16*HOUR || offset >= 16*HOUR) {
            ostringstream os;
            os << "Invalid input offset " << offset
               << " for year " << year
               << " and rule ID " << ruleid;
            throw invalid_argument(os.str());
        }
        if (year < 1900) {
            ostringstream os;
            os << "Invalid input year " << year
               << " with offset " << offset
               << " and rule ID " << ruleid;
            throw invalid_argument(os.str());
        }
    }
    FinalZone() : offset(-1), year(-1) {}
    void addLink(const string& alias) {
        if (aliases.find(alias) != aliases.end()) {
            ostringstream os;
            os << "Duplicate alias " << alias;
            throw invalid_argument(os.str());
        }
        aliases.insert(alias);
    }
};

struct FinalRulePart {
    int32_t mode;
    int32_t month;
    int32_t dom;
    int32_t dow;
    int32_t time;
    int32_t offset; // dst offset, usually either 0 or 1:00

    // Isstd and isgmt only have 3 valid states, corresponding to local
    // wall time, local standard time, and GMT standard time.
    // Here is how the isstd & isgmt flags are set by zic:
    //| case 's':       /* Standard */
    //|         rp->r_todisstd = TRUE;
    //|         rp->r_todisgmt = FALSE;
    //| case 'w':       /* Wall */
    //|         rp->r_todisstd = FALSE;
    //|         rp->r_todisgmt = FALSE;
    //| case 'g':       /* Greenwich */
    //| case 'u':       /* Universal */
    //| case 'z':       /* Zulu */
    //|         rp->r_todisstd = TRUE;
    //|         rp->r_todisgmt = TRUE;
    bool isstd;
    bool isgmt;

    bool isset; // used during building; later ignored

    FinalRulePart() : isset(false) {}
    void set(const string& id,
             const string& _mode,
             int32_t _month,
             int32_t _dom,
             int32_t _dow,
             int32_t _time,
             bool _isstd,
             bool _isgmt,
             int32_t _offset) {
        if (isset) {
            throw invalid_argument("FinalRulePart set twice");
        }
        isset = true;
        if (_mode == "DOWLEQ") {
            mode = DOWLEQ;
        } else if (_mode == "DOWGEQ") {
            mode = DOWGEQ;
        } else if (_mode == "DOM") {
            mode = DOM;
        } else {
            throw invalid_argument("Unrecognized FinalRulePart mode");
        }
        month = _month;
        dom = _dom;
        dow = _dow;
        time = _time;
        isstd = _isstd;
        isgmt = _isgmt;
        offset = _offset;

        ostringstream os;
        if (month < 0 || month >= 12) {
            os << "Invalid input month " << month;
        }
        if (dom < 1 || dom > MONTH_LEN[month]) {
            os << "Invalid input day of month " << dom;
        }
        if (mode != DOM && (dow < 0 || dow >= 7)) {
            os << "Invalid input day of week " << dow;
        }
        if (offset < (-1 * HOUR) || offset > (2 * HOUR)) {
            os << "Invalid input offset " << offset;
        }
        if (isgmt && !isstd) {
            os << "Invalid input isgmt && !isstd";
        }
        if (!os.str().empty()) {
            os << " for rule "
               << id
               << _mode
               << month << dom << dow << time
               << isstd << isgmt
               << offset;
            throw invalid_argument(os.str());
        }
    }

    /**
     * Return the time mode as an ICU SimpleTimeZone int from 0..2;
     * see simpletz.h.
     */
    int32_t timemode() const {
        if (isgmt) {
            assert(isstd);
            return 2; // gmt standard
        }
        if (isstd) {
            return 1; // local standard
        }
        return 0; // local wall
    }

    // The SimpleTimeZone encoding method for rules is as follows:
    //          stz_dowim  stz_dow
    // DOM:     dom        0
    // DOWGEQ:  dom        -(dow+1)
    // DOWLEQ:  -dom       -(dow+1)
    // E.g., to encode Mon>=7, use stz_dowim=7, stz_dow=-2
    //       to encode Mon<=7, use stz_dowim=-7, stz_dow=-2
    //       to encode 7, use stz_dowim=7, stz_dow=0
    // Note that for this program and for SimpleTimeZone, 0==Jan,
    // but for this program 0==Sun while for SimpleTimeZone 1==Sun.

    /**
     * Return a "dowim" param suitable for SimpleTimeZone.
     */
    int32_t stz_dowim() const {
        return (mode == DOWLEQ) ? -dom : dom;
    }

    /**
     * Return a "dow" param suitable for SimpleTimeZone.
     */
    int32_t stz_dow() const {
        return (mode == DOM) ? 0 : -(dow+1);
    }
};

struct FinalRule {
    FinalRulePart part[2];

    bool isset() const {
        return part[0].isset && part[1].isset;
    }

    void print(ostream& os) const;
};

map<string,FinalZone> finalZones;
map<string,FinalRule> finalRules;

map<string, set<string> > links;
map<string, string> reverseLinks;

/**
 * Predicate used to find FinalRule objects that do not have both
 * sub-parts set (indicating an error in the input file).
 */
bool isNotSet(const pair<const string,FinalRule>& p) {
    return !p.second.isset();
}

/**
 * Predicate used to find FinalZone objects that do not map to a known
 * rule (indicating an error in the input file).
 */
bool mapsToUnknownRule(const pair<const string,FinalZone>& p) {
    return finalRules.find(p.second.ruleid) == finalRules.end();
}

/**
 * This set is used to make sure each rule in finalRules is used at
 * least once.  First we populate it with all the rules from
 * finalRules; then we remove all the rules referred to in
 * finaleZones.
 */
set<string> ruleIDset;

void insertRuleID(const pair<string,FinalRule>& p) {
    ruleIDset.insert(p.first);
}

void eraseRuleID(const pair<string,FinalZone>& p) {
    ruleIDset.erase(p.second.ruleid);
}

/**
 * Populate finalZones and finalRules from the given istream.
 */
void readFinalZonesAndRules(istream& in) {

    for (;;) {
        string token;
        in >> token;
        if (in.eof() || !in) {
            break;
        } else if (token == "zone") {
            // zone Africa/Cairo 7200 1995 Egypt # zone Africa/Cairo, offset 7200, year >= 1995, rule Egypt (0)
            string id, ruleid;
            int32_t offset, year;
            in >> id >> offset >> year >> ruleid;
            consumeLine(in);
            finalZones[id] = FinalZone(offset, year, ruleid);
        } else if (token == "rule") {
            // rule US DOWGEQ 3 1 0 7200 0 0 3600 # 52: US, file data/northamerica, line 119, mode DOWGEQ, April, dom 1, Sunday, time 7200, isstd 0, isgmt 0, offset 3600
            // rule US DOWLEQ 9 31 0 7200 0 0 0 # 53: US, file data/northamerica, line 114, mode DOWLEQ, October, dom 31, Sunday, time 7200, isstd 0, isgmt 0, offset 0
            string id, mode;
            int32_t month, dom, dow, time, offset;
            bool isstd, isgmt;
            in >> id >> mode >> month >> dom >> dow >> time >> isstd >> isgmt >> offset;
            consumeLine(in);
            FinalRule& fr = finalRules[id];
            int32_t p = fr.part[0].isset ? 1 : 0;
            fr.part[p].set(id, mode, month, dom, dow, time, isstd, isgmt, offset);
        } else if (token == "link") {
            string fromid, toid; // fromid == "real" zone, toid == alias
            in >> fromid >> toid;
            // DO NOT consumeLine(in);
            if (finalZones.find(toid) != finalZones.end()) {
                throw invalid_argument("Bad link: `to' id is a \"real\" zone");
            }

            links[fromid].insert(toid);
            reverseLinks[toid] = fromid;
        } else if (token.length() > 0 && token[0] == '#') {
            consumeLine(in);
        } else {
            throw invalid_argument("Unrecognized keyword");
        }
    }

    if (!in.eof() && !in) {
        throw invalid_argument("Parse failure");
    }

    // Perform validity check: Each rule should have data for 2 parts.
    if (count_if(finalRules.begin(), finalRules.end(), isNotSet) != 0) {
        throw invalid_argument("One or more incomplete rule pairs");
    }

    // Perform validity check: Each zone should map to a known rule.
    if (count_if(finalZones.begin(), finalZones.end(), mapsToUnknownRule) != 0) {
        throw invalid_argument("One or more zones refers to an unknown rule");
    }

    // Perform validity check: Each rule should be referred to by a zone.
    ruleIDset.clear();
    for_each(finalRules.begin(), finalRules.end(), insertRuleID);
    for_each(finalZones.begin(), finalZones.end(), eraseRuleID);
    if (ruleIDset.size() != 0) {
        throw invalid_argument("Unused rules");
    }
}

//--------------------------------------------------------------------
// Resource bundle output
//--------------------------------------------------------------------

// SEE olsontz.h FOR RESOURCE BUNDLE DATA LAYOUT

void ZoneInfo::print(ostream& os, const string& id) const {
    // Implement compressed format #2:
  os << "  /* " << id << " */ ";

    if (aliasTo >= 0) {
        assert(aliases.size() == 0);
        os << ":int { " << aliasTo << " } "; // No endl - save room for comment.
        return;
    }

    if (ICU44PLUS) {
        os << ":table {" << endl;
    } else {
        os << ":array {" << endl;
    }

    vector<Transition>::const_iterator trn;
    vector<ZoneType>::const_iterator typ;

    bool first;

    if (ICU44PLUS) {
        trn = transitions.begin();

        // pre 32bit transitions
        if (trn != transitions.end() && trn->time < LOWEST_TIME32) {
            os << "    transPre32:intvector { ";
            for (first = true; trn != transitions.end() && trn->time < LOWEST_TIME32; ++trn) {
                if (!first) {
                    os<< ", ";
                }
                first = false;
                os << (int32_t)(trn->time >> 32) << ", " << (int32_t)(trn->time & 0x00000000ffffffff);
            }
            os << " }" << endl;
        }

        // 32bit transtions
        if (trn != transitions.end() && trn->time < HIGHEST_TIME32) {
            os << "    trans:intvector { ";
            for (first = true; trn != transitions.end() && trn->time < HIGHEST_TIME32; ++trn) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                os << trn->time;
            }
            os << " }" << endl;
        }

        // post 32bit transitons
        if (trn != transitions.end()) {
            os << "    transPost32:intvector { ";
            for (first = true; trn != transitions.end(); ++trn) {
                if (!first) {
                    os<< ", ";
                }
                first = false;
                os << (int32_t)(trn->time >> 32) << ", " << (int32_t)(trn->time & 0x00000000ffffffff);
            }
            os << " }" << endl;
        }
    } else {
        os << "    :intvector { ";
        for (trn = transitions.begin(), first = true; trn != transitions.end(); ++trn) {
            if (!first) os << ", ";
            first = false;
            os << trn->time;
        }
        os << " }" << endl;
    }


    first=true;
    if (ICU44PLUS) {
        os << "    typeOffsets:intvector { ";
    } else {
        os << "    :intvector { ";
    }
    for (typ = types.begin(); typ != types.end(); ++typ) {
        if (!first) os << ", ";
        first = false;
        os << typ->rawoffset << ", " << typ->dstoffset;
    }
    os << " }" << endl;

    if (ICU44PLUS) {
        if (transitions.size() != 0) {
            os << "    typeMap:bin { \"" << hex << setfill('0');
            for (trn = transitions.begin(); trn != transitions.end(); ++trn) {
                os << setw(2) << trn->type;
            }
            os << dec << "\" }" << endl;
        }
    } else {
        os << "    :bin { \"" << hex << setfill('0');
        for (trn = transitions.begin(); trn != transitions.end(); ++trn) {
            os << setw(2) << trn->type;
        }
        os << dec << "\" }" << endl;
    }

    // Final zone info, if any
    if (finalYear != -1) {
        if (ICU44PLUS) {
            os << "    finalRule { \"" << finalRuleID << "\" }" << endl;
            os << "    finalRaw:int { " << finalOffset << " }" << endl;
            os << "    finalYear:int { " << finalYear << " }" << endl;
        } else {
            os << "    \"" << finalRuleID << "\"" << endl;
            os << "    :intvector { " << finalOffset << ", "
               << finalYear << " }" << endl;
        }
    }

    // Alias list, if any
    if (aliases.size() != 0) {
        first = true;
        if (ICU44PLUS) {
            os << "    links:intvector { ";
        } else {
            os << "    :intvector { ";
        }
        for (set<int32_t>::const_iterator i=aliases.begin(); i!=aliases.end(); ++i) {
            if (!first) os << ", ";
            first = false;
            os << *i;
        }
        os << " }" << endl;
    }

    os << "  } "; // no trailing 'endl', so comments can be placed.
}

inline ostream&
operator<<(ostream& os, const ZoneMap& zoneinfo) {
    int32_t c = 0;
    for (ZoneMapIter it = zoneinfo.begin();
         it != zoneinfo.end();
         ++it) {
        if(c && !ICU44PLUS)  os << ",";
        it->second.print(os, it->first);
        os << "//Z#" << c++ << endl;
    }
    return os;
}

// print the string list 
ostream& printStringList( ostream& os, const ZoneMap& zoneinfo) {
  int32_t n = 0; // count
  int32_t col = 0; // column
  os << " Names {" << endl
     << "    ";
  for (ZoneMapIter it = zoneinfo.begin();
       it != zoneinfo.end();
       ++it) {
    if(n) {
      os << ",";
      col ++;
    }
    const string& id = it->first;
    os << "\"" << id << "\"";
    col += id.length() + 2;
    if(col >= 50) {
      os << " // " << n << endl
         << "    ";
      col = 0;
    }
    n++;
  }
  os << " // " << (n-1) << endl
     << " }" << endl;

  return os;
}

//--------------------------------------------------------------------
// main
//--------------------------------------------------------------------

// Unary predicate for finding transitions after a given time
bool isAfter(const Transition t, int64_t thresh) {
    return t.time >= thresh;
}

/**
 * A zone type that contains only the raw and dst offset.  Used by the
 * optimizeTypeList() method.
 */
struct SimplifiedZoneType {
    int64_t rawoffset;
    int64_t dstoffset;
    SimplifiedZoneType() : rawoffset(-1), dstoffset(-1) {}
    SimplifiedZoneType(const ZoneType& t) : rawoffset(t.rawoffset),
                                            dstoffset(t.dstoffset) {}
    bool operator<(const SimplifiedZoneType& t) const {
        return rawoffset < t.rawoffset ||
            (rawoffset == t.rawoffset &&
             dstoffset < t.dstoffset);
    }
};

/**
 * Construct a ZoneType from a SimplifiedZoneType.  Note that this
 * discards information; the new ZoneType will have meaningless
 * (empty) abbr, isdst, isstd, and isgmt flags; this is appropriate,
 * since ignoring these is how we do optimization (we have no use for
 * these in historical transitions).
 */
ZoneType::ZoneType(const SimplifiedZoneType& t) :
    rawoffset(t.rawoffset), dstoffset(t.dstoffset),
    abbr(-1), isdst(false), isstd(false), isgmt(false) {}

/**
 * Optimize the type list to remove excess entries.  The type list may
 * contain entries that are distinct only in terms of their dst, std,
 * or gmt flags.  Since we don't care about those flags, we can reduce
 * the type list to a set of unique raw/dst offset pairs, and remap
 * the type indices in the transition list, which stores, for each
 * transition, a transition time and a type index.
 */
void ZoneInfo::optimizeTypeList() {
    // Assemble set of unique types; only those in the `transitions'
    // list, since there may be unused types in the `types' list
    // corresponding to transitions that have been trimmed (during
    // merging of final data).

    if (aliasTo >= 0) return; // Nothing to do for aliases

    if (!ICU44PLUS) {
        // This is the old logic which has a bug, which occasionally removes
        // the type before the first transition.  The problem was fixed
        // by inserting the dummy transition indirectly.

        // If there are zero transitions and one type, then leave that as-is.
        if (transitions.size() == 0) {
            if (types.size() != 1) {
                cerr << "Error: transition count = 0, type count = " << types.size() << endl;
            }
            return;
        }

        set<SimplifiedZoneType> simpleset;
        for (vector<Transition>::const_iterator i=transitions.begin();
             i!=transitions.end(); ++i) {
            assert(i->type < (int32_t)types.size());
            simpleset.insert(types[i->type]);
        }

        // Map types to integer indices
        map<SimplifiedZoneType,int32_t> simplemap;
        int32_t n=0;
        for (set<SimplifiedZoneType>::const_iterator i=simpleset.begin();
             i!=simpleset.end(); ++i) {
            simplemap[*i] = n++;
        }

        // Remap transitions
        for (vector<Transition>::iterator i=transitions.begin();
             i!=transitions.end(); ++i) {
            assert(i->type < (int32_t)types.size());
            ZoneType oldtype = types[i->type];
            SimplifiedZoneType newtype(oldtype);
            assert(simplemap.find(newtype) != simplemap.end());
            i->type = simplemap[newtype];
        }

        // Replace type list
        types.clear();
        copy(simpleset.begin(), simpleset.end(), back_inserter(types));

    } else {
        if (types.size() > 1) {
            // Note: localtime uses the very first non-dst type as initial offsets.
            // If all types are DSTs, the very first type is treated as the initial offsets.

            // Decide a type used as the initial offsets.  ICU put the type at index 0.
            ZoneType initialType = types[0];
            for (vector<ZoneType>::const_iterator i=types.begin(); i!=types.end(); ++i) {
                if (i->dstoffset == 0) {
                    initialType = *i;
                    break;
                }
            }

            SimplifiedZoneType initialSimplifiedType(initialType);

            // create a set of unique types, but ignoring fields which we're not interested in
            set<SimplifiedZoneType> simpleset;
            simpleset.insert(initialSimplifiedType);
            for (vector<Transition>::const_iterator i=transitions.begin(); i!=transitions.end(); ++i) {
                assert(i->type < (int32_t)types.size());
                simpleset.insert(types[i->type]);
            }

            // Map types to integer indices, however, keeping the first type at offset 0
            map<SimplifiedZoneType,int32_t> simplemap;
            simplemap[initialSimplifiedType] = 0;
            int32_t n = 1;
            for (set<SimplifiedZoneType>::const_iterator i=simpleset.begin(); i!=simpleset.end(); ++i) {
                if (*i < initialSimplifiedType || initialSimplifiedType < *i) {
                    simplemap[*i] = n++;
                }
            }

            // Remap transitions
            for (vector<Transition>::iterator i=transitions.begin();
                 i!=transitions.end(); ++i) {
                assert(i->type < (int32_t)types.size());
                ZoneType oldtype = types[i->type];
                SimplifiedZoneType newtype(oldtype);
                assert(simplemap.find(newtype) != simplemap.end());
                i->type = simplemap[newtype];
            }

            // Replace type list
            types.clear();
            types.push_back(initialSimplifiedType);
            for (set<SimplifiedZoneType>::const_iterator i=simpleset.begin(); i!=simpleset.end(); ++i) {
                if (*i < initialSimplifiedType || initialSimplifiedType < *i) {
                    types.push_back(*i);
                }
            }

            // Reiterating transitions to remove any transitions which
            // do not actually change the raw/dst offsets
            int32_t prevTypeIdx = 0;
            for (vector<Transition>::iterator i=transitions.begin(); i!=transitions.end();) {
                if (i->type == prevTypeIdx) {
                    // this is not a time transition, probably just name change
                    // e.g. America/Resolute after 2006 in 2010b
                    transitions.erase(i);
                } else {
                    prevTypeIdx = i->type;
                    i++;
                }
            }
        }
    }

}

/**
 * Merge final zone data into this zone.
 */
void ZoneInfo::mergeFinalData(const FinalZone& fz) {
    int32_t year = fz.year;
    int64_t seconds = yearToSeconds(year);

    if (!ICU44PLUS) {
        if (seconds > HIGHEST_TIME32) {
            // Avoid transitions beyond signed 32bit max second.
            // This may result incorrect offset computation around
            // HIGHEST_TIME32.  This is a limitation of ICU
            // before 4.4.
            seconds = HIGHEST_TIME32;
        }
    }

    vector<Transition>::iterator it =
        find_if(transitions.begin(), transitions.end(),
                bind2nd(ptr_fun(isAfter), seconds));
    transitions.erase(it, transitions.end());

    if (finalYear != -1) {
        throw invalid_argument("Final zone already merged in");
    }
    finalYear = fz.year;
    finalOffset = fz.offset;
    finalRuleID = fz.ruleid;
}

/**
 * Merge the data from the given final zone into the core zone data by
 * calling the ZoneInfo member function mergeFinalData.
 */
void mergeOne(const string& zoneid, const FinalZone& fz) {
    if (ZONEINFO.find(zoneid) == ZONEINFO.end()) {
        throw invalid_argument("Unrecognized final zone ID");
    }
    ZONEINFO[zoneid].mergeFinalData(fz);
}

/**
 * Visitor function that merges the final zone data into the main zone
 * data structures.  It calls mergeOne for each final zone and its
 * list of aliases.
 */
void mergeFinalZone(const pair<string,FinalZone>& p) {
    const string& id = p.first;
    const FinalZone& fz = p.second;

    mergeOne(id, fz);
}

/**
 * Print this rule in resource bundle format to os.  ID and enclosing
 * braces handled elsewhere.
 */
void FinalRule::print(ostream& os) const {
    // First print the rule part that enters DST; then the rule part
    // that exits it.
    int32_t whichpart = (part[0].offset != 0) ? 0 : 1;
    assert(part[whichpart].offset != 0);
    assert(part[1-whichpart].offset == 0);

    os << "    ";
    for (int32_t i=0; i<2; ++i) {
        const FinalRulePart& p = part[whichpart];
        whichpart = 1-whichpart;
        os << p.month << ", " << p.stz_dowim() << ", " << p.stz_dow() << ", "
           << p.time << ", " << p.timemode() << ", ";
    }
    os << part[whichpart].offset << endl;
}

#define ICU_ZONE_OVERRIDE_SUFFIX "--ICU"
#define ICU_ZONE_OVERRIDE_SUFFIX_LEN 5

int main(int argc, char *argv[]) {
    string rootpath, zonetab, version;
    bool validArgs = false;

    if (argc == 4 || argc == 5) {
        validArgs = true;
        rootpath = argv[1];
        zonetab = argv[2];
        version = argv[3];
        if (argc == 5) {
            if (strcmp(argv[4], "--old") == 0) {
                ICU44PLUS = false;
                TZ_RESOURCE_NAME = ICU_TZ_RESOURCE_OLD;
            } else {
                validArgs = false;
            }
        }
    }
    if (!validArgs) {
        cout << "Usage: tz2icu <dir> <cmap> <tzver> [--old]" << endl
             << " <dir>    path to zoneinfo file tree generated by" << endl
             << "          ICU-patched version of zic" << endl
             << " <cmap>   country map, from tzdata archive," << endl
             << "          typically named \"zone.tab\"" << endl
             << " <tzver>  version string, such as \"2003e\"" << endl
             << " --old    generating resource format before ICU4.4" << endl;
        exit(1);
    }

    cout << "Olson data version: " << version << endl;
    cout << "ICU 4.4+ format: " << (ICU44PLUS ? "Yes" : "No") << endl;

    try {
        ifstream finals(ICU_ZONE_FILE);
        if (finals) {
            readFinalZonesAndRules(finals);

            cout << "Finished reading " << finalZones.size()
                 << " final zones and " << finalRules.size()
                 << " final rules from " ICU_ZONE_FILE << endl;
        } else {
            cerr << "Error: Unable to open " ICU_ZONE_FILE << endl;
            return 1;
        }
    } catch (const exception& error) {
        cerr << "Error: While reading " ICU_ZONE_FILE ": " << error.what() << endl;
        return 1;
    }

    try {
        // Recursively scan all files below the given path, accumulating
        // their data into ZONEINFO.  All files must be TZif files.  Any
        // failure along the way will result in a call to exit(1).
        scandir(rootpath);
    } catch (const exception& error) {
        cerr << "Error: While scanning " << rootpath << ": " << error.what() << endl;
        return 1;
    }

    cout << "Finished reading " << ZONEINFO.size() << " zoneinfo files ["
         << (ZONEINFO.begin())->first << ".."
         << (--ZONEINFO.end())->first << "]" << endl;

    // Overrides TZ database zones with ICU custom zone definition.
    // These ICU zone overrides are defined in icuzones, with suffix --ICU.
    // If there is a matching TZ database zone, the zoneinfo is replaced
    // with the ICU definition. Then, the zone ID with --ICU suffix
    // will be deleted from the final list.
    // For example, zoneinfo for Europe/Dublin imported from the TZ database
    // will be replaced with the zone definition for Europe/Dublin--ICU
    // in icuzones.

    // Collect zone IDs to be modified with ICU definition.
    vector<string> customZones;
    for (ZoneMapIter i = ZONEINFO.begin(); i != ZONEINFO.end(); ++i) {
        const string& id = i->first;
        size_t idx = id.rfind(ICU_ZONE_OVERRIDE_SUFFIX);
        if (idx != string::npos && idx == id.length() - ICU_ZONE_OVERRIDE_SUFFIX_LEN) {
            cout << "ICU zone override: " << id << endl;
            customZones.push_back(id.substr(0, idx));
        }
    }

    //
    // BEGIN ICU Custom ZoneInfo Override Handling
    //

    // Replace zoneinfo with ICU definition, then remove ICU zone ID with
    // the special suffix.
    for (vector<string>::iterator i = customZones.begin(); i != customZones.end(); i++) {
        string& origId = *i;
        string custId = origId + ICU_ZONE_OVERRIDE_SUFFIX;

        map<string,ZoneInfo>::iterator origZi = ZONEINFO.find(origId);
        map<string,ZoneInfo>::iterator custZi = ZONEINFO.find(custId);
        if (origZi != ZONEINFO.end() && custZi != ZONEINFO.end()) {
            // replace original zone info with custom override,
            // then delete one custom ID
            cout << "Replacing ZoneInfo " << origId << " with " << custId << endl;
            origZi->second = custZi->second;
            ZONEINFO.erase(custZi);
        }

        // Also replace final rule
        map<string,FinalZone>::iterator origFz = finalZones.find(origId);
        map<string,FinalZone>::iterator custFz = finalZones.find(custId);
        if (origFz != finalZones.end() && custFz != finalZones.end()) {
            // replace original final zone with custom override,
            // then delete one for custom ID
            cout << "Replacing FinalZone for " << origId << " with " << custId << endl;
            origFz->second = custFz->second;
            finalZones.erase(custFz);
        }
    }

    // Also remove aliases for ICU custom zoneinfo overrides.
    for (map<string,set<string>>::const_iterator i = links.begin(); i != links.end(); ) {
        const string& id = i->first;
        size_t idx = id.rfind(ICU_ZONE_OVERRIDE_SUFFIX);
        if (idx != string::npos && idx == id.length() - ICU_ZONE_OVERRIDE_SUFFIX_LEN) {
            const set<string>& aliases = i->second;
            // Also remove all revserse links
            for (set<string>::const_iterator j = aliases.begin(); j != aliases.end(); j++) {
                const string& alias = *j;
                cout << "Removing alias " << alias << endl;
                reverseLinks.erase(alias);
            }

            links.erase(i++);
        } else {
            i++;
        }
    }


    //
    // END ICU Custom ZoneInfo Override Handling
    //

    try {
        for_each(finalZones.begin(), finalZones.end(), mergeFinalZone);
    } catch (const exception& error) {
        cerr << "Error: While merging final zone data: " << error.what() << endl;
        return 1;
    }

    // Process links (including ICU aliases).  For each link set we have
    // a canonical ID (e.g., America/Los_Angeles) and a set of one or more
    // aliases (e.g., PST, PST8PDT, ...).
    
    // 1. Add all aliases as zone objects in ZONEINFO
    for (map<string,set<string> >::const_iterator i = links.begin();
         i!=links.end(); ++i) {
        const string& olson = i->first;
        const set<string>& aliases = i->second;
        if (ZONEINFO.find(olson) == ZONEINFO.end()) {
            cerr << "Error: Invalid 'Link' to non-existent \""
                 << olson << "\"" << endl;
            return 1;
        }
        for (set<string>::const_iterator j=aliases.begin();
             j!=aliases.end(); ++j) {
            ZONEINFO[*j] = ZoneInfo();
        }
    }
 
    // 2. Create a mapping from zones to index numbers 0..n-1.
    map<string,int32_t> zoneIDs;
    vector<string> zoneIDlist;
    int32_t z=0;
    for (ZoneMap::iterator i=ZONEINFO.begin(); i!=ZONEINFO.end(); ++i) {
        zoneIDs[i->first] = z++;
        zoneIDlist.push_back(i->first);
    }
    assert(z == (int32_t) ZONEINFO.size());

    // 3. Merge aliases.  Sometimes aliases link to other aliases; we
    // resolve these into simplest possible sets.
    map<string,set<string> > links2;
    map<string,string> reverse2;
    for (map<string,set<string> >::const_iterator i = links.begin();
         i!=links.end(); ++i) {
        string olson = i->first;
        while (reverseLinks.find(olson) != reverseLinks.end()) {
            olson = reverseLinks[olson];
        }
        for (set<string>::const_iterator j=i->second.begin(); j!=i->second.end(); ++j) {
            links2[olson].insert(*j);
            reverse2[*j] = olson;
        }
    }
    links = links2;
    reverseLinks = reverse2;

    if (false) { // Debugging: Emit link map
        for (map<string,set<string> >::const_iterator i = links.begin();
             i!=links.end(); ++i) {
            cout << i->first << ": ";
            for (set<string>::const_iterator j=i->second.begin(); j!=i->second.end(); ++j) {
                cout << *j << ", ";
            }
            cout << endl;
        }
    }

    // 4. Update aliases
    for (map<string,set<string> >::const_iterator i = links.begin();
         i!=links.end(); ++i) {
        const string& olson = i->first;
        const set<string>& aliases = i->second;
        ZONEINFO[olson].clearAliases();
        ZONEINFO[olson].addAlias(zoneIDs[olson]);
        for (set<string>::const_iterator j=aliases.begin();
             j!=aliases.end(); ++j) {
            assert(zoneIDs.find(olson) != zoneIDs.end());
            assert(zoneIDs.find(*j) != zoneIDs.end());
            assert(ZONEINFO.find(*j) != ZONEINFO.end());
            ZONEINFO[*j].setAliasTo(zoneIDs[olson]);
            ZONEINFO[olson].addAlias(zoneIDs[*j]);
        }
    }

    // Once merging of final data is complete, we can optimize the type list
    for (ZoneMap::iterator i=ZONEINFO.begin(); i!=ZONEINFO.end(); ++i) {
        i->second.optimizeTypeList();
    }

    // Create the country map
    map<string, string> icuRegions;        // ICU's custom zone -> country override
    map<string, set<string> > countryMap;  // country -> set of zones
    map<string, string> reverseCountryMap; // zone -> country

    try {
        // Read icuregions file to collect ICU's own zone-region mapping data.
        ifstream frg(ICU_REGIONS);
        if (frg) {
            string line;
            while (getline(frg, line)) {
                if (line[0] == '#') continue;

                string zone, country;
                istringstream is(line);
                is >> zone >> country;
                if (zone.size() == 0) continue;
                if (country.size() < 2) {
                    cerr << "Error: Can't parse " << line << " in " << ICU_REGIONS << endl;
                    return 1;
                }
                icuRegions[zone] = country;
            }
        } else {
            cout << "No custom region map [icuregions]" << endl;
        }
    } catch (const exception& error) {
        cerr << "Error: While reading " << ICU_REGIONS << ": " << error.what() << endl;
        return 1;
    }

    try {
        ifstream f(zonetab.c_str());
        if (!f) {
            cerr << "Error: Unable to open " << zonetab << endl;
            return 1;
        }
        int32_t n = 0;
        string line;
        while (getline(f, line)) {
            string::size_type lb = line.find('#');
            if (lb != string::npos) {
                line.resize(lb); // trim comments
            }
            string country, coord, zone;
            istringstream is(line);
            is >> country >> coord >> zone;
            if (country.size() == 0) continue;
            if (country.size() != 2 || zone.size() < 1) {
                cerr << "Error: Can't parse " << line << " in " << zonetab << endl;
                return 1;
            }
            if (ZONEINFO.find(zone) == ZONEINFO.end()) {
                cerr << "Error: Country maps to invalid zone " << zone
                     << " in " << zonetab << endl;
                return 1;
            }
            if (icuRegions.find(zone) != icuRegions.end()) {
                // Custom override
                string customCountry = icuRegions[zone];
                cout << "Region Mapping: custom override for " << zone
                    << " " << country << " -> " << customCountry << endl;
                country = customCountry;
            }
            countryMap[country].insert(zone);
            reverseCountryMap[zone] = country;
            //cerr << (n+1) << ": " << country << " <=> " << zone << endl;
            ++n;
        }
        cout << "Finished reading " << n
             << " country entries from " << zonetab << endl;
    } catch (const exception& error) {
        cerr << "Error: While reading " << zonetab << ": " << error.what() << endl;
        return 1;
    }

    // Merge ICU's own zone-region mapping data
    for (map<string,string>::const_iterator i = icuRegions.begin();
        i != icuRegions.end(); ++i) {
        const string& zid(i->first);
        if (reverseCountryMap.find(zid) != reverseCountryMap.end()) {
            continue;
        }
        cout << "Region Mapping: custom data zone=" << zid
            << ", region=" << i->second << endl;

        reverseCountryMap[zid] = i->second;
        countryMap[i->second].insert(zid);
    }

    // Merge ICU aliases into country map.  Don't merge any alias
    // that already has a country map, since that doesn't make sense.
    // E.g.  "Link Europe/Oslo Arctic/Longyearbyen" doesn't mean we
    // should cross-map the countries between these two zones.
    for (map<string,set<string> >::const_iterator i = links.begin();
         i!=links.end(); ++i) {
        const string& olson(i->first);
        if (reverseCountryMap.find(olson) == reverseCountryMap.end()) {
            continue;
        }
        string c = reverseCountryMap[olson];
        const set<string>& aliases(i->second);
        for (set<string>::const_iterator j=aliases.begin();
             j != aliases.end(); ++j) {
            if (reverseCountryMap.find(*j) == reverseCountryMap.end()) {
                countryMap[c].insert(*j);
                reverseCountryMap[*j] = c;
                //cerr << "Aliased country: " << c << " <=> " << *j << endl;
            }
        }
    }

    // Create a pseudo-country containing all zones belonging to no country
    set<string> nocountry;
    for (ZoneMap::iterator i=ZONEINFO.begin(); i!=ZONEINFO.end(); ++i) {
        if (reverseCountryMap.find(i->first) == reverseCountryMap.end()) {
            nocountry.insert(i->first);
        }
    }
    countryMap[""] = nocountry;

    // Get local time & year for below
    time_t sec;
    time(&sec);
    struct tm* now = localtime(&sec);

    string filename = TZ_RESOURCE_NAME + ".txt";
    // Write out a resource-bundle source file containing data for
    // all zones.
    ofstream file(filename.c_str());
    if (file) {
        file << "//---------------------------------------------------------" << endl
             << "// Copyright (C) 2016 and later: Unicode, Inc. and others." << endl
             << "// License & terms of use: http://www.unicode.org/copyright.html" << endl
             << "//---------------------------------------------------------" << endl
             << "// Build tool:  tz2icu" << endl
             << "// Build date:  " << asctime(now) /* << endl -- asctime emits CR */
             << "// tz database: ftp://ftp.iana.org/tz/" << endl
             << "// tz version:  " << version << endl
             << "// ICU version: " << U_ICU_VERSION << endl
             << "//---------------------------------------------------------" << endl
             << "// >> !!! >>   THIS IS A MACHINE-GENERATED FILE   << !!! <<" << endl
             << "// >> !!! >>>            DO NOT EDIT             <<< !!! <<" << endl
             << "//---------------------------------------------------------" << endl
             << endl
             << TZ_RESOURCE_NAME << ":table(nofallback) {" << endl
             << " TZVersion { \"" << version << "\" }" << endl
             << " Zones:array { " << endl
             << ZONEINFO // Zones (the actual data)
             << " }" << endl;

        // Names correspond to the Zones list, used for binary searching.
        printStringList ( file, ZONEINFO ); // print the Names list

        // Final Rules are used if requested by the zone
        file << " Rules { " << endl;
        // Emit final rules
        int32_t frc = 0;
        for(map<string,FinalRule>::iterator i=finalRules.begin();
            i!=finalRules.end(); ++i) {
            const string& id = i->first;
            const FinalRule& r = i->second;
            file << "  " << id << ":intvector {" << endl;
            r.print(file);
            file << "  } //_#" << frc++ << endl;
        }
        file << " }" << endl;

        // Emit country (region) map.
        if (ICU44PLUS) {
            file << " Regions:array {" << endl;
            int32_t zn = 0;
            for (ZoneMap::iterator i=ZONEINFO.begin(); i!=ZONEINFO.end(); ++i) {
                map<string, string>::iterator cit = reverseCountryMap.find(i->first);
                if (cit == reverseCountryMap.end()) {
                    file << "  \"001\",";
                } else {
                    file << "  \"" << cit->second << "\", ";
                }
                file << "//Z#" << zn++ << " " << i->first << endl;
            }
            file << " }" << endl;
        } else {
            file << " Regions { " << endl;
            int32_t  rc = 0;
            for (map<string, set<string> >::const_iterator i=countryMap.begin();
                 i != countryMap.end(); ++i) {
                string country = i->first;
                const set<string>& zones(i->second);
                file << "  ";
                if(country[0]==0) {
                  file << "Default";
                }
                file << country << ":intvector { ";
                bool first = true;
                for (set<string>::const_iterator j=zones.begin();
                     j != zones.end(); ++j) {
                    if (!first) file << ", ";
                    first = false;
                    if (zoneIDs.find(*j) == zoneIDs.end()) {
                        cerr << "Error: Nonexistent zone in country map: " << *j << endl;
                        return 1;
                    }
                    file << zoneIDs[*j]; // emit the zone's index number
                }
                file << " } //R#" << rc++ << endl;
            }
            file << " }" << endl;
        }

        file << "}" << endl;
    }

    file.close();
     
    if (file) { // recheck error bit
        cout << "Finished writing " << TZ_RESOURCE_NAME << ".txt" << endl;
    } else {
        cerr << "Error: Unable to open/write to " << TZ_RESOURCE_NAME << ".txt" << endl;
        return 1;
    }
}
//eof
