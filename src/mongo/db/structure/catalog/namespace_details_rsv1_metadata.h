// namespace_details_rsv1_metadata.h

#include "mongo/db/structure/record_store_v1_base.h"

namespace mongo {

    /*
     * NOTE: NamespaceDetails will become a struct
     *      all dur, etc... will move here
     */
    class NamespaceDetailsRSV1MetaData : public RecordStoreV1MetaData {
    public:
        explicit NamespaceDetailsRSV1MetaData( NamespaceDetails* details ) {
            _details = details;
        }

        virtual ~NamespaceDetailsRSV1MetaData(){}

        virtual int bucket(int size) const {
            return _details->bucket( size );
        }

        virtual const DiskLoc& capExtent() const {
            return _details->capExtent();
        }

        virtual void setCapExtent( const DiskLoc& loc ) {
            _details->setCapExtent( loc );
        }

        virtual const DiskLoc& capFirstNewRecord() const {
            return _details->capFirstNewRecord();
        }

        virtual void setCapFirstNewRecord( const DiskLoc& loc ) {
            _details->setCapFirstNewRecord( loc );
        }

        virtual bool capLooped() const {
            return _details->capLooped();
        }

        virtual void clearSystemFlags() {
            _details->clearSystemFlags();
        }

        virtual long long dataSize() const {
            return _details->dataSize();
        }
        virtual long long numRecords() const {
            return _details->numRecords();
        }

        virtual void incrementStats( long long dataSizeIncrement,
                                     long long numRecordsIncrement ) {
            _details->incrementStats( dataSizeIncrement, numRecordsIncrement );
        }

        virtual void setStats( long long dataSizeIncrement,
                               long long numRecordsIncrement ) {
            _details->setStats( dataSizeIncrement,
                                numRecordsIncrement );
        }

        virtual const DiskLoc& deletedListEntry( int bucket ) const {
            return _details->deletedListEntry( bucket );
        }

        virtual void setDeletedListEntry( int bucket, const DiskLoc& loc ) {
            _details->setDeletedListEntry( bucket, loc );
        }

        virtual void orphanDeletedList() {
            _details->orphanDeletedList();
        }

        virtual const DiskLoc& firstExtent() const {
            return _details->firstExtent();
        }

        virtual void setFirstExtent( const DiskLoc& loc ) {
            _details->setFirstExtent( loc );
        }

        virtual const DiskLoc& lastExtent() const {
            return _details->lastExtent();
        }

        virtual void setLastExtent( const DiskLoc& loc ) {
            _details->setLastExtent( loc );
        }

        virtual bool isCapped() const {
            return _details->isCapped();
        }

        virtual bool isUserFlagSet( int flag ) const {
            return _details->isUserFlagSet( flag );
        }

        virtual int lastExtentSize() const {
            return _details->lastExtentSize();
        }

        virtual void setLastExtentSize( int newMax ) {
            _details->setLastExtentSize( newMax );
        }

        virtual long long maxCappedDocs() const {
            return _details->maxCappedDocs();
        }

        virtual double paddingFactor() const {
            return _details->paddingFactor();
        }

        virtual void setPaddingFactor( double paddingFactor ) {
            _details->setPaddingFactor( paddingFactor );
        }

        virtual int quantizePowerOf2AllocationSpace(int allocSize) const {
            return _details->quantizePowerOf2AllocationSpace( allocSize );
        }

    private:
        NamespaceDetails* _details;
    };

}
