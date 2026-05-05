const printFunctions = [
    "jsTestLog",
    "jsTest.log",
    "jsTest.log.info",
    "jsTest.log.debug",
    "jsTest.log.warning",
    "jsTest.log.error",
    "print",
];

function getPropertyName(expr) {
    if (!expr.computed && expr.property.type == "Identifier") {
        return expr.property.name;
    }

    if (expr.computed && expr.property.type == "Literal" && typeof expr.property.value == "string") {
        return expr.property.value;
    }

    return "";
}

function flattenCallTargetName(expr) {
    if (expr.type == "ChainExpression") {
        return flattenCallTargetName(expr.expression);
    }

    if (expr.type == "Identifier") {
        return expr.name;
    }

    if (expr.type != "MemberExpression") {
        return "";
    }

    const objectName = flattenCallTargetName(expr.object);
    const propertyName = getPropertyName(expr);

    if (!objectName || !propertyName) {
        return "";
    }

    return `${objectName}.${propertyName}`;
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
                const calleeName = flattenCallTargetName(node.callee);

                if (printFunctions.every((name) => name != calleeName)) return;

                node.arguments.forEach((arg) => {
                    if (arg.type != "CallExpression") return;

                    if (arg.callee.type != "Identifier") return;

                    if (arg.callee.name != "tojson" && arg.callee.name != "tojsononeline") return;

                    context.report({
                        node,
                        message: `Calling ${arg.callee.name}() as a parameter of ${
                            calleeName
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
