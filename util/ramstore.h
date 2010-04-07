// ramstore.h

// mmap.h

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

extern bool checkNsFilesOnLoad;

class RamStoreFile : public MongoFile {
    char name[256];
    struct Node { 
        char *p;
        int len;
        Node() : len(0) { }
        void check();
    };
    map<int,Node> _m;
    long _len;

    static void validate();
    void check();

    int _last;

    /* maxLen can be -1 for existing data */
    void* at(int offset, int maxLen);

protected:
    virtual void close() { 
        cout << "ramstore dealloc not yet implemented" << endl;
        if( _len ) {
            _len = 0;
        }
    }
    virtual void flush(bool sync) { }

public:
    ~RamStoreFile();
    RamStoreFile();

    virtual long length() { return _len; }

    class Pointer {
        RamStoreFile* _f;
        friend class RamStoreFile;
    public:
        void* at(int offset, int maxLen) { 
            assert( maxLen <= /*MaxBSONObjectSize*/4*1024*1024 + 128 );
            return _f->at(offset,maxLen);
        }
        bool isNull() const { return _f == 0; }
    };

    Pointer map( const char *filename ) { 
        assert(false); return Pointer(); 
    }
    Pointer map(const char *_filename, long &length, int options=0) { 
        strncpy(name, _filename, sizeof(name)-1);
        Pointer p;
        p._f = this;
        return p;
    }

    static bool exists(boost::filesystem::path p) {
        return false;
    }
};
