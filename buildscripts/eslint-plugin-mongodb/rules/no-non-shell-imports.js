// Enforces that static imports in src/mongo/shell/ only reference modules that are available in
// the standalone shell binary: files under src/mongo/shell/ (embedded at build time) and std:* (internal C++ modules)

export default {
    meta: {
        type: "problem",
        docs: {
            description:
                "Shell files may only statically import bundled modules (src/mongo/shell/ or std:)",
        },
    },

    create(context) {
        function checkSource(node) {
            if (!node.source) {
                return;
            }
            const source = node.source.value;
            if (!source.startsWith("src/mongo/shell/") && !source.startsWith("std:")) {
                context.report({
                    node,
                    message: `'${source}' is not available in the standalone shell binary. Static imports in src/mongo/shell/ must come from 'src/mongo/shell/' or 'std:' modules.`,
                });
            }
        }

        return {
            ImportDeclaration: checkSource,
            ExportNamedDeclaration: checkSource,
            ExportAllDeclaration: checkSource,
        };
    },
};
