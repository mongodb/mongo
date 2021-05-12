//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include "time_zone.hpp"

//
// Bug - when ICU tries to find a file that is equivalent to /etc/localtime it finds /usr/share/zoneinfo/localtime
// that is just a symbolic link to /etc/localtime.
//
// It started in 4.0 and was fixed in version 4.6, also the fix was backported to the 4.4 branch so it should be
// available from 4.4.3... So we test if the workaround is required
//
// It is also relevant only for Linux, BSD and Apple (as I see in ICU code)
//

#if U_ICU_VERSION_MAJOR_NUM == 4 && (U_ICU_VERSION_MINOR_NUM * 100 + U_ICU_VERSION_PATCHLEVEL_NUM) <= 402
# if defined(__linux) || defined(__FreeBSD__) || defined(__APPLE__)
#   define BOOST_LOCALE_WORKAROUND_ICU_BUG
# endif
#endif

#ifdef BOOST_LOCALE_WORKAROUND_ICU_BUG
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <pthread.h>
#include <string.h>
#include <memory>
#endif

#include <boost/locale/hold_ptr.hpp>

namespace boost {
    namespace locale {
        namespace impl_icu {

            #ifndef BOOST_LOCALE_WORKAROUND_ICU_BUG

            // This is normal behavior

            icu::TimeZone *get_time_zone(std::string const &time_zone)
            {

                if(time_zone.empty()) {
                    return icu::TimeZone::createDefault();
                }
                else {
                    return icu::TimeZone::createTimeZone(time_zone.c_str());
                }
            }

            #else

            //
            // This is a workaround for an ICU timezone detection bug.
            // It is \b very ICU specific and should not be used 
            // in general. It is also designed to work only on
            // specific patforms: Linux, BSD and Apple, where this bug may actually
            // occur
            // 
            namespace {

                // Under BSD, Linux and Mac OS X dirent has normal size
                // so no issues with readdir_r

                class directory {
                public:
                    directory(char const *name) : d(0),read_result(0)
                    {
                        d=opendir(name);
                        if(!d)
                            return;
                    }
                    ~directory()
                    {
                        if(d)
                            closedir(d);
                    }
                    bool is_open()
                    {
                        return d;
                    }
                    char const *next()
                    {
                        if(d && readdir_r(d,&de,&read_result)==0 && read_result!=0)
                            return de.d_name;
                        return 0;
                    }
                private:
                    DIR *d;
                    struct dirent de;
                    struct dirent *read_result;
                };
               
                bool files_equal(std::string const &left,std::string const &right)
                {
                    char l[256],r[256];
                    std::ifstream ls(left.c_str());
                    if(!ls)
                        return false;
                    std::ifstream rs(right.c_str());
                    if(!rs)
                        return false;
                    do {
                        ls.read(l,sizeof(l));
                        rs.read(r,sizeof(r));
                        size_t n;
                        if((n=ls.gcount())!=size_t(rs.gcount()))
                            return false;
                        if(memcmp(l,r,n)!=0)
                            return false;
                    }while(!ls.eof() || !rs.eof());
                    if(bool(ls.eof())!=bool(rs.eof()))
                        return false;
                    return true;
                }
                
                std::string find_file_in(std::string const &ref,size_t size,std::string const &dir)
                {
                    directory d(dir.c_str());
                    if(!d.is_open())
                        return std::string();
                
                    char const *name=0;
                    while((name=d.next())!=0) {
                        std::string file_name = name;
                        if( file_name == "." 
                            || file_name ==".." 
                            || file_name=="posixrules" 
                            || file_name=="localtime")
                        {
                            continue;
                        }
                        struct stat st;
                        std::string path = dir+"/"+file_name;
                        if(stat(path.c_str(),&st)==0) {
                            if(S_ISDIR(st.st_mode)) {
                                std::string res = find_file_in(ref,size,path);
                                if(!res.empty())
                                    return file_name + "/" + res;
                            }
                            else { 
                                if(size_t(st.st_size) == size && files_equal(path,ref)) {
                                    return file_name;
                                }
                            }
                        }
                    }
                    return std::string();
                }

                // This actually emulates ICU's search
                // algorithm... just it ignores localtime
                std::string detect_correct_time_zone()
                {
                     
                    char const *tz_dir = "/usr/share/zoneinfo";
                    char const *tz_file = "/etc/localtime";

                    struct stat st;
                    if(::stat(tz_file,&st)!=0)
                        return std::string();
                    size_t size = st.st_size;
                    std::string r = find_file_in(tz_file,size,tz_dir);
                    if(r.empty())
                        return r;
                    if(r.compare(0,6,"posix/")==0 || r.compare(0,6,"right/",6)==0)
                        return r.substr(6);
                    return r;
                }

                
                //
                // Using pthread as:
                // - This bug is relevant for only Linux, BSD, Mac OS X and
                //  pthreads are native threading API
                // - The dependency on boost.thread may be removed when using
                //   more recent ICU versions (so TLS would not be needed)
                //
                // This the dependency on Boost.Thread is eliminated
                //

                pthread_once_t init_tz = PTHREAD_ONCE_INIT;
                std::string default_time_zone_name;

                extern "C" {
                    static void init_tz_proc()
                    {
                        try {
                            default_time_zone_name = detect_correct_time_zone();
                        }
                        catch(...){}
                    }
                }

                std::string get_time_zone_name()
                {
                    pthread_once(&init_tz,init_tz_proc);
                    return default_time_zone_name;
                }


            } // namespace

            icu::TimeZone *get_time_zone(std::string const &time_zone)
            {

                if(!time_zone.empty()) {
                    return icu::TimeZone::createTimeZone(time_zone.c_str());
                }
                hold_ptr<icu::TimeZone> tz(icu::TimeZone::createDefault());
                icu::UnicodeString id;
                tz->getID(id);
                // Check if there is a bug?
                if(id != icu::UnicodeString("localtime"))
                    return tz.release();
                // Now let's deal with the bug and run the fixed
                // search loop as that of ICU
                std::string real_id = get_time_zone_name();
                if(real_id.empty()) {
                    // if we failed fallback to ICU's time zone
                    return tz.release();
                }
                return icu::TimeZone::createTimeZone(real_id.c_str());
            }
            #endif // bug workaround

        }
    }
}

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
