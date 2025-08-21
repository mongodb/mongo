const print_fns = [
    "jsTestLog",
    "jsTest.log",
    "jsTest.log.info",
    "jsTest.log.debug",
    "jsTest.log.warning",
    "jsTest.log.error",
    "print",
];

function flattenMemberExpressionName(expr) {
    if (expr.object.type == "MemberExpression") {
        return flattenMemberExpressionName(expr.object) + "." + expr.property.name;
    } else if (expr.object.type == "Identifier") {
        return expr.object.name + "." + expr.property.name;
    } else {
        return "";
    }
}

export default {
    meta: {
        type: "problem",
        docs: {
            description: "Ensure no calls like print(tojson(x))",
        },
        fixable: "code",
    },

    create(context) {
        return {
            CallExpression: function (node) {
                if (node.callee.type == "MemberExpression") {
                    node.callee.name = flattenMemberExpressionName(node.callee);
                } else if (node.callee.type != "Identifier") return;

                if (print_fns.every((name) => name != node.callee.name)) return;

                node.arguments.forEach((arg) => {
                    if (arg.type != "CallExpression") return;

                    if (arg.callee.type != "Identifier") return;

                    if (arg.callee.name != "tojson" && arg.callee.name != "tojsononeline") return;

                    context.report({
                        node,
                        message: `Calling ${arg.callee.name}() as a parameter of ${
                            node.callee.name
                        }(). Consider using toJsonForLog() instead or disable this rule by adding '// eslint-disable-next-line mongodb/no-printing-tojson'`,
                        fix(fixer) {
                            return fixer.replaceTextRange([arg.callee.start, arg.callee.end], "toJsonForLog");
                        },
                    });
                });
            },
        };
    },
};
