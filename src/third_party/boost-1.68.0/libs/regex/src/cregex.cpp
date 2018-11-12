/*
 *
 * Copyright (c) 1998-2002
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE:        cregex.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Implements high level class boost::RexEx
  */


#define BOOST_REGEX_SOURCE

#include <boost/regex.hpp>
#include <boost/cregex.hpp>
#if !defined(BOOST_NO_STD_STRING)
#include <map>
#include <list>
#include <boost/regex/v4/fileiter.hpp>
typedef boost::match_flag_type match_flag_type;
#include <cstdio>

#ifdef BOOST_MSVC
#pragma warning(disable:4309)
#endif
#ifdef BOOST_INTEL
#pragma warning(disable:981 383)
#endif

namespace boost{

#ifdef __BORLANDC__
#if __BORLANDC__ < 0x530
//
// we need to instantiate the vector classes we use
// since declaring a reference to type doesn't seem to
// do the job...
std::vector<std::size_t> inst1;
std::vector<std::string> inst2;
#endif
#endif

namespace{

template <class iterator>
std::string to_string(iterator i, iterator j)
{
   std::string s;
   while(i != j)
   {
      s.append(1, *i);
      ++i;
   }
   return s;
}

inline std::string to_string(const char* i, const char* j)
{
   return std::string(i, j);
}

}
namespace BOOST_REGEX_DETAIL_NS{

class RegExData
{
public:
   enum type
   {
      type_pc,
      type_pf,
      type_copy
   };
   regex e;
   cmatch m;
#ifndef BOOST_REGEX_NO_FILEITER
   match_results<mapfile::iterator> fm;
#endif
   type t;
   const char* pbase;
#ifndef BOOST_REGEX_NO_FILEITER
   mapfile::iterator fbase;
#endif
   std::map<int, std::string, std::less<int> > strings;
   std::map<int, std::ptrdiff_t, std::less<int> > positions;
   void update();
   void clean();
   RegExData() : e(), m(),
#ifndef BOOST_REGEX_NO_FILEITER
   fm(),
#endif
   t(type_copy), pbase(0),
#ifndef BOOST_REGEX_NO_FILEITER
   fbase(),
#endif
   strings(), positions() {}
};

void RegExData::update()
{
   strings.erase(strings.begin(), strings.end());
   positions.erase(positions.begin(), positions.end());
   if(t == type_pc)
   {
      for(unsigned int i = 0; i < m.size(); ++i)
      {
         if(m[i].matched) strings[i] = std::string(m[i].first, m[i].second);
         positions[i] = m[i].matched ? m[i].first - pbase : -1;
      }
   }
#ifndef BOOST_REGEX_NO_FILEITER
   else
   {
      for(unsigned int i = 0; i < fm.size(); ++i)
      {
         if(fm[i].matched) strings[i] = to_string(fm[i].first, fm[i].second);
         positions[i] = fm[i].matched ? fm[i].first - fbase : -1;
      }
   }
#endif
   t = type_copy;
}

void RegExData::clean()
{
#ifndef BOOST_REGEX_NO_FILEITER
   fbase = mapfile::iterator();
   fm = match_results<mapfile::iterator>();
#endif
}

} // namespace

RegEx::RegEx()
{
   pdata = new BOOST_REGEX_DETAIL_NS::RegExData();
}

RegEx::RegEx(const RegEx& o)
{
   pdata = new BOOST_REGEX_DETAIL_NS::RegExData(*(o.pdata));
}

RegEx::~RegEx()
{
   delete pdata;
}

RegEx::RegEx(const char* c, bool icase)
{
   pdata = new BOOST_REGEX_DETAIL_NS::RegExData();
   SetExpression(c, icase);
}

RegEx::RegEx(const std::string& s, bool icase)
{
   pdata = new BOOST_REGEX_DETAIL_NS::RegExData();
   SetExpression(s.c_str(), icase);
}

RegEx& RegEx::operator=(const RegEx& o)
{
   *pdata = *(o.pdata);
   return *this;
}

RegEx& RegEx::operator=(const char* p)
{
   SetExpression(p, false);
   return *this;
}

unsigned int RegEx::SetExpression(const char* p, bool icase)
{
   boost::uint_fast32_t f = icase ? regex::normal | regex::icase : regex::normal;
   return pdata->e.set_expression(p, f);
}

unsigned int RegEx::error_code()const
{
   return pdata->e.error_code();
}


std::string RegEx::Expression()const
{
   return pdata->e.expression();
}

//
// now matching operators:
//
bool RegEx::Match(const char* p, match_flag_type flags)
{
   pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pc;
   pdata->pbase = p;
   const char* end = p;
   while(*end)++end;

   if(regex_match(p, end, pdata->m, pdata->e, flags))
   {
      pdata->update();
      return true;
   }
   return false;
}

bool RegEx::Search(const char* p, match_flag_type flags)
{
   pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pc;
   pdata->pbase = p;
   const char* end = p;
   while(*end)++end;

   if(regex_search(p, end, pdata->m, pdata->e, flags))
   {
      pdata->update();
      return true;
   }
   return false;
}
namespace BOOST_REGEX_DETAIL_NS{
struct pred1
{
   GrepCallback cb;
   RegEx* pe;
   pred1(GrepCallback c, RegEx* i) : cb(c), pe(i) {}
   bool operator()(const cmatch& m)
   {
      pe->pdata->m = m;
      return cb(*pe);
   }
};
}
unsigned int RegEx::Grep(GrepCallback cb, const char* p, match_flag_type flags)
{
   pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pc;
   pdata->pbase = p;
   const char* end = p;
   while(*end)++end;

   unsigned int result = regex_grep(BOOST_REGEX_DETAIL_NS::pred1(cb, this), p, end, pdata->e, flags);
   if(result)
      pdata->update();
   return result;
}
namespace BOOST_REGEX_DETAIL_NS{
struct pred2
{
   std::vector<std::string>& v;
   RegEx* pe;
   pred2(std::vector<std::string>& o, RegEx* e) : v(o), pe(e) {}
   bool operator()(const cmatch& m)
   {
      pe->pdata->m = m;
      v.push_back(std::string(m[0].first, m[0].second));
      return true;
   }
private:
   pred2& operator=(const pred2&);
};
}

unsigned int RegEx::Grep(std::vector<std::string>& v, const char* p, match_flag_type flags)
{
   pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pc;
   pdata->pbase = p;
   const char* end = p;
   while(*end)++end;

   unsigned int result = regex_grep(BOOST_REGEX_DETAIL_NS::pred2(v, this), p, end, pdata->e, flags);
   if(result)
      pdata->update();
   return result;
}
namespace BOOST_REGEX_DETAIL_NS{
struct pred3
{
   std::vector<std::size_t>& v;
   const char* base;
   RegEx* pe;
   pred3(std::vector<std::size_t>& o, const char* pb, RegEx* p) : v(o), base(pb), pe(p) {}
   bool operator()(const cmatch& m)
   {
      pe->pdata->m = m;
      v.push_back(static_cast<std::size_t>(m[0].first - base));
      return true;
   }
private:
   pred3& operator=(const pred3&);
};
}
unsigned int RegEx::Grep(std::vector<std::size_t>& v, const char* p, match_flag_type flags)
{
   pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pc;
   pdata->pbase = p;
   const char* end = p;
   while(*end)++end;

   unsigned int result = regex_grep(BOOST_REGEX_DETAIL_NS::pred3(v, p, this), p, end, pdata->e, flags);
   if(result)
      pdata->update();
   return result;
}
#ifndef BOOST_REGEX_NO_FILEITER
namespace BOOST_REGEX_DETAIL_NS{
struct pred4
{
   GrepFileCallback cb;
   RegEx* pe;
   const char* file;
   bool ok;
   pred4(GrepFileCallback c, RegEx* i, const char* f) : cb(c), pe(i), file(f), ok(true) {}
   bool operator()(const match_results<mapfile::iterator>& m)
   {
      pe->pdata->t = RegExData::type_pf;
      pe->pdata->fm = m;
      pe->pdata->update();
      ok = cb(file, *pe);
      return ok;
   }
};
}
namespace{
void BuildFileList(std::list<std::string>* pl, const char* files, bool recurse)
{
   file_iterator start(files);
   file_iterator end;
   if(recurse)
   {
      // go through sub directories:
      char buf[MAX_PATH];
      BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcpy_s(buf, MAX_PATH, start.root()));
      if(*buf == 0)
      {
         BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcpy_s(buf, MAX_PATH, "."));
         BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcat_s(buf, MAX_PATH, directory_iterator::separator()));
         BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcat_s(buf, MAX_PATH, "*"));
      }
      else
      {
         BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcat_s(buf, MAX_PATH, directory_iterator::separator()));
         BOOST_REGEX_DETAIL_NS::overflow_error_if_not_zero(BOOST_REGEX_DETAIL_NS::strcat_s(buf, MAX_PATH, "*"));
      }
      directory_iterator dstart(buf);
      directory_iterator dend;

      // now get the file mask bit of "files":
      const char* ptr = files;
      while(*ptr) ++ptr;
      while((ptr != files) && (*ptr != *directory_iterator::separator()) && (*ptr != '/'))--ptr;
      if(ptr != files) ++ptr;

      while(dstart != dend)
      {
         // Verify that sprintf will not overflow:
         if(std::strlen(dstart.path()) + std::strlen(directory_iterator::separator()) + std::strlen(ptr) >= MAX_PATH)
         {
            // Oops overflow, skip this item:
            ++dstart;
            continue;
         }
#if BOOST_WORKAROUND(BOOST_MSVC, >= 1400) && !defined(_WIN32_WCE) && !defined(UNDER_CE)
         int r = (::sprintf_s)(buf, sizeof(buf), "%s%s%s", dstart.path(), directory_iterator::separator(), ptr);
#else
         int r = (std::sprintf)(buf, "%s%s%s", dstart.path(), directory_iterator::separator(), ptr);
#endif
         if(r < 0)
         {
            // sprintf failed, skip this item:
            ++dstart;
            continue;
         }
         BuildFileList(pl, buf, recurse);
         ++dstart;
      }
   }
   while(start != end)
   {
      pl->push_back(*start);
      ++start;
   }
}
}

unsigned int RegEx::GrepFiles(GrepFileCallback cb, const char* files, bool recurse, match_flag_type flags)
{
   unsigned int result = 0;
   std::list<std::string> file_list;
   BuildFileList(&file_list, files, recurse);
   std::list<std::string>::iterator start, end;
   start = file_list.begin();
   end = file_list.end();

   while(start != end)
   {
      mapfile map((*start).c_str());
      pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pf;
      pdata->fbase = map.begin();
      BOOST_REGEX_DETAIL_NS::pred4 pred(cb, this, (*start).c_str());
      int r = regex_grep(pred, map.begin(), map.end(), pdata->e, flags);
      result += r;
      ++start;
      pdata->clean();
      if(pred.ok == false)
         return result;
   }

   return result;
}


unsigned int RegEx::FindFiles(FindFilesCallback cb, const char* files, bool recurse, match_flag_type flags)
{
   unsigned int result = 0;
   std::list<std::string> file_list;
   BuildFileList(&file_list, files, recurse);
   std::list<std::string>::iterator start, end;
   start = file_list.begin();
   end = file_list.end();

   while(start != end)
   {
      mapfile map((*start).c_str());
      pdata->t = BOOST_REGEX_DETAIL_NS::RegExData::type_pf;
      pdata->fbase = map.begin();

      if(regex_search(map.begin(), map.end(), pdata->fm, pdata->e, flags))
      {
         ++result;
         if(false == cb((*start).c_str()))
            return result;
      }
      //pdata->update();
      ++start;
      //pdata->clean();
   }

   return result;
}
#endif

#ifdef BOOST_REGEX_V3
#define regex_replace regex_merge
#endif

std::string RegEx::Merge(const std::string& in, const std::string& fmt,
                    bool copy, match_flag_type flags)
{
   std::string result;
   BOOST_REGEX_DETAIL_NS::string_out_iterator<std::string> i(result);
   if(!copy) flags |= format_no_copy;
   regex_replace(i, in.begin(), in.end(), pdata->e, fmt.c_str(), flags);
   return result;
}

std::string RegEx::Merge(const char* in, const char* fmt,
                    bool copy, match_flag_type flags)
{
   std::string result;
   if(!copy) flags |= format_no_copy;
   BOOST_REGEX_DETAIL_NS::string_out_iterator<std::string> i(result);
   regex_replace(i, in, in + std::strlen(in), pdata->e, fmt, flags);
   return result;
}

std::size_t RegEx::Split(std::vector<std::string>& v, 
                      std::string& s,
                      match_flag_type flags,
                      unsigned max_count)
{
   return regex_split(std::back_inserter(v), s, pdata->e, flags, max_count);
}



//
// now operators for returning what matched in more detail:
//
std::size_t RegEx::Position(int i)const
{
   switch(pdata->t)
   {
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pc:
      return pdata->m[i].matched ? pdata->m[i].first - pdata->pbase : RegEx::npos;
#ifndef BOOST_REGEX_NO_FILEITER
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pf:
      return pdata->fm[i].matched ? pdata->fm[i].first - pdata->fbase : RegEx::npos;
#endif
   case BOOST_REGEX_DETAIL_NS::RegExData::type_copy:
      {
      std::map<int, std::ptrdiff_t, std::less<int> >::iterator pos = pdata->positions.find(i);
      if(pos == pdata->positions.end())
         return RegEx::npos;
      return (*pos).second;
      }
   }
   return RegEx::npos;
}

std::size_t RegEx::Marks()const
{
   return pdata->e.mark_count();
}


std::size_t RegEx::Length(int i)const
{
   switch(pdata->t)
   {
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pc:
      return pdata->m[i].matched ? pdata->m[i].second - pdata->m[i].first : RegEx::npos;
#ifndef BOOST_REGEX_NO_FILEITER
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pf:
      return pdata->fm[i].matched ? pdata->fm[i].second - pdata->fm[i].first : RegEx::npos;
#endif
   case BOOST_REGEX_DETAIL_NS::RegExData::type_copy:
      {
      std::map<int, std::string, std::less<int> >::iterator pos = pdata->strings.find(i);
      if(pos == pdata->strings.end())
         return RegEx::npos;
      return (*pos).second.size();
      }
   }
   return RegEx::npos;
}

bool RegEx::Matched(int i)const
{
   switch(pdata->t)
   {
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pc:
      return pdata->m[i].matched;
#ifndef BOOST_REGEX_NO_FILEITER
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pf:
      return pdata->fm[i].matched;
#endif      
   case BOOST_REGEX_DETAIL_NS::RegExData::type_copy:
      {
      std::map<int, std::string, std::less<int> >::iterator pos = pdata->strings.find(i);
      if(pos == pdata->strings.end())
         return false;
      return true;
      }
   }
   return false;
}


std::string RegEx::What(int i)const
{
   std::string result;
   switch(pdata->t)
   {
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pc:
      if(pdata->m[i].matched) 
         result.assign(pdata->m[i].first, pdata->m[i].second);
      break;
   case BOOST_REGEX_DETAIL_NS::RegExData::type_pf:
      if(pdata->m[i].matched) 
         result.assign(to_string(pdata->m[i].first, pdata->m[i].second));
      break;
   case BOOST_REGEX_DETAIL_NS::RegExData::type_copy:
      {
      std::map<int, std::string, std::less<int> >::iterator pos = pdata->strings.find(i);
      if(pos != pdata->strings.end())
         result = (*pos).second;
      break;
      }
   }
   return result;
}

const std::size_t RegEx::npos = ~static_cast<std::size_t>(0);

} // namespace boost

#if defined(__BORLANDC__) && (__BORLANDC__ >= 0x550) && (__BORLANDC__ <= 0x551) && !defined(_RWSTD_COMPILE_INSTANTIATE)
//
// this is an ugly hack to work around an ugly problem:
// by default this file will produce unresolved externals during
// linking unless _RWSTD_COMPILE_INSTANTIATE is defined (Borland bug).
// However if _RWSTD_COMPILE_INSTANTIATE is defined then we get separate
// copies of basic_string's static data in the RTL and this DLL, this messes
// with basic_string's memory management and results in run-time crashes,
// Oh sweet joy of Catch 22....
//
namespace std{
template<> template<>
basic_string<char>& BOOST_REGEX_DECL
basic_string<char>::replace<const char*>(char* f1, char* f2, const char* i1, const char* i2)
{
   unsigned insert_pos = f1 - begin();
   unsigned remove_len = f2 - f1;
   unsigned insert_len = i2 - i1;
   unsigned org_size = size();
   if(insert_len > remove_len)
   {
      append(insert_len-remove_len, ' ');
      std::copy_backward(begin() + insert_pos + remove_len, begin() + org_size, end());
      std::copy(i1, i2, begin() + insert_pos);
   }
   else
   {
      std::copy(begin() + insert_pos + remove_len, begin() + org_size, begin() + insert_pos + insert_len);
      std::copy(i1, i2, begin() + insert_pos);
      erase(size() + insert_len - remove_len);
   }
   return *this;
}
template<> template<>
basic_string<wchar_t>& BOOST_REGEX_DECL
basic_string<wchar_t>::replace<const wchar_t*>(wchar_t* f1, wchar_t* f2, const wchar_t* i1, const wchar_t* i2)
{
   unsigned insert_pos = f1 - begin();
   unsigned remove_len = f2 - f1;
   unsigned insert_len = i2 - i1;
   unsigned org_size = size();
   if(insert_len > remove_len)
   {
      append(insert_len-remove_len, ' ');
      std::copy_backward(begin() + insert_pos + remove_len, begin() + org_size, end());
      std::copy(i1, i2, begin() + insert_pos);
   }
   else
   {
      std::copy(begin() + insert_pos + remove_len, begin() + org_size, begin() + insert_pos + insert_len);
      std::copy(i1, i2, begin() + insert_pos);
      erase(size() + insert_len - remove_len);
   }
   return *this;
}
} // namespace std
#endif

#endif
















