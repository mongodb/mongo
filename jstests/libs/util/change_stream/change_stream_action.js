/**
 * Enum-like class for actions that can be performed on database/collection states.
 */
class Action {
    static INSERT_DOC = 0;
    static CREATE_DATABASE = 1;
    static CREATE_SHARDED_COLLECTION_RANGE = 2;
    static CREATE_SHARDED_COLLECTION_HASHED = 3;
    static CREATE_UNSPLITTABLE_COLLECTION = 4;
    static CREATE_UNTRACKED_COLLECTION = 5;
    static DROP_COLLECTION = 6;
    static DROP_DATABASE = 7;
    static RENAME_TO_NON_EXISTENT_SAME_DB = 8;
    static RENAME_TO_EXISTENT_SAME_DB = 9;
    static RENAME_TO_NON_EXISTENT_DIFFERENT_DB = 10;
    static RENAME_TO_EXISTENT_DIFFERENT_DB = 11;
    static SHARD_COLLECTION_RANGE = 12;
    static SHARD_COLLECTION_HASHED = 13;
    static UNSHARD_COLLECTION = 14;
    static RESHARD_COLLECTION_TO_RANGE = 15;
    static RESHARD_COLLECTION_TO_HASHED = 16;
    static MOVE_PRIMARY = 17;
    static MOVE_COLLECTION = 18;
    static MOVE_CHUNK = 19;

    static getName(actionId) {
        switch (actionId) {
            case Action.INSERT_DOC:
                return "insert doc";
            case Action.CREATE_DATABASE:
                return "create database";
            case Action.CREATE_SHARDED_COLLECTION_RANGE:
                return "create sharded collection (range)";
            case Action.CREATE_SHARDED_COLLECTION_HASHED:
                return "create sharded collection (hashed)";
            case Action.CREATE_UNSPLITTABLE_COLLECTION:
                return "create unsplittable collection";
            case Action.CREATE_UNTRACKED_COLLECTION:
                return "create untracked collection";
            case Action.DROP_COLLECTION:
                return "drop collection";
            case Action.DROP_DATABASE:
                return "drop database";
            case Action.RENAME_TO_NON_EXISTENT_SAME_DB:
                return "rename to non-existent collection same database";
            case Action.RENAME_TO_EXISTENT_SAME_DB:
                return "rename to existent collection same database";
            case Action.RENAME_TO_NON_EXISTENT_DIFFERENT_DB:
                return "rename to non-existent collection different database";
            case Action.RENAME_TO_EXISTENT_DIFFERENT_DB:
                return "rename to existent collection different database";
            case Action.SHARD_COLLECTION_RANGE:
                return "shard collection (range)";
            case Action.SHARD_COLLECTION_HASHED:
                return "shard collection (hashed)";
            case Action.UNSHARD_COLLECTION:
                return "unshard collection";
            case Action.RESHARD_COLLECTION_TO_RANGE:
                return "reshard collection (range)";
            case Action.RESHARD_COLLECTION_TO_HASHED:
                return "reshard collection (hashed)";
            case Action.MOVE_PRIMARY:
                return "move primary";
            case Action.MOVE_COLLECTION:
                return "move collection";
            case Action.MOVE_CHUNK:
                return "move chunk";
            default:
                throw new Error(`Invalid action ID: ${actionId}`);
        }
    }

    /**
     * Get all action IDs.
     * @returns {Array<number>} Array of all action IDs.
     */
    static getAllActionIds() {
        // Static class fields are not enumerable, so Object.values() won't work.
        // Use getOwnPropertyNames and filter for numeric values.
        return Object.getOwnPropertyNames(Action)
            .map((name) => Action[name])
            .filter((value) => typeof value === "number");
    }
}

export {Action};
