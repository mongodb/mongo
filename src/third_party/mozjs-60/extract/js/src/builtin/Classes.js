// Give a builtin constructor that we can use as the default. When we give
// it to our newly made class, we will be sure to set it up with the correct name
// and .prototype, so that everything works properly.

var DefaultDerivedClassConstructor =
    class extends null {
        constructor(...args) {
            super(...allowContentIter(args));
        }
    };
MakeDefaultConstructor(DefaultDerivedClassConstructor);

var DefaultBaseClassConstructor =
    class {
        constructor() { }
    };
MakeDefaultConstructor(DefaultBaseClassConstructor);
