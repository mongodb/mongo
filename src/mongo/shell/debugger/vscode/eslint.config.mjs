// This folder contains JS intended to run in a Node-based environment, not the Mongo shell.

import globals from "globals";

export default {
    languageOptions: {
        ecmaVersion: 2022,
        sourceType: "module",
        globals: {
            ...globals.node,
            ...globals.es2021,
        },
    },
};
