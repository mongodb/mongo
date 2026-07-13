namespace mongo {

/**
 * TODO SERVER-68868: Remove this file once we don't have the method anymore.
 */
class OperationContext {
public:
    bool uninterruptibleLocksRequested_DO_NOT_USE() const {
        return true;
    }
};

void f(OperationContext* opCtx) {
    opCtx->uninterruptibleLocksRequested_DO_NOT_USE();
}

}  // namespace mongo
