/**
 * Enum-like class for database/collection states in the finite state machine.
 */
class State {
    static DATABASE_ABSENT = 0;
    static DATABASE_PRESENT_COLLECTION_ABSENT = 1;
    static COLLECTION_PRESENT_SHARDED_RANGE = 2;
    static COLLECTION_PRESENT_SHARDED_HASHED = 3;
    static COLLECTION_PRESENT_UNSPLITTABLE = 4;
    static COLLECTION_PRESENT_UNTRACKED = 5;

    static getName(stateId) {
        switch (stateId) {
            case State.DATABASE_ABSENT:
                return "DatabaseAbsent";
            case State.DATABASE_PRESENT_COLLECTION_ABSENT:
                return "DatabasePresent::CollectionAbsent";
            case State.COLLECTION_PRESENT_SHARDED_RANGE:
                return "CollectionPresent::ShardedCollection(Range)";
            case State.COLLECTION_PRESENT_SHARDED_HASHED:
                return "CollectionPresent::ShardedCollection(Hashed)";
            case State.COLLECTION_PRESENT_UNSPLITTABLE:
                return "CollectionPresent::UnsplittableCollection";
            case State.COLLECTION_PRESENT_UNTRACKED:
                return "CollectionPresent::UntrackedCollection";
            default:
                throw new Error(`Invalid state ID: ${stateId}`);
        }
    }
}

export {State};
