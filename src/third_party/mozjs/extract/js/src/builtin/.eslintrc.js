/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

module.exports = {
  plugins: ["spidermonkey-js"],

  overrides: [
    {
      files: ["*.js"],
      excludedFiles: ".eslintrc.js",
      processor: "spidermonkey-js/processor",
      env: {
        // Disable all built-in environments.
        node: false,
        browser: false,
        builtin: false,

        // We need to explicitly disable the default environments added from
        // "tools/lint/eslint/eslint-plugin-mozilla/lib/configs/recommended.js".
        es2021: false,
        "mozilla/privileged": false,
        "mozilla/specific": false,

        // Enable SpiderMonkey's self-hosted environment.
        "spidermonkey-js/environment": true,
      },

      parserOptions: {
        ecmaVersion: "latest",
        sourceType: "script",

        // Self-hosted code defaults to strict mode.
        ecmaFeatures: {
          impliedStrict: true,
        },

        // Strict mode has to be enabled separately for the Babel parser.
        babelOptions: {
          parserOpts: {
            strictMode: true,
          },
        },
      },

      rules: {
        // We should fix those at some point, but we use this to detect NaNs.
        "no-self-compare": "off",
        "no-lonely-if": "off",
        // Disabled until we can use let/const to fix those erorrs, and undefined
        // names cause an exception and abort during runtime initialization.
        "no-redeclare": "off",
        // Disallow use of |void 0|. Instead use |undefined|.
        "no-void": ["error", { allowAsStatement: true }],
        // Disallow loose equality because of objects with the [[IsHTMLDDA]]
        // internal slot, aka |document.all|, aka "objects emulating undefined".
        eqeqeq: "error",
        // All self-hosted code is implicitly strict mode, so there's no need to
        // add a strict-mode directive.
        strict: ["error", "never"],
        // Disallow syntax not supported in self-hosted code.
        "no-restricted-syntax": [
          "error",
          {
            selector: "ClassDeclaration",
            message: "Class declarations are not allowed",
          },
          {
            selector: "ClassExpression",
            message: "Class expressions are not allowed",
          },
          {
            selector: "Literal[regex]",
            message: "Regular expression literals are not allowed",
          },
          {
            selector: "CallExpression > MemberExpression.callee",
            message:
              "Direct method calls are not allowed, use callFunction() or callContentFunction()",
          },
          {
            selector: "NewExpression > MemberExpression.callee",
            message:
              "Direct method calls are not allowed, use constructContentFunction()",
          },
          {
            selector: "YieldExpression[delegate=true]",
            message:
              "yield* is not allowed because it can run user-modifiable iteration code",
          },
          {
            selector: "ForOfStatement > :not(CallExpression).right",
            message:
              "for-of loops must use allowContentIter(), allowContentIterWith(), or allowContentIterWithNext()",
          },
          {
            selector:
              "ForOfStatement > CallExpression.right > :not(Identifier[name='allowContentIter'], Identifier[name='allowContentIterWith'], Identifier[name='allowContentIterWithNext']).callee",
            message:
              "for-of loops must use allowContentIter(), allowContentIterWith(), or allowContentIterWithNext",
          },
          {
            selector:
              "CallExpression[callee.name='TO_PROPERTY_KEY'] > :not(Identifier).arguments:first-child",
            message:
              "TO_PROPERTY_KEY macro must be called with a simple identifier",
          },
          {
            selector: "Identifier[name='arguments']",
            message:
              "'arguments' is disallowed, use ArgumentsLength(), GetArgument(n), or rest-parameters",
          },
          {
            selector: "VariableDeclaration[kind='let']",
            message: "'let' declarations are disallowed to avoid TDZ checks, use 'var' instead",
          },
          {
            selector: "VariableDeclaration[kind='const']",
            message: "'const' declarations are disallowed to avoid TDZ checks, use 'var' instead",
          },
        ],
        // Method signatures are important in builtins so disable unused argument errors.
        "no-unused-vars": [
          "error",
          {
            args: "none",
            vars: "local",
          },
        ],
      },

      globals: {
        // The bytecode compiler special-cases these identifiers.
        ArgumentsLength: "readonly",
        allowContentIter: "readonly",
        allowContentIterWith: "readonly",
        allowContentIterWithNext: "readonly",
        callContentFunction: "readonly",
        callFunction: "readonly",
        constructContentFunction: "readonly",
        DefineDataProperty: "readonly",
        forceInterpreter: "readonly",
        GetArgument: "readonly",
        GetBuiltinConstructor: "readonly",
        GetBuiltinPrototype: "readonly",
        GetBuiltinSymbol: "readonly",
        getPropertySuper: "readonly",
        hasOwn: "readonly",
        IsNullOrUndefined: "readonly",
        IteratorClose: "readonly",
        resumeGenerator: "readonly",
        SetCanonicalName: "readonly",
        SetIsInlinableLargeFunction: "readonly",
        ToNumeric: "readonly",
        ToString: "readonly",

        // We've disabled all built-in environments, which also removed
        // `undefined` from the list of globals. Put it back because it's
        // actually allowed in self-hosted code.
        undefined: "readonly",

        // Disable globals from stage 2/3 proposals for which we have work in
        // progress patches. Eventually these will be part of a future ES
        // release, in which case we can remove these extra entries.
        AsyncIterator: "off",
        Iterator: "off",
        Record: "off",
        Temporal: "off",
        Tuple: "off",
      },
    },
  ],
};
