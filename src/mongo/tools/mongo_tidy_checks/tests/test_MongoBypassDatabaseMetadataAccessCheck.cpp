namespace mongo {
class BypassDatabaseMetadataAccessCheck {
public:
    BypassDatabaseMetadataAccessCheck(int test) {
        m_test = test;
    }

private:
    int m_test;
};

BypassDatabaseMetadataAccessCheck bypass(2);

}  // namespace mongo
