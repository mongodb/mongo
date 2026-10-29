namespace mongo {
namespace multiversion {

enum class FeatureCompatibilityVersion {
    kInvalid,
    kVersion_7_0,
};

}  // namespace multiversion

using FCV = multiversion::FeatureCompatibilityVersion;

struct ServerGlobalParams {
    struct FCVSnapshot {
        bool isLessThan(FCV version) const {
            return true;
        }
    } mutableFeatureCompatibility;
    const FCVSnapshot& featureCompatibility = mutableFeatureCompatibility;
};

void testFCVConstant() {
    const ServerGlobalParams mockParam = {};
    mockParam.featureCompatibility.isLessThan(
        multiversion::FeatureCompatibilityVersion::kVersion_7_0);
}

}  // namespace mongo
