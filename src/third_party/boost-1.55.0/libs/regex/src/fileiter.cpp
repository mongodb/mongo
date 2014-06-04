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
  *   FILE:        fileiter.cpp
  *   VERSION:     see <boost/version.hpp>
  *   DESCRIPTION: Implements file io primitives + directory searching for class boost::RegEx.
  */


#define BOOST_REGEX_SOURCE

#include <boost/config.hpp>
#include <climits>
#include <stdexcept>
#include <string>
#include <boost/throw_exception.hpp>
#include <boost/regex/v4/fileiter.hpp>
#include <boost/regex/v4/regex_workaround.hpp>
#include <boost/regex/pattern_except.hpp>

#include <cstdio>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{
   using ::sprintf;
   using ::fseek;
   using ::fread;
   using ::ftell;
   using ::fopen;
   using ::fclose;
   using ::FILE;
   using ::strcpy;
   using ::strcpy;
   using ::strcat;
   using ::strcmp;
   using ::strlen;
}
#endif


#ifndef BOOST_REGEX_NO_FILEITER

#if defined(__CYGWIN__) || defined(__CYGWIN32__)
#include <sys/cygwin.h>
#endif

#ifdef BOOST_MSVC
#  pragma warning(disable: 4800)
#endif

namespace boost{
   namespace re_detail{
// start with the operating system specific stuff:

#if (defined(__BORLANDC__) || defined(BOOST_REGEX_FI_WIN32_DIR) || defined(BOOST_MSVC)) && !defined(BOOST_RE_NO_WIN32)

// platform is DOS or Windows
// directories are separated with '\\'
// and names are insensitive of case

BOOST_REGEX_DECL const char* _fi_sep = "\\";
const char* _fi_sep_alt = "/";
#define BOOST_REGEX_FI_TRANSLATE(c) std::tolower(c)

#else

// platform is not DOS or Windows
// directories are separated with '/'
// and names are sensitive of case

BOOST_REGEX_DECL const char* _fi_sep = "/";
const char* _fi_sep_alt = _fi_sep;
#define BOOST_REGEX_FI_TRANSLATE(c) c

#endif

#ifdef BOOST_REGEX_FI_WIN32_MAP

void mapfile::open(const char* file)
{
#if defined(BOOST_NO_ANSI_APIS)
   int filename_size = strlen(file);
   LPWSTR wide_file = (LPWSTR)_alloca( (filename_size + 1) * sizeof(WCHAR) );
   if(::MultiByteToWideChar(CP_ACP, 0,  file, filename_size,  wide_file, filename_size + 1) == 0)
      hfile = INVALID_HANDLE_VALUE;
   else
      hfile = CreateFileW(wide_file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
#elif defined(__CYGWIN__)||defined(__CYGWIN32__)
   char win32file[ MAX_PATH ];
   cygwin_conv_to_win32_path( file, win32file );
   hfile = CreateFileA(win32file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
#else
   hfile = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
#endif
   if(hfile != INVALID_HANDLE_VALUE)
   {
      hmap = CreateFileMapping(hfile, 0, PAGE_READONLY, 0, 0, 0);
      if((hmap == INVALID_HANDLE_VALUE) || (hmap == NULL))
      {
         CloseHandle(hfile);
         hmap = 0;
         hfile = 0;
         std::runtime_error err("Unable to create file mapping.");
         boost::re_detail::raise_runtime_error(err);
      }
      _first = static_cast<const char*>(MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0));
      if(_first == 0)
      {
         CloseHandle(hmap);
         CloseHandle(hfile);
         hmap = 0;
         hfile = 0;
         std::runtime_error err("Unable to create file mapping.");
      }
      _last = _first + GetFileSize(hfile, 0);
   }
   else
   {
      hfile = 0;
#ifndef BOOST_NO_EXCEPTIONS
      throw std::runtime_error("Unable to open file.");
#else
      BOOST_REGEX_NOEH_ASSERT(hfile != INVALID_HANDLE_VALUE);
#endif
   }
}

void mapfile::close()
{
   if(hfile != INVALID_HANDLE_VALUE)
   {
      UnmapViewOfFile((void*)_first);
      CloseHandle(hmap);
      CloseHandle(hfile);
      hmap = hfile = 0;
      _first = _last = 0;
   }
}

#elif !defined(BOOST_RE_NO_STL)

mapfile_iterator& mapfile_iterator::operator = (const mapfile_iterator& i)
{
   if(file && node)
      file->unlock(node);
   file = i.file;
   node = i.node;
   offset = i.offset;
   if(file)
      file->lock(node);
   return *this;
}

mapfile_iterator& mapfile_iterator::operator++ ()
{
   if((++offset == mapfile::buf_size) && file)
   {
      ++node;
      offset = 0;
      file->lock(node);
      file->unlock(node-1);
   }
   return *this;
}

mapfile_iterator mapfile_iterator::operator++ (int)
{
   mapfile_iterator temp(*this);
   if((++offset == mapfile::buf_size) && file)
   {
      ++node;
      offset = 0;
      file->lock(node);
      file->unlock(node-1);
   }
   return temp;
}

mapfile_iterator& mapfile_iterator::operator-- ()
{
   if((offset == 0) && file)
   {
      --node;
      offset = mapfile::buf_size - 1;
      file->lock(node);
      file->unlock(node + 1);
   }
   else
      --offset;
   return *this;
}

mapfile_iterator mapfile_iterator::operator-- (int)
{
   mapfile_iterator temp(*this);
   if((offset == 0) && file)
   {
      --node;
      offset = mapfile::buf_size - 1;
      file->lock(node);
      file->unlock(node + 1);
   }
   else
      --offset;
   return temp;
}

mapfile_iterator operator + (const mapfile_iterator& i, long off)
{
   mapfile_iterator temp(i);
   temp += off;
   return temp;
}

mapfile_iterator operator - (const mapfile_iterator& i, long off)
{
   mapfile_iterator temp(i);
   temp -= off;
   return temp;
}

mapfile::iterator mapfile::begin()const
{
   return mapfile_iterator(this, 0);
}

mapfile::iterator mapfile::end()const
{
   return mapfile_iterator(this, _size);
}

void mapfile::lock(pointer* node)const
{
   BOOST_ASSERT(node >= _first);
   BOOST_ASSERT(node <= _last);
   if(node < _last)
   {
      if(*node == 0)
      {
         if(condemed.empty())
         {
            *node = new char[sizeof(int) + buf_size];
            *(reinterpret_cast<int*>(*node)) = 1;
         }
         else
         {
            pointer* p = condemed.front();
            condemed.pop_front();
            *node = *p;
            *p = 0;
            *(reinterpret_cast<int*>(*node)) = 1;
         }
 
        std::size_t read_size = 0; 
        int read_pos = std::fseek(hfile, (node - _first) * buf_size, SEEK_SET); 

        if(0 == read_pos && node == _last - 1) 
           read_size = std::fread(*node + sizeof(int), _size % buf_size, 1, hfile); 
        else
           read_size = std::fread(*node + sizeof(int), buf_size, 1, hfile);
#ifndef BOOST_NO_EXCEPTIONS 
        if((read_size == 0) || (std::ferror(hfile)))
        { 
           throw std::runtime_error("Unable to read file."); 
        } 
#else 
        BOOST_REGEX_NOEH_ASSERT((0 == std::ferror(hfile)) && (read_size != 0)); 
#endif 
      }
      else
      {
         if(*reinterpret_cast<int*>(*node) == 0)
         {
            *reinterpret_cast<int*>(*node) = 1;
            condemed.remove(node);
         }
         else
            ++(*reinterpret_cast<int*>(*node));
      }
   }
}

void mapfile::unlock(pointer* node)const
{
   BOOST_ASSERT(node >= _first);
   BOOST_ASSERT(node <= _last);
   if(node < _last)
   {
      if(--(*reinterpret_cast<int*>(*node)) == 0)
      {
         condemed.push_back(node);
      }
   }
}

long int get_file_length(std::FILE* hfile)
{
   long int result;
   std::fseek(hfile, 0, SEEK_END);
   result = std::ftell(hfile);
   std::fseek(hfile, 0, SEEK_SET);
   return result;
}


void mapfile::open(const char* file)
{
   hfile = std::fopen(file, "rb");
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   if(hfile != 0)
   {
      _size = get_file_length(hfile);
      long cnodes = (_size + buf_size - 1) / buf_size;

      // check that number of nodes is not too high:
      if(cnodes > (long)((INT_MAX) / sizeof(pointer*)))
      {
         std::fclose(hfile);
         hfile = 0;
         _size = 0;
         return;
      }

      _first = new pointer[(int)cnodes];
      _last = _first + cnodes;
      std::memset(_first, 0, cnodes*sizeof(pointer));
   }
   else
   {
       std::runtime_error err("Unable to open file.");
   }
#ifndef BOOST_NO_EXCEPTIONS
   }catch(...)
   { close(); throw; }
#endif
}

void mapfile::close()
{
   if(hfile != 0)
   {
      pointer* p = _first;
      while(p != _last)
      {
         if(*p)
            delete[] *p;
         ++p;
      }
      delete[] _first;
      _size = 0;
      _first = _last = 0;
      std::fclose(hfile);
      hfile = 0;
      condemed.erase(condemed.begin(), condemed.end());
   }
}


#endif

inline _fi_find_handle find_first_file(const char* wild,  _fi_find_data& data)
{
#ifdef BOOST_NO_ANSI_APIS
   std::size_t wild_size = std::strlen(wild);
   LPWSTR wide_wild = (LPWSTR)_alloca( (wild_size + 1) * sizeof(WCHAR) );
   if (::MultiByteToWideChar(CP_ACP, 0,  wild, wild_size,  wide_wild, wild_size + 1) == 0)
      return _fi_invalid_handle;

   return FindFirstFileW(wide_wild, &data);
#else
   return FindFirstFileA(wild, &data);
#endif
}

inline bool find_next_file(_fi_find_handle hf,  _fi_find_data& data)
{
#ifdef BOOST_NO_ANSI_APIS
   return FindNextFileW(hf, &data);
#else
   return FindNextFileA(hf, &data);
#endif
}
   
inline void copy_find_file_result_with_overflow_check(const _fi_find_data& data,  char* path, size_t max_size)
{
#ifdef BOOST_NO_ANSI_APIS
   if (::WideCharToMultiByte(CP_ACP, 0,  data.cFileName, -1,  path, max_size,  NULL, NULL) == 0)
      re_detail::overflow_error_if_not_zero(1);
#else
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(path, max_size,  data.cFileName));
#endif
}

inline bool is_not_current_or_parent_path_string(const _fi_find_data& data)
{
#ifdef BOOST_NO_ANSI_APIS
   return (std::wcscmp(data.cFileName, L".") && std::wcscmp(data.cFileName, L".."));
#else
   return (std::strcmp(data.cFileName, ".") && std::strcmp(data.cFileName, ".."));
#endif
}


file_iterator::file_iterator()
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   ptr = _path;
   *_path = 0;
   *_root = 0;
   ref = new file_iterator_ref();
   BOOST_REGEX_NOEH_ASSERT(ref)
   ref->hf = _fi_invalid_handle;
   ref->count = 1;
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      delete ref;
      throw;
   }
#endif
}

file_iterator::file_iterator(const char* wild)
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, wild));
   ptr = _root;
   while(*ptr)++ptr;
   while((ptr > _root) && (*ptr != *_fi_sep) && (*ptr != *_fi_sep_alt))--ptr;
   if((ptr == _root) && ( (*ptr== *_fi_sep) || (*ptr==*_fi_sep_alt) ) )
   {
     _root[1]='\0';
     re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, _root));
   }
   else
   {
     *ptr = 0;
     re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, _root));
     if(*_path == 0)
       re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, "."));
     re_detail::overflow_error_if_not_zero(re_detail::strcat_s(_path, MAX_PATH, _fi_sep));
   }
   ptr = _path + std::strlen(_path);

   ref = new file_iterator_ref();
   BOOST_REGEX_NOEH_ASSERT(ref)
   ref->hf = find_first_file(wild,  ref->_data);
   ref->count = 1;

   if(ref->hf == _fi_invalid_handle)
   {
      *_path = 0;
      ptr = _path;
   }
   else
   {
      copy_find_file_result_with_overflow_check(ref->_data,  ptr, (MAX_PATH - (ptr - _path)));
      if(ref->_data.dwFileAttributes & _fi_dir)
         next();
   }
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      delete ref;
      throw;
   }
#endif
}

file_iterator::file_iterator(const file_iterator& other)
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, other._root));
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, other._path));
   ptr = _path + (other.ptr - other._path);
   ref = other.ref;
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      throw;
   }
#endif
   ++(ref->count);
}

file_iterator& file_iterator::operator=(const file_iterator& other)
{
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, other._root));
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, other._path));
   ptr = _path + (other.ptr - other._path);
   if(--(ref->count) == 0)
   {
      if(ref->hf != _fi_invalid_handle)
         FindClose(ref->hf);
      delete ref;
   }
   ref = other.ref;
   ++(ref->count);
   return *this;
}


file_iterator::~file_iterator()
{
   delete[] _root;
   delete[] _path;
   if(--(ref->count) == 0)
   {
      if(ref->hf != _fi_invalid_handle)
         FindClose(ref->hf);
      delete ref;
   }
}

file_iterator file_iterator::operator++(int)
{
   file_iterator temp(*this);
   next();
   return temp;
}


void file_iterator::next()
{
   if(ref->hf != _fi_invalid_handle)
   {
      bool cont = true;
      while(cont)
      {
         cont = find_next_file(ref->hf, ref->_data);
         if(cont && ((ref->_data.dwFileAttributes & _fi_dir) == 0))
            break;
      }
      if(!cont)
      {
         // end of sequence
         FindClose(ref->hf);
         ref->hf = _fi_invalid_handle;
         *_path = 0;
         ptr = _path;
      }
      else
         copy_find_file_result_with_overflow_check(ref->_data,  ptr, MAX_PATH - (ptr - _path));
   }
}



directory_iterator::directory_iterator()
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   ptr = _path;
   *_path = 0;
   *_root = 0;
   ref = new file_iterator_ref();
   BOOST_REGEX_NOEH_ASSERT(ref)
   ref->hf = _fi_invalid_handle;
   ref->count = 1;
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      delete ref;
      throw;
   }
#endif
}

directory_iterator::directory_iterator(const char* wild)
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, wild));
   ptr = _root;
   while(*ptr)++ptr;
   while((ptr > _root) && (*ptr != *_fi_sep) && (*ptr != *_fi_sep_alt))--ptr;

   if((ptr == _root) && ( (*ptr== *_fi_sep) || (*ptr==*_fi_sep_alt) ) )
   {
     _root[1]='\0';
     re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, _root));
   }
   else
   {
     *ptr = 0;
     re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, _root));
     if(*_path == 0)
       re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, "."));
     re_detail::overflow_error_if_not_zero(re_detail::strcat_s(_path, MAX_PATH, _fi_sep));
   }
   ptr = _path + std::strlen(_path);

   ref = new file_iterator_ref();
   BOOST_REGEX_NOEH_ASSERT(ref)
   ref->count = 1;
   ref->hf = find_first_file(wild,  ref->_data);
   if(ref->hf == _fi_invalid_handle)
   {
      *_path = 0;
      ptr = _path;
   }
   else
   {
      copy_find_file_result_with_overflow_check(ref->_data,  ptr, MAX_PATH - (ptr - _path));
      if(((ref->_data.dwFileAttributes & _fi_dir) == 0) || (std::strcmp(ptr, ".") == 0) || (std::strcmp(ptr, "..") == 0))
         next();
   }
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      delete ref;
      throw;
   }
#endif
}

directory_iterator::~directory_iterator()
{
   delete[] _root;
   delete[] _path;
   if(--(ref->count) == 0)
   {
      if(ref->hf != _fi_invalid_handle)
         FindClose(ref->hf);
      delete ref;
   }
}

directory_iterator::directory_iterator(const directory_iterator& other)
{
   _root = _path = 0;
   ref = 0;
#ifndef BOOST_NO_EXCEPTIONS
   try{
#endif
   _root = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_root)
   _path = new char[MAX_PATH];
   BOOST_REGEX_NOEH_ASSERT(_path)
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, other._root));
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, other._path));
   ptr = _path + (other.ptr - other._path);
   ref = other.ref;
#ifndef BOOST_NO_EXCEPTIONS
   }
   catch(...)
   {
      delete[] _root;
      delete[] _path;
      throw;
   }
#endif
   ++(ref->count);
}

directory_iterator& directory_iterator::operator=(const directory_iterator& other)
{
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_root, MAX_PATH, other._root));
   re_detail::overflow_error_if_not_zero(re_detail::strcpy_s(_path, MAX_PATH, other._path));
   ptr = _path + (other.ptr - other._path);
   if(--(ref->count) == 0)
   {
      if(ref->hf != _fi_invalid_handle)
         FindClose(ref->hf);
      delete ref;
   }
   ref = other.ref;
   ++(ref->count);
   return *this;
}

directory_iterator directory_iterator::operator++(int)
{
   directory_iterator temp(*this);
   next();
   return temp;
}

void directory_iterator::next()
{
   if(ref->hf != _fi_invalid_handle)
   {
      bool cont = true;
      while(cont)
      {
         cont = find_next_file(ref->hf, ref->_data);
         if(cont && (ref->_data.dwFileAttributes & _fi_dir))
         {
            if(is_not_current_or_parent_path_string(ref->_data))
               break;
         }
      }
      if(!cont)
      {
         // end of sequence
         FindClose(ref->hf);
         ref->hf = _fi_invalid_handle;
         *_path = 0;
         ptr = _path;
      }
      else
         copy_find_file_result_with_overflow_check(ref->_data,  ptr, MAX_PATH - (ptr - _path));
   }
}


#ifdef BOOST_REGEX_FI_POSIX_DIR

struct _fi_priv_data
{
   char root[MAX_PATH];
   char* mask;
   DIR* d;
   _fi_priv_data(const char* p);
};

_fi_priv_data::_fi_priv_data(const char* p)
{
   std::strcpy(root, p);
   mask = root;
   while(*mask) ++mask;
   while((mask > root) && (*mask != *_fi_sep) && (*mask != *_fi_sep_alt)) --mask;
   if(mask == root && ((*mask== *_fi_sep) || (*mask == *_fi_sep_alt)) )
   {
      root[1] = '\0';
      std::strcpy(root+2, p+1);
      mask = root+2;
   }
   else if(mask == root)
   {
      root[0] = '.';
      root[1] = '\0';
      std::strcpy(root+2, p);
      mask = root+2;
   }
   else
   {
      *mask = 0;
      ++mask;
   }
}

bool iswild(const char* mask, const char* name)
{
   while(*mask && *name)
   {
      switch(*mask)
      {
      case '?':
         ++name;
         ++mask;
         continue;
      case '*':
         ++mask;
         if(*mask == 0)
            return true;
         while(*name)
         {
            if(iswild(mask, name))
               return true;
            ++name;
         }
         return false;
      case '.':
         if(0 == *name)
         {
            ++mask;
            continue;
         }
         // fall through:
      default:
         if(BOOST_REGEX_FI_TRANSLATE(*mask) != BOOST_REGEX_FI_TRANSLATE(*name))
            return false;
         ++mask;
         ++name;
         continue;
      }
   }
   if(*mask != *name)
      return false;
   return true;
}

unsigned _fi_attributes(const char* root, const char* name)
{
   char buf[MAX_PATH];
   // verify that we can not overflow:
   if(std::strlen(root) + std::strlen(_fi_sep) + std::strlen(name) >= MAX_PATH)
      return 0;
   int r;
   if( ( (root[0] == *_fi_sep) || (root[0] == *_fi_sep_alt) ) && (root[1] == '\0') )
      r = (std::sprintf)(buf, "%s%s", root, name);
   else
      r = (std::sprintf)(buf, "%s%s%s", root, _fi_sep, name);
   if(r < 0)
      return 0; // sprintf failed
   DIR* d = opendir(buf);
   if(d)
   {
      closedir(d);
      return _fi_dir;
   }
   return 0;
}

_fi_find_handle _fi_FindFirstFile(const char* lpFileName, _fi_find_data* lpFindFileData)
{
   _fi_find_handle dat = new _fi_priv_data(lpFileName);

   DIR* h = opendir(dat->root);
   dat->d = h;
   if(h != 0)
   {
      if(_fi_FindNextFile(dat, lpFindFileData))
         return dat;
      closedir(h);
   }
   delete dat;
   return 0;
}

bool _fi_FindNextFile(_fi_find_handle dat, _fi_find_data* lpFindFileData)
{
   dirent* d;
   do
   {
      d = readdir(dat->d);
   } while(d && !iswild(dat->mask, d->d_name));

   if(d)
   {
      std::strcpy(lpFindFileData->cFileName, d->d_name);
      lpFindFileData->dwFileAttributes = _fi_attributes(dat->root, d->d_name);
      return true;
   }
   return false;
}

bool _fi_FindClose(_fi_find_handle dat)
{
   closedir(dat->d);
   delete dat;
   return true;
}

#endif

} // namespace re_detail
} // namspace boost

#endif    // BOOST_REGEX_NO_FILEITER












