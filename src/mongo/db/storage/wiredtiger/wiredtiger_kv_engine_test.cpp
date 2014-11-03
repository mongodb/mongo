// wiredtiger_kv_engine_test.cpp

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

    class WiredTigerKVHarnessHelper : public KVHarnessHelper {
    public:
        WiredTigerKVHarnessHelper()
            : _dbpath( "wt-kv-harness" ) {
            _engine.reset( new WiredTigerKVEngine( _dbpath.path() ) );
        }

        virtual ~WiredTigerKVHarnessHelper() {
            _engine.reset( NULL );
        }

        virtual KVEngine* restartEngine() {
            _engine.reset( NULL );
            _engine.reset( new WiredTigerKVEngine( _dbpath.path() ) );
            return _engine.get();
        }

        virtual KVEngine* getEngine() { return _engine.get(); }

    private:
        unittest::TempDir _dbpath;
        boost::scoped_ptr<WiredTigerKVEngine> _engine;
    };

    KVHarnessHelper* KVHarnessHelper::create() {
        return new WiredTigerKVHarnessHelper();
    }
}
