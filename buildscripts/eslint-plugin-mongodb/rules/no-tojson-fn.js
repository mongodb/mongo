const stopList = [
    "tojson",
    "tojsononeline",
];

export default {

    meta: {
        type: "problem",
        docs: {
            description: "Ensure no direct calls to tojson* functions",
        },
        fixable: "code",
    },

    create(context) {
        return {
            CallExpression: function(node) {
                if (node.callee.type == "Identifier" &&
                    stopList.some(fn => fn == node.callee.name)) {
                    context.report(
                        {
                            node,
                            message: `Direct use of '${
                                node.callee
                                    .name}()'. Consider using jsTest.log.info() instead or disable mongodb/no-tojson-fn rule when necessary, e.g., '// eslint-disable-next-line mongodb/no-print-fn'

More about rules configuration: https://eslint.org/docs/latest/use/configure/rules`,
                        });
                }
            }
        };
    }
};
