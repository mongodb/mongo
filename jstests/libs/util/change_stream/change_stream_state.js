/**
 * Enum-like class for database/collection states in the finite state machine.
 */
class State {
    static DATABASE_ABSENT = 0;
    static DATABASE_PRESENT_COLLECTION_ABSENT = 1;
    static COLLECTION_PRESENT_SHARDED = 2;
    static COLLECTION_PRESENT_UNSPLITTABLE = 3;
    static COLLECTION_PRESENT_UNTRACKED = 4;

    static getName(stateId) {
        switch (stateId) {
            case State.DATABASE_ABSENT:
                return "DatabaseAbsent";
            case State.DATABASE_PRESENT_COLLECTION_ABSENT:
                return "DatabasePresent::CollectionAbsent";
            case State.COLLECTION_PRESENT_SHARDED:
                return "CollectionPresent::ShardedCollection";
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
