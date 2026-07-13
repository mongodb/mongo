namespace mongo {
class UninterruptibleLockGuard {
public:
    UninterruptibleLockGuard(int test) {
        m_test = test;
    }

private:
    int m_test;
};

UninterruptibleLockGuard noInterrupt(2);

}  // namespace mongo
