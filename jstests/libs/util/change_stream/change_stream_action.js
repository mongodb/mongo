/**
 * Enum-like class for actions that can be performed on database/collection states.
 */
class Action {
    static INSERT_DOC = 0;
    static CREATE_DATABASE = 1;
    static CREATE_SHARDED_COLLECTION = 2;
    static CREATE_UNSPLITTABLE_COLLECTION = 3;
    static CREATE_UNTRACKED_COLLECTION = 4;
    static DROP_COLLECTION = 5;
    static DROP_DATABASE = 6;
    static RENAME_TO_NON_EXISTENT_SAME_DB = 7;
    static RENAME_TO_EXISTENT_SAME_DB = 8;
    static RENAME_TO_NON_EXISTENT_DIFFERENT_DB = 9;
    static RENAME_TO_EXISTENT_DIFFERENT_DB = 10;
    static SHARD_COLLECTION = 11;
    static UNSHARD_COLLECTION = 12;
    static RESHARD_COLLECTION = 13;
    static MOVE_PRIMARY = 14;
    static MOVE_COLLECTION = 15;
    static MOVE_CHUNK = 16;

    static getName(actionId) {
        switch (actionId) {
            case Action.INSERT_DOC:
                return "insert doc";
            case Action.CREATE_DATABASE:
                return "create database";
            case Action.CREATE_SHARDED_COLLECTION:
                return "create sharded collection";
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
            case Action.SHARD_COLLECTION:
                return "shard collection";
            case Action.UNSHARD_COLLECTION:
                return "unshard collection";
            case Action.RESHARD_COLLECTION:
                return "reshard collection";
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
}

export {Action};
