#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/json.h"
#include "mongo/util/log.h"

namespace mongo {
    class GeoIndexTest : public mongo::unittest::Test {
    protected:
        int myVar;
        void setUp() {
            myVar = 10;
        }
    };

    void printKeyInfo(const BSONObj& b){
        std::string k = b.getField("").String();
        log() << k;
    }

    TEST_F(GeoIndexTest, TestKeysForPoint){
        BSONObj infoObj_fine = fromjson("{ finestIndexedLevel : 30, coarsestIndexedLevel : 29}");
        BSONObj infoObj_coarse = fromjson("{ finestIndexedLevel : 14, coarsestIndexedLevel : 7}");

        S2IndexingParams params_fine, params_coarse;
        ExpressionParams::parse2dsphereParams(infoObj_fine, &params_fine);
        ExpressionParams::parse2dsphereParams(infoObj_coarse, &params_coarse);

        BSONObj testPoint = fromjson("{loc : {type : 'Point', coordinates : [10,10]}}");
        BSONObj keyPattern = fromjson("{ loc : '2dsphere'}");

        BSONObjSet out_point_fine, out_point_coarse;
        ExpressionKeysPrivate::getS2Keys(testPoint, keyPattern, params_coarse, &out_point_coarse);

        log() << "Coarse keys for point [10,10]";
        for(BSONObj b : out_point_coarse){
            printKeyInfo(b);
        }

        ExpressionKeysPrivate::getS2Keys(testPoint, keyPattern, params_fine, &out_point_fine);
        log() << "Fine keys for point [10,10]";
        for(BSONObj b : out_point_fine){
            printKeyInfo(b);
        }

        /*
        BSONObjSet out_polygon;
        BSONObj testPolygon = fromjson("{loc : {type : 'Polygon', coordinates :[[ [10,10],[11,10],[11,11],[10,11],[10,10] ]]}}");
        ExpressionKeysPrivate::getS2Keys(testPolygon, keyPattern, params, &out_polygon);
        log() << "Keys for polygon";
        for(BSONObj b: out_polygon){
            printKeyInfo(b);
        }
        */
        log() << "Done";
    }
}
