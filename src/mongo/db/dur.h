// @file dur.h durability support

#pragma once

#include "diskloc.h"
#include "mongommf.h"

namespace mongo {

    class NamespaceDetails;

    void mongoAbort(const char *msg);
    void abort(); // not defined -- use mongoAbort() instead

    namespace dur {

        void releasedWriteLock();

        // a smaller limit is likely better on 32 bit
#if defined(__i386__) || defined(_M_IX86)
        const unsigned UncommittedBytesLimit = 50 * 1024 * 1024;
#else
        const unsigned UncommittedBytesLimit = 100 * 1024 * 1024;
#endif

        /** Call during startup so durability module can initialize
            Throws if fatal error
            Does nothing if cmdLine.dur is false
         */
        void startup();

        class DurableInterface : boost::noncopyable {
        public:
            virtual ~DurableInterface() { log() << "ERROR warning ~DurableInterface not intended to be called" << endl; }

            /** Declare that a file has been created
                Normally writes are applied only after journaling, for safety.  But here the file
                is created first, and the journal will just replay the creation if the create didn't
                happen because of crashing.
            */
            virtual void createdFile(string filename, unsigned long long len) = 0;

            /** Declarations of write intent.

                Use these methods to declare "i'm about to write to x and it should be logged for redo."

                Failure to call writing...() is checked in _DEBUG mode by using a read only mapped view
                (i.e., you'll segfault if the code is covered in that situation).  The _DEBUG check doesn't
                verify that your length is correct though.
            */

            /** declare intent to write to x for up to len
                @return pointer where to write.  this is modified when testIntent is true.
            */
            virtual void* writingPtr(void *x, unsigned len) = 0;

            /** declare write intent; should already be in the write view to work correctly when testIntent is true.
                if you aren't, use writingPtr() instead.
            */
            virtual void declareWriteIntent(void *x, unsigned len) = 0;

            /** declare intent to write
                @param ofs offset within buf at which we will write
                @param len the length at ofs we will write
                @return new buffer pointer.  this is modified when testIntent is true.
            */
            virtual void* writingAtOffset(void *buf, unsigned ofs, unsigned len) = 0;

            /** declare intent to write
                @param ranges vector of pairs representing ranges.  Each pair
                comprises an offset from buf where a range begins, then the
                range length.
                @return new buffer pointer.  this is modified when testIntent is true.
             */
            virtual void* writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges ) = 0;

            /** Wait for acknowledgement of the next group commit.
                @return true if --dur is on.  There will be delay.
                @return false if --dur is off.
            */
            virtual bool awaitCommit() = 0;

            /** Commit immediately.

                Generally, you do not want to do this often, as highly granular committing may affect
                performance.

                Does not return until the commit is complete.

                You must be at least read locked when you call this.  Ideally, you are not write locked
                and then read operations can occur concurrently.

                Do not use this. Use commitIfNeeded() instead.

                @return true if --dur is on.
                @return false if --dur is off. (in which case there is action)
            */
            virtual bool commitNow() = 0;

            /** Commit if enough bytes have been modified. Current threshold is 50MB

                The idea is that long running write operations that dont yield
                (like creating an index or update with $atomic) can call this
                whenever the db is in a sane state and it will prevent commits
                from growing too large.
                @return true if commited
            */
            virtual bool commitIfNeeded(bool force=false) = 0;

            /** @return true if time to commit but does NOT do a commit */
            virtual bool aCommitIsNeeded() const = 0;

            /** Declare write intent for a DiskLoc.  @see DiskLoc::writing() */
            inline DiskLoc& writingDiskLoc(DiskLoc& d) { return *((DiskLoc*) writingPtr(&d, sizeof(d))); }

            /** Declare write intent for an int */
            inline little<int>& writingInt( little<int>& d) { return *static_cast<little<int>*>(writingPtr( &d, sizeof(d))); }

            /** "assume i've already indicated write intent, let me write"
                redeclaration is fine too, but this is faster.
            */
            template <typename T>
            inline
            T* alreadyDeclared(T *x) {
#if defined(_TESTINTENT)
                return (T*) MongoMMF::switchToPrivateView(x);
#else
                return x;
#endif
            }

            /** declare intent to write to x for sizeof(*x) */
            template <typename T>
            inline
            T* writing(T *x) {
                return (T*) writingPtr(x, sizeof(T));
            }

            /** Commits pending changes, flushes all changes to main data
                files, then removes the journal.
                
                This is useful as a "barrier" to ensure that writes before this
                call will never go through recovery and be applied to files
                that have had changes made after this call applied.
             */
            virtual void syncDataAndTruncateJournal() = 0;

            static DurableInterface& getDur() { return *_impl; }

        private:
            /** Intentionally unimplemented method.
             It's very easy to manipulate Record::data open ended.  Thus a call to writing(Record*) is suspect.
             This will override the templated version and yield an unresolved external.
             */
            Record* writing(Record* r);
            /** Intentionally unimplemented method. BtreeBuckets are allocated in buffers larger than sizeof( BtreeBucket ). */
//            BtreeBucket* writing( BtreeBucket* );
            /** Intentionally unimplemented method. NamespaceDetails may be based on references to 'Extra' objects. */
            NamespaceDetails* writing( NamespaceDetails* );

            static DurableInterface* _impl; // NonDurableImpl at startup()
            static void enableDurability(); // makes _impl a DurableImpl
            static void disableDurability(); // makes _impl a NonDurableImpl

            // these need to be able to enable/disable Durability
            friend void startup();
            friend class TempDisableDurability;
        }; // class DurableInterface

        class NonDurableImpl : public DurableInterface {
            void* writingPtr(void *x, unsigned len);
            void* writingAtOffset(void *buf, unsigned ofs, unsigned len) { return buf; }
            void* writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges) { return buf; }
            void declareWriteIntent(void *, unsigned);
            void createdFile(string filename, unsigned long long len) { }
            bool awaitCommit() { return false; }
            bool commitNow() { return false; }
            bool commitIfNeeded(bool) { return false; }
            bool aCommitIsNeeded() const { return false; }
            void syncDataAndTruncateJournal() {}
        };

        class DurableImpl : public DurableInterface {
            void* writingPtr(void *x, unsigned len);
            void* writingAtOffset(void *buf, unsigned ofs, unsigned len);
            void* writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges);
            void declareWriteIntent(void *, unsigned);
            void createdFile(string filename, unsigned long long len);
            bool awaitCommit();
            bool commitNow();
            bool aCommitIsNeeded() const;
            bool commitIfNeeded(bool);
            void syncDataAndTruncateJournal();
        };

    } // namespace dur

    inline dur::DurableInterface& getDur() { return dur::DurableInterface::getDur(); }

    /** declare that we are modifying a diskloc and this is a datafile write. */
    inline DiskLoc& DiskLoc::writing() const { return getDur().writingDiskLoc(*const_cast< DiskLoc * >( this )); }

}
