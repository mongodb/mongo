// © 2016 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html
/*
*******************************************************************************
*
*   Copyright (C) 2007-2016, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  icuzdump.cpp
*   encoding:   UTF-8
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2007-04-02
*   created by: Yoshito Umaoka
*
*   This tool write out timezone transitions for ICU timezone.  This tool
*   is used as a part of tzdata update process to check if ICU timezone
*   code works as well as the corresponding Olson stock localtime/zdump.
*/

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>

#include "unicode/utypes.h"
#include "unicode/ustring.h"
#include "unicode/timezone.h"
#include "unicode/simpletz.h"
#include "unicode/smpdtfmt.h"
#include "unicode/decimfmt.h"
#include "unicode/gregocal.h"
#include "unicode/ustream.h"
#include "unicode/putil.h"

#include "cmemory.h"
#include "uoptions.h"

using namespace std;
using namespace icu;

class DumpFormatter {
public:
    DumpFormatter() {
        UErrorCode status = U_ZERO_ERROR;
        stz = new SimpleTimeZone(0, "");
        sdf = new SimpleDateFormat((UnicodeString)"yyyy-MM-dd EEE HH:mm:ss", Locale::getEnglish(), status);
        DecimalFormatSymbols *symbols = new DecimalFormatSymbols(Locale::getEnglish(), status);
        decf = new DecimalFormat("00", symbols, status);
    }
    ~DumpFormatter() {
    }

    UnicodeString& format(UDate time, int32_t offset, UBool isDst, UnicodeString& appendTo) {
        stz->setRawOffset(offset);
        sdf->setTimeZone(*stz);
        UnicodeString str = sdf->format(time, appendTo);
        if (offset < 0) {
            appendTo += "-";
            offset = -offset;
        } else {
            appendTo += "+";
        }

        int32_t hour, min, sec;

        offset /= 1000;
        sec = offset % 60;
        offset = (offset - sec) / 60;
        min = offset % 60;
        hour = offset / 60;

        decf->format(hour, appendTo);
        decf->format(min, appendTo);
        decf->format(sec, appendTo);
        appendTo += "[DST=";
        if (isDst) {
            appendTo += "1";
        } else {
            appendTo += "0";
        }
        appendTo += "]";
        return appendTo;
    }
private:
    SimpleTimeZone*     stz;
    SimpleDateFormat*   sdf;
    DecimalFormat*      decf;
};

class ICUZDump {
public:
    ICUZDump() {
        formatter = new DumpFormatter();
        loyear = 1902;
        hiyear = 2050;
        tick = 1000;
        linesep = NULL;
    }

    ~ICUZDump() {
    }

    void setLowYear(int32_t lo) {
        loyear = lo;
    }

    void setHighYear(int32_t hi) {
        hiyear = hi;
    }

    void setTick(int32_t t) {
        tick = t;
    }

    void setTimeZone(TimeZone* tz) {
        timezone = tz;
    }

    void setDumpFormatter(DumpFormatter* fmt) {
        formatter = fmt;
    }

    void setLineSeparator(const char* sep) {
        linesep = sep;
    }

    void dump(ostream& out) {
        UErrorCode status = U_ZERO_ERROR;
        UDate SEARCH_INCREMENT = 12 * 60 * 60 * 1000; // half day
        UDate t, cutlo, cuthi;
        int32_t rawOffset, dstOffset;
        UnicodeString str;

        getCutOverTimes(cutlo, cuthi);
        t = cutlo;
        timezone->getOffset(t, false, rawOffset, dstOffset, status);
        while (t < cuthi) {
            int32_t newRawOffset, newDstOffset;
            UDate newt = t + SEARCH_INCREMENT;

            timezone->getOffset(newt, false, newRawOffset, newDstOffset, status);

            UBool bSameOffset = (rawOffset + dstOffset) == (newRawOffset + newDstOffset);
            UBool bSameDst = ((dstOffset != 0) && (newDstOffset != 0)) || ((dstOffset == 0) && (newDstOffset == 0));

            if (!bSameOffset || !bSameDst) {
                // find the boundary
                UDate lot = t;
                UDate hit = newt;
                while (true) {
                    int32_t diff = (int32_t)(hit - lot);
                    if (diff <= tick) {
                        break;
                    }
                    UDate medt = lot + ((diff / 2) / tick) * tick;
                    int32_t medRawOffset, medDstOffset;
                    timezone->getOffset(medt, false, medRawOffset, medDstOffset, status);

                    bSameOffset = (rawOffset + dstOffset) == (medRawOffset + medDstOffset);
                    bSameDst = ((dstOffset != 0) && (medDstOffset != 0)) || ((dstOffset == 0) && (medDstOffset == 0));

                    if (!bSameOffset || !bSameDst) {
                        hit = medt;
                    } else {
                        lot = medt;
                    }
                }
                // write out the boundary
                str.remove();
                formatter->format(lot, rawOffset + dstOffset, (dstOffset == 0 ? false : true), str);
                out << str << " > ";
                str.remove();
                formatter->format(hit, newRawOffset + newDstOffset, (newDstOffset == 0 ? false : true), str);
                out << str;
                if (linesep != NULL) {
                    out << linesep;
                } else {
                    out << endl;
                }

                rawOffset = newRawOffset;
                dstOffset = newDstOffset;
            }
            t = newt;
        }
    }

private:
    void getCutOverTimes(UDate& lo, UDate& hi) {
        UErrorCode status = U_ZERO_ERROR;
        GregorianCalendar* gcal = new GregorianCalendar(timezone, Locale::getEnglish(), status);
        gcal->clear();
        gcal->set(loyear, 0, 1, 0, 0, 0);
        lo = gcal->getTime(status);
        gcal->set(hiyear, 0, 1, 0, 0, 0);
        hi = gcal->getTime(status);
    }

    TimeZone*   timezone;
    int32_t     loyear;
    int32_t     hiyear;
    int32_t     tick;

    DumpFormatter*  formatter;
    const char*  linesep;
};

class ZoneIterator {
public:
    ZoneIterator(UBool bAll = false) {
        if (bAll) {
            zenum = TimeZone::createEnumeration();
        }
        else {
            zenum = NULL;
            zids = NULL;
            idx = 0;
            numids = 1;
        }
    }

    ZoneIterator(const char** ids, int32_t num) {
        zenum = NULL;
        zids = ids;
        idx = 0;
        numids = num;
    }

    ~ZoneIterator() {
        if (zenum != NULL) {
            delete zenum;
        }
    }

    TimeZone* next() {
        TimeZone* tz = NULL;
        if (zenum != NULL) {
            UErrorCode status = U_ZERO_ERROR;
            const UnicodeString* zid = zenum->snext(status);
            if (zid != NULL) {
                tz = TimeZone::createTimeZone(*zid);
            }
        }
        else {
            if (idx < numids) {
                if (zids != NULL) {
                    tz = TimeZone::createTimeZone((const UnicodeString&)zids[idx]);
                }
                else {
                    tz = TimeZone::createDefault();
                }
                idx++;
            }
        }
        return tz;
    }

private:
    const char** zids;
    StringEnumeration* zenum;
    int32_t idx;
    int32_t numids;
};

enum { 
  kOptHelpH = 0,
  kOptHelpQuestionMark,
  kOptAllZones,
  kOptCutover,
  kOptDestDir,
  kOptLineSep
};

static UOption options[]={
    UOPTION_HELP_H,
    UOPTION_HELP_QUESTION_MARK,
    UOPTION_DEF("allzones", 'a', UOPT_NO_ARG),
    UOPTION_DEF("cutover", 'c', UOPT_REQUIRES_ARG),
    UOPTION_DEF("destdir", 'd', UOPT_REQUIRES_ARG),
    UOPTION_DEF("linesep", 'l', UOPT_REQUIRES_ARG)
};

extern int
main(int argc, char *argv[]) {
    int32_t low = 1902;
    int32_t high = 2038;
    UBool bAll = false;
    const char *dir = NULL;
    const char *linesep = NULL;

    U_MAIN_INIT_ARGS(argc, argv);
    argc = u_parseArgs(argc, argv, UPRV_LENGTHOF(options), options);

    if (argc < 0) {
        cerr << "Illegal command line argument(s)" << endl << endl;
    }

    if (argc < 0 || options[kOptHelpH].doesOccur || options[kOptHelpQuestionMark].doesOccur) {
        cerr
            << "Usage: icuzdump [-options] [zoneid1 zoneid2 ...]" << endl
            << endl
            << "\tDump all offset transitions for the specified zones." << endl
            << endl
            << "Options:" << endl
            << "\t-a       : Dump all available zones." << endl
            << "\t-d <dir> : When specified, write transitions in a file under" << endl
            << "\t           the directory for each zone." << endl
            << "\t-l <sep> : New line code type used in file outputs. CR or LF (default)"
            << "\t           or CRLF." << endl
            << "\t-c [<low_year>,]<high_year>" << endl
            << "\t         : When specified, dump transitions starting <low_year>" << endl
            << "\t           (inclusive) up to <high_year> (exclusive).  The default" << endl
            << "\t           values are 1902(low) and 2038(high)." << endl;
        return argc < 0 ? U_ILLEGAL_ARGUMENT_ERROR : U_ZERO_ERROR;
    }

    bAll = options[kOptAllZones].doesOccur;

    if (options[kOptDestDir].doesOccur) {
        dir = options[kOptDestDir].value;
    }

    if (options[kOptLineSep].doesOccur) {
        if (strcmp(options[kOptLineSep].value, "CR") == 0) {
            linesep = "\r";
        } else if (strcmp(options[kOptLineSep].value, "CRLF") == 0) {
            linesep = "\r\n";
        } else if (strcmp(options[kOptLineSep].value, "LF") == 0) {
            linesep = "\n";
        }
    }

    if (options[kOptCutover].doesOccur) {
        char* comma = (char*)strchr(options[kOptCutover].value, ',');
        if (comma == NULL) {
            high = atoi(options[kOptCutover].value);
        } else {
            *comma = 0;
            low = atoi(options[kOptCutover].value);
            high = atoi(comma + 1);
        }
    }

    ICUZDump dumper;
    dumper.setLowYear(low);
    dumper.setHighYear(high);
    if (dir != NULL && linesep != NULL) {
        // use the specified line separator only for file output
        dumper.setLineSeparator((const char*)linesep);
    }

    ZoneIterator* zit;
    if (bAll) {
        zit = new ZoneIterator(true);
    } else {
        if (argc <= 1) {
            zit = new ZoneIterator();
        } else {
            zit = new ZoneIterator((const char**)&argv[1], argc - 1);
        }
    }

    UnicodeString id;
    if (dir != NULL) {
        // file output
        ostringstream path;
        ios::openmode mode = ios::out;
        if (linesep != NULL) {
            mode |= ios::binary;
        }
        for (;;) {
            TimeZone* tz = zit->next();
            if (tz == NULL) {
                break;
            }
            dumper.setTimeZone(tz);
            tz->getID(id);

            // target file path
            path.str("");
            path << dir << U_FILE_SEP_CHAR;
            id = id.findAndReplace("/", "-");
            path << id;

            ofstream* fout = new ofstream(path.str().c_str(), mode);
            if (fout->fail()) {
                cerr << "Cannot open file " << path.str() << endl;
                delete fout;
                delete tz;
                break;
            }

            dumper.dump(*fout);
            fout->close();
            delete fout;
            delete tz;
        }

    } else {
        // stdout
        UBool bFirst = true;
        for (;;) {
            TimeZone* tz = zit->next();
            if (tz == NULL) {
                break;
            }
            dumper.setTimeZone(tz);
            tz->getID(id);
            if (bFirst) {
                bFirst = false;
            } else {
                cout << endl;
            }
            cout << "ZONE: " << id << endl;
            dumper.dump(cout);
            delete tz;
        }
    }
    delete zit;
}
