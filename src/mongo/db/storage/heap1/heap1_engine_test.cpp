// heap1_engine_test.cpp

#include "mongo/db/storage/heap1/heap1_engine.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"

namespace mongo {

    class Heap1KVHarnessHelper : public KVHarnessHelper {
    public:
        Heap1KVHarnessHelper() : _engine( new Heap1Engine()) {}

        virtual KVEngine* restartEngine() {
            // Intentionally not restarting since heap doesn't keep data across restarts
            return _engine.get();
        }

        virtual KVEngine* getEngine() { return _engine.get(); }

    private:
        boost::scoped_ptr<Heap1Engine> _engine;
    };

    KVHarnessHelper* KVHarnessHelper::create() {
        return new Heap1KVHarnessHelper();
    }
}
