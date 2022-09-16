var fc = (function() {
    'use strict';

    var commonjsGlobal = typeof globalThis !== 'undefined' ? globalThis
                                                           : typeof window !== 'undefined'
            ? window
            : typeof global !== 'undefined' ? global : typeof self !== 'undefined' ? self : {};

    function getDefaultExportFromCjs(x) {
        return x && x.__esModule && Object.prototype.hasOwnProperty.call(x, 'default')
            ? x['default']
            : x;
    }

    var fastCheckDefault$1 = {};

    var Pre = {};

    var PreconditionFailure$1 = {};

    Object.defineProperty(PreconditionFailure$1, "__esModule", {value: true});
    PreconditionFailure$1.PreconditionFailure = void 0;
    class PreconditionFailure extends Error {
        constructor(interruptExecution = false) {
            super();
            this.interruptExecution = interruptExecution;
            this.footprint = PreconditionFailure.SharedFootPrint;
        }
        static isFailure(err) {
            return err != null && err.footprint === PreconditionFailure.SharedFootPrint;
        }
    }
    PreconditionFailure$1.PreconditionFailure = PreconditionFailure;
    PreconditionFailure.SharedFootPrint = Symbol('fast-check/PreconditionFailure');

    Object.defineProperty(Pre, "__esModule", {value: true});
    Pre.pre = void 0;
    const PreconditionFailure_1$5 = PreconditionFailure$1;
    function pre(expectTruthy) {
        if (!expectTruthy) {
            throw new PreconditionFailure_1$5.PreconditionFailure();
        }
    }
    Pre.pre = pre;

    var AsyncProperty$1 = {};

    var Arbitrary$1 = {};

    var Stream$1 = {};

    var StreamHelpers = {};

    Object.defineProperty(StreamHelpers, "__esModule", {value: true});
    StreamHelpers.joinHelper = StreamHelpers.takeWhileHelper = StreamHelpers.takeNHelper =
        StreamHelpers.filterHelper = StreamHelpers.flatMapHelper = StreamHelpers.mapHelper =
            StreamHelpers.nilHelper = void 0;
    class Nil {
        [Symbol.iterator]() {
            return this;
        }
        next(value) {
            return {value, done: true};
        }
    }
    Nil.nil = new Nil();
    function nilHelper() {
        return Nil.nil;
    }
    StreamHelpers.nilHelper = nilHelper;
    function* mapHelper(g, f) {
        for (const v of g) {
            yield f(v);
        }
    }
    StreamHelpers.mapHelper = mapHelper;
    function* flatMapHelper(g, f) {
        for (const v of g) {
            yield* f(v);
        }
    }
    StreamHelpers.flatMapHelper = flatMapHelper;
    function* filterHelper(g, f) {
        for (const v of g) {
            if (f(v)) {
                yield v;
            }
        }
    }
    StreamHelpers.filterHelper = filterHelper;
    function* takeNHelper(g, n) {
        for (let i = 0; i < n; ++i) {
            const cur = g.next();
            if (cur.done) {
                break;
            }
            yield cur.value;
        }
    }
    StreamHelpers.takeNHelper = takeNHelper;
    function* takeWhileHelper(g, f) {
        let cur = g.next();
        while (!cur.done && f(cur.value)) {
            yield cur.value;
            cur = g.next();
        }
    }
    StreamHelpers.takeWhileHelper = takeWhileHelper;
    function* joinHelper(g, others) {
        for (let cur = g.next(); !cur.done; cur = g.next()) {
            yield cur.value;
        }
        for (const s of others) {
            for (let cur = s.next(); !cur.done; cur = s.next()) {
                yield cur.value;
            }
        }
    }
    StreamHelpers.joinHelper = joinHelper;

    Object.defineProperty(Stream$1, "__esModule", {value: true});
    Stream$1.stream = Stream$1.Stream = void 0;
    const StreamHelpers_1 = StreamHelpers;
    class Stream {
        constructor(g) {
            this.g = g;
        }
        static nil() {
            return new Stream((0, StreamHelpers_1.nilHelper)());
        }
        static of(...elements) {
            return new Stream(elements[Symbol.iterator]());
        }
        next() {
            return this.g.next();
        }
        [Symbol.iterator]() {
            return this.g;
        }
        map(f) {
            return new Stream((0, StreamHelpers_1.mapHelper)(this.g, f));
        }
        flatMap(f) {
            return new Stream((0, StreamHelpers_1.flatMapHelper)(this.g, f));
        }
        dropWhile(f) {
            let foundEligible = false;
            function* helper(v) {
                if (foundEligible || !f(v)) {
                    foundEligible = true;
                    yield v;
                }
            }
            return this.flatMap(helper);
        }
        drop(n) {
            let idx = 0;
            function helper() {
                return idx++ < n;
            }
            return this.dropWhile(helper);
        }
        takeWhile(f) {
            return new Stream((0, StreamHelpers_1.takeWhileHelper)(this.g, f));
        }
        take(n) {
            return new Stream((0, StreamHelpers_1.takeNHelper)(this.g, n));
        }
        filter(f) {
            return new Stream((0, StreamHelpers_1.filterHelper)(this.g, f));
        }
        every(f) {
            for (const v of this.g) {
                if (!f(v)) {
                    return false;
                }
            }
            return true;
        }
        has(f) {
            for (const v of this.g) {
                if (f(v)) {
                    return [true, v];
                }
            }
            return [false, null];
        }
        join(...others) {
            return new Stream((0, StreamHelpers_1.joinHelper)(this.g, others));
        }
        getNthOrLast(nth) {
            let remaining = nth;
            let last = null;
            for (const v of this.g) {
                if (remaining-- === 0)
                    return v;
                last = v;
            }
            return last;
        }
    }
    Stream$1.Stream = Stream;
    function stream(g) {
        return new Stream(g);
    }
    Stream$1.stream = stream;

    var symbols = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.cloneIfNeeded = exports.hasCloneMethod = exports.cloneMethod = void 0;
        exports.cloneMethod = Symbol('fast-check/cloneMethod');
        function hasCloneMethod(instance) {
            return (instance !== null &&
                    (typeof instance === 'object' || typeof instance === 'function') &&
                    exports.cloneMethod in instance &&
                    typeof instance[exports.cloneMethod] === 'function');
        }
        exports.hasCloneMethod = hasCloneMethod;
        function cloneIfNeeded(instance) {
            return hasCloneMethod(instance) ? instance[exports.cloneMethod]() : instance;
        }
        exports.cloneIfNeeded = cloneIfNeeded;
    }(symbols));

    var Value$1 = {};

    Object.defineProperty(Value$1, "__esModule", {value: true});
    Value$1.Value = void 0;
    const symbols_1$c = symbols;
    class Value {
        constructor(value_, context, customGetValue = undefined) {
            this.value_ = value_;
            this.context = context;
            this.hasToBeCloned =
                customGetValue !== undefined || (0, symbols_1$c.hasCloneMethod)(value_);
            this.readOnce = false;
            if (this.hasToBeCloned) {
                Object.defineProperty(this, 'value', {
                    get: customGetValue !== undefined ? customGetValue : this.getValue
                });
            } else {
                this.value = value_;
            }
        }
        getValue() {
            if (this.hasToBeCloned) {
                if (!this.readOnce) {
                    this.readOnce = true;
                    return this.value_;
                }
                return this.value_[symbols_1$c.cloneMethod]();
            }
            return this.value_;
        }
    }
    Value$1.Value = Value;

    Object.defineProperty(Arbitrary$1, "__esModule", {value: true});
    Arbitrary$1.assertIsArbitrary = Arbitrary$1.isArbitrary = Arbitrary$1.Arbitrary = void 0;
    const Stream_1$l = Stream$1;
    const symbols_1$b = symbols;
    const Value_1$j = Value$1;
    class Arbitrary {
        filter(refinement) {
            return new FilterArbitrary(this, refinement);
        }
        map(mapper, unmapper) {
            return new MapArbitrary(this, mapper, unmapper);
        }
        chain(chainer) {
            return new ChainArbitrary(this, chainer);
        }
        noShrink() {
            return new NoShrinkArbitrary(this);
        }
        noBias() {
            return new NoBiasArbitrary(this);
        }
    }
    Arbitrary$1.Arbitrary = Arbitrary;
    class ChainArbitrary extends Arbitrary {
        constructor(arb, chainer) {
            super();
            this.arb = arb;
            this.chainer = chainer;
        }
        generate(mrng, biasFactor) {
            const clonedMrng = mrng.clone();
            const src = this.arb.generate(mrng, biasFactor);
            return this.valueChainer(src, mrng, clonedMrng, biasFactor);
        }
        canShrinkWithoutContext(value) {
            return false;
        }
        shrink(value, context) {
            if (this.isSafeContext(context)) {
                return (!context.stoppedForOriginal
                            ? this.arb.shrink(context.originalValue, context.originalContext)
                                  .map((v) => this.valueChainer(v,
                                                                context.clonedMrng.clone(),
                                                                context.clonedMrng,
                                                                context.originalBias))
                            : Stream_1$l.Stream.nil())
                    .join(context.chainedArbitrary.shrink(value, context.chainedContext)
                              .map((dst) => {
                                  const newContext = Object.assign(
                                      Object.assign({}, context),
                                      {chainedContext: dst.context, stoppedForOriginal: true});
                                  return new Value_1$j.Value(dst.value_, newContext);
                              }));
            }
            return Stream_1$l.Stream.nil();
        }
        valueChainer(v, generateMrng, clonedMrng, biasFactor) {
            const chainedArbitrary = this.chainer(v.value_);
            const dst = chainedArbitrary.generate(generateMrng, biasFactor);
            const context = {
                originalBias: biasFactor,
                originalValue: v.value_,
                originalContext: v.context,
                stoppedForOriginal: false,
                chainedArbitrary,
                chainedContext: dst.context,
                clonedMrng,
            };
            return new Value_1$j.Value(dst.value_, context);
        }
        isSafeContext(context) {
            return (context != null && typeof context === 'object' && 'originalBias' in context &&
                    'originalValue' in context && 'originalContext' in context &&
                    'stoppedForOriginal' in context && 'chainedArbitrary' in context &&
                    'chainedContext' in context && 'clonedMrng' in context);
        }
    }
    class MapArbitrary extends Arbitrary {
        constructor(arb, mapper, unmapper) {
            super();
            this.arb = arb;
            this.mapper = mapper;
            this.unmapper = unmapper;
            this.bindValueMapper = this.valueMapper.bind(this);
        }
        generate(mrng, biasFactor) {
            const g = this.arb.generate(mrng, biasFactor);
            return this.valueMapper(g);
        }
        canShrinkWithoutContext(value) {
            if (this.unmapper !== undefined) {
                try {
                    const unmapped = this.unmapper(value);
                    return this.arb.canShrinkWithoutContext(unmapped);
                } catch (_err) {
                    return false;
                }
            }
            return false;
        }
        shrink(value, context) {
            if (this.isSafeContext(context)) {
                return this.arb.shrink(context.originalValue, context.originalContext)
                    .map(this.bindValueMapper);
            }
            if (this.unmapper !== undefined) {
                const unmapped = this.unmapper(value);
                return this.arb.shrink(unmapped, undefined).map(this.bindValueMapper);
            }
            return Stream_1$l.Stream.nil();
        }
        mapperWithCloneIfNeeded(v) {
            const sourceValue = v.value;
            const mappedValue = this.mapper(sourceValue);
            if (v.hasToBeCloned &&
                ((typeof mappedValue === 'object' && mappedValue !== null) ||
                 typeof mappedValue === 'function') &&
                Object.isExtensible(mappedValue) && !(0, symbols_1$b.hasCloneMethod)(mappedValue)) {
                Object.defineProperty(mappedValue,
                                      symbols_1$b.cloneMethod,
                                      {get: () => () => this.mapperWithCloneIfNeeded(v)[0]});
            }
            return [mappedValue, sourceValue];
        }
        valueMapper(v) {
            const [mappedValue, sourceValue] = this.mapperWithCloneIfNeeded(v);
            const context = {originalValue: sourceValue, originalContext: v.context};
            return new Value_1$j.Value(mappedValue, context);
        }
        isSafeContext(context) {
            return (context != null && typeof context === 'object' && 'originalValue' in context &&
                    'originalContext' in context);
        }
    }
    class FilterArbitrary extends Arbitrary {
        constructor(arb, refinement) {
            super();
            this.arb = arb;
            this.refinement = refinement;
            this.bindRefinementOnValue = this.refinementOnValue.bind(this);
        }
        generate(mrng, biasFactor) {
            while (true) {
                const g = this.arb.generate(mrng, biasFactor);
                if (this.refinementOnValue(g)) {
                    return g;
                }
            }
        }
        canShrinkWithoutContext(value) {
            return this.arb.canShrinkWithoutContext(value) && this.refinement(value);
        }
        shrink(value, context) {
            return this.arb.shrink(value, context).filter(this.bindRefinementOnValue);
        }
        refinementOnValue(v) {
            return this.refinement(v.value);
        }
    }
    class NoShrinkArbitrary extends Arbitrary {
        constructor(arb) {
            super();
            this.arb = arb;
        }
        generate(mrng, biasFactor) {
            return this.arb.generate(mrng, biasFactor);
        }
        canShrinkWithoutContext(value) {
            return this.arb.canShrinkWithoutContext(value);
        }
        shrink(_value, _context) {
            return Stream_1$l.Stream.nil();
        }
        noShrink() {
            return this;
        }
    }
    class NoBiasArbitrary extends Arbitrary {
        constructor(arb) {
            super();
            this.arb = arb;
        }
        generate(mrng, _biasFactor) {
            return this.arb.generate(mrng, undefined);
        }
        canShrinkWithoutContext(value) {
            return this.arb.canShrinkWithoutContext(value);
        }
        shrink(value, context) {
            return this.arb.shrink(value, context);
        }
        noBias() {
            return this;
        }
    }
    function isArbitrary(instance) {
        return (typeof instance === 'object' && instance !== null && 'generate' in instance &&
                'shrink' in instance && 'canShrinkWithoutContext' in instance);
    }
    Arbitrary$1.isArbitrary = isArbitrary;
    function assertIsArbitrary(instance) {
        if (!isArbitrary(instance)) {
            throw new Error('Unexpected value received: not an instance of Arbitrary');
        }
    }
    Arbitrary$1.assertIsArbitrary = assertIsArbitrary;

    var tuple$1 = {};

    var TupleArbitrary$1 = {};

    Object.defineProperty(TupleArbitrary$1, "__esModule", {value: true});
    TupleArbitrary$1.TupleArbitrary = void 0;
    const Stream_1$k = Stream$1;
    const symbols_1$a = symbols;
    const Arbitrary_1$j = Arbitrary$1;
    const Value_1$i = Value$1;
    class TupleArbitrary extends Arbitrary_1$j.Arbitrary {
        constructor(arbs) {
            super();
            this.arbs = arbs;
            for (let idx = 0; idx !== arbs.length; ++idx) {
                const arb = arbs[idx];
                if (arb == null || arb.generate == null)
                    throw new Error(
                        `Invalid parameter encountered at index ${idx}: expecting an Arbitrary`);
            }
        }
        static makeItCloneable(vs, values) {
            return Object.defineProperty(vs, symbols_1$a.cloneMethod, {
                value: () => {
                    const cloned = [];
                    for (let idx = 0; idx !== values.length; ++idx) {
                        cloned.push(values[idx].value);
                    }
                    TupleArbitrary.makeItCloneable(cloned, values);
                    return cloned;
                },
            });
        }
        static wrapper(values) {
            let cloneable = false;
            const vs = [];
            const ctxs = [];
            for (let idx = 0; idx !== values.length; ++idx) {
                const v = values[idx];
                cloneable = cloneable || v.hasToBeCloned;
                vs.push(v.value);
                ctxs.push(v.context);
            }
            if (cloneable) {
                TupleArbitrary.makeItCloneable(vs, values);
            }
            return new Value_1$i.Value(vs, ctxs);
        }
        generate(mrng, biasFactor) {
            return TupleArbitrary.wrapper(this.arbs.map((a) => a.generate(mrng, biasFactor)));
        }
        canShrinkWithoutContext(value) {
            if (!Array.isArray(value) || value.length !== this.arbs.length) {
                return false;
            }
            for (let index = 0; index !== this.arbs.length; ++index) {
                if (!this.arbs[index].canShrinkWithoutContext(value[index])) {
                    return false;
                }
            }
            return true;
        }
        shrink(value, context) {
            let s = Stream_1$k.Stream.nil();
            const safeContext = Array.isArray(context) ? context : [];
            for (let idx = 0; idx !== this.arbs.length; ++idx) {
                const shrinksForIndex =
                    this.arbs[idx]
                        .shrink(value[idx], safeContext[idx])
                        .map((v) => {
                            const nextValues =
                                value.map((v, idx) => new Value_1$i.Value(
                                              (0, symbols_1$a.cloneIfNeeded)(v), safeContext[idx]));
                            return nextValues.slice(0, idx).concat([v]).concat(
                                nextValues.slice(idx + 1));
                        })
                        .map((values) => TupleArbitrary.wrapper(values));
                s = s.join(shrinksForIndex);
            }
            return s;
        }
    }
    TupleArbitrary$1.TupleArbitrary = TupleArbitrary;

    Object.defineProperty(tuple$1, "__esModule", {value: true});
    tuple$1.tuple = void 0;
    const TupleArbitrary_1 = TupleArbitrary$1;
    function tuple(...arbs) {
        return new TupleArbitrary_1.TupleArbitrary(arbs);
    }
    tuple$1.tuple = tuple;

    var AsyncProperty_generic = {};

    var IRawProperty = {};

    Object.defineProperty(IRawProperty, "__esModule", {value: true});
    IRawProperty.runIdToFrequency = void 0;
    const runIdToFrequency = (runId) => 2 + Math.floor(Math.log(runId + 1) / Math.log(10));
    IRawProperty.runIdToFrequency = runIdToFrequency;

    var GlobalParameters = {};

    Object.defineProperty(GlobalParameters, "__esModule", {value: true});
    GlobalParameters.resetConfigureGlobal = GlobalParameters.readConfigureGlobal =
        GlobalParameters.configureGlobal = void 0;
    let globalParameters = {};
    function configureGlobal(parameters) {
        globalParameters = parameters;
    }
    GlobalParameters.configureGlobal = configureGlobal;
    function readConfigureGlobal() {
        return globalParameters;
    }
    GlobalParameters.readConfigureGlobal = readConfigureGlobal;
    function resetConfigureGlobal() {
        globalParameters = {};
    }
    GlobalParameters.resetConfigureGlobal = resetConfigureGlobal;

    var NoUndefinedAsContext = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.noUndefinedAsContext = exports.UndefinedContextPlaceholder = void 0;
        const Value_1 = Value$1;
        exports.UndefinedContextPlaceholder = Symbol('UndefinedContextPlaceholder');
        function noUndefinedAsContext(value) {
            if (value.context !== undefined) {
                return value;
            }
            if (value.hasToBeCloned) {
                return new Value_1.Value(
                    value.value_, exports.UndefinedContextPlaceholder, () => value.value);
            }
            return new Value_1.Value(value.value_, exports.UndefinedContextPlaceholder);
        }
        exports.noUndefinedAsContext = noUndefinedAsContext;
    }(NoUndefinedAsContext));

    Object.defineProperty(AsyncProperty_generic, "__esModule", {value: true});
    AsyncProperty_generic.AsyncProperty = void 0;
    const PreconditionFailure_1$4 = PreconditionFailure$1;
    const IRawProperty_1$1 = IRawProperty;
    const GlobalParameters_1$3 = GlobalParameters;
    const Stream_1$j = Stream$1;
    const NoUndefinedAsContext_1$2 = NoUndefinedAsContext;
    class AsyncProperty {
        constructor(arb, predicate) {
            this.arb = arb;
            this.predicate = predicate;
            const {asyncBeforeEach, asyncAfterEach, beforeEach, afterEach} =
                (0, GlobalParameters_1$3.readConfigureGlobal)() || {};
            if (asyncBeforeEach !== undefined && beforeEach !== undefined) {
                throw Error(
                    'Global "asyncBeforeEach" and "beforeEach" parameters can\'t be set at the same time when running async properties');
            }
            if (asyncAfterEach !== undefined && afterEach !== undefined) {
                throw Error(
                    'Global "asyncAfterEach" and "afterEach" parameters can\'t be set at the same time when running async properties');
            }
            this.beforeEachHook = asyncBeforeEach || beforeEach || AsyncProperty.dummyHook;
            this.afterEachHook = asyncAfterEach || afterEach || AsyncProperty.dummyHook;
        }
        isAsync() {
            return true;
        }
        generate(mrng, runId) {
            const value = this.arb.generate(
                mrng, runId != null ? (0, IRawProperty_1$1.runIdToFrequency)(runId) : undefined);
            return (0, NoUndefinedAsContext_1$2.noUndefinedAsContext)(value);
        }
        shrink(value) {
            if (value.context === undefined && !this.arb.canShrinkWithoutContext(value.value_)) {
                return Stream_1$j.Stream.nil();
            }
            const safeContext =
                value.context !== NoUndefinedAsContext_1$2.UndefinedContextPlaceholder
                ? value.context
                : undefined;
            return this.arb.shrink(value.value_, safeContext)
                .map(NoUndefinedAsContext_1$2.noUndefinedAsContext);
        }
        async run(v) {
            await this.beforeEachHook();
            try {
                const output = await this.predicate(v);
                return output == null || output === true
                    ? null
                    : {error: undefined, errorMessage: 'Property failed by returning false'};
            } catch (err) {
                if (PreconditionFailure_1$4.PreconditionFailure.isFailure(err))
                    return err;
                if (err instanceof Error && err.stack) {
                    return {error: err, errorMessage: `${err}\n\nStack trace: ${err.stack}`};
                }
                return {error: err, errorMessage: String(err)};
            } finally {
                await this.afterEachHook();
            }
        }
        beforeEach(hookFunction) {
            const previousBeforeEachHook = this.beforeEachHook;
            this.beforeEachHook = () => hookFunction(previousBeforeEachHook);
            return this;
        }
        afterEach(hookFunction) {
            const previousAfterEachHook = this.afterEachHook;
            this.afterEachHook = () => hookFunction(previousAfterEachHook);
            return this;
        }
    }
    AsyncProperty_generic.AsyncProperty = AsyncProperty;
    AsyncProperty.dummyHook = () => {};

    var AlwaysShrinkableArbitrary$1 = {};

    Object.defineProperty(AlwaysShrinkableArbitrary$1, "__esModule", {value: true});
    AlwaysShrinkableArbitrary$1.AlwaysShrinkableArbitrary = void 0;
    const Arbitrary_1$i = Arbitrary$1;
    const Stream_1$i = Stream$1;
    const NoUndefinedAsContext_1$1 = NoUndefinedAsContext;
    class AlwaysShrinkableArbitrary extends Arbitrary_1$i.Arbitrary {
        constructor(arb) {
            super();
            this.arb = arb;
        }
        generate(mrng, biasFactor) {
            const value = this.arb.generate(mrng, biasFactor);
            return (0, NoUndefinedAsContext_1$1.noUndefinedAsContext)(value);
        }
        canShrinkWithoutContext(value) {
            return true;
        }
        shrink(value, context) {
            if (context === undefined && !this.arb.canShrinkWithoutContext(value)) {
                return Stream_1$i.Stream.nil();
            }
            const safeContext = context !== NoUndefinedAsContext_1$1.UndefinedContextPlaceholder
                ? context
                : undefined;
            return this.arb.shrink(value, safeContext)
                .map(NoUndefinedAsContext_1$1.noUndefinedAsContext);
        }
    }
    AlwaysShrinkableArbitrary$1.AlwaysShrinkableArbitrary = AlwaysShrinkableArbitrary;

    Object.defineProperty(AsyncProperty$1, "__esModule", {value: true});
    AsyncProperty$1.asyncProperty = void 0;
    const Arbitrary_1$h = Arbitrary$1;
    const tuple_1$h = tuple$1;
    const AsyncProperty_generic_1 = AsyncProperty_generic;
    const AlwaysShrinkableArbitrary_1$1 = AlwaysShrinkableArbitrary$1;
    function asyncProperty(...args) {
        if (args.length < 2) {
            throw new Error('asyncProperty expects at least two parameters');
        }
        const arbs = args.slice(0, args.length - 1);
        const p = args[args.length - 1];
        arbs.forEach(Arbitrary_1$h.assertIsArbitrary);
        const mappedArbs =
            arbs.map((arb) => new AlwaysShrinkableArbitrary_1$1.AlwaysShrinkableArbitrary(arb));
        return new AsyncProperty_generic_1.AsyncProperty((0, tuple_1$h.tuple)(...mappedArbs),
                                                         (t) => p(...t));
    }
    AsyncProperty$1.asyncProperty = asyncProperty;

    var Property$1 = {};

    var Property_generic = {};

    Object.defineProperty(Property_generic, "__esModule", {value: true});
    Property_generic.Property = void 0;
    const PreconditionFailure_1$3 = PreconditionFailure$1;
    const IRawProperty_1 = IRawProperty;
    const GlobalParameters_1$2 = GlobalParameters;
    const Stream_1$h = Stream$1;
    const NoUndefinedAsContext_1 = NoUndefinedAsContext;
    class Property {
        constructor(arb, predicate) {
            this.arb = arb;
            this.predicate = predicate;
            const {
                beforeEach = Property.dummyHook,
                afterEach = Property.dummyHook,
                asyncBeforeEach,
                asyncAfterEach,
            } = (0, GlobalParameters_1$2.readConfigureGlobal)() || {};
            if (asyncBeforeEach !== undefined) {
                throw Error('"asyncBeforeEach" can\'t be set when running synchronous properties');
            }
            if (asyncAfterEach !== undefined) {
                throw Error('"asyncAfterEach" can\'t be set when running synchronous properties');
            }
            this.beforeEachHook = beforeEach;
            this.afterEachHook = afterEach;
        }
        isAsync() {
            return false;
        }
        generate(mrng, runId) {
            const value = this.arb.generate(
                mrng, runId != null ? (0, IRawProperty_1.runIdToFrequency)(runId) : undefined);
            return (0, NoUndefinedAsContext_1.noUndefinedAsContext)(value);
        }
        shrink(value) {
            if (value.context === undefined && !this.arb.canShrinkWithoutContext(value.value_)) {
                return Stream_1$h.Stream.nil();
            }
            const safeContext = value.context !== NoUndefinedAsContext_1.UndefinedContextPlaceholder
                ? value.context
                : undefined;
            return this.arb.shrink(value.value_, safeContext)
                .map(NoUndefinedAsContext_1.noUndefinedAsContext);
        }
        run(v) {
            this.beforeEachHook();
            try {
                const output = this.predicate(v);
                return output == null || output === true
                    ? null
                    : {error: undefined, errorMessage: 'Property failed by returning false'};
            } catch (err) {
                if (PreconditionFailure_1$3.PreconditionFailure.isFailure(err))
                    return err;
                if (err instanceof Error && err.stack) {
                    return {error: err, errorMessage: `${err}\n\nStack trace: ${err.stack}`};
                }
                return {error: err, errorMessage: String(err)};
            } finally {
                this.afterEachHook();
            }
        }
        beforeEach(hookFunction) {
            const previousBeforeEachHook = this.beforeEachHook;
            this.beforeEachHook = () => hookFunction(previousBeforeEachHook);
            return this;
        }
        afterEach(hookFunction) {
            const previousAfterEachHook = this.afterEachHook;
            this.afterEachHook = () => hookFunction(previousAfterEachHook);
            return this;
        }
    }
    Property_generic.Property = Property;
    Property.dummyHook = () => {};

    Object.defineProperty(Property$1, "__esModule", {value: true});
    Property$1.property = void 0;
    const Arbitrary_1$g = Arbitrary$1;
    const tuple_1$g = tuple$1;
    const Property_generic_1$1 = Property_generic;
    const AlwaysShrinkableArbitrary_1 = AlwaysShrinkableArbitrary$1;
    function property(...args) {
        if (args.length < 2) {
            throw new Error('property expects at least two parameters');
        }
        const arbs = args.slice(0, args.length - 1);
        const p = args[args.length - 1];
        arbs.forEach(Arbitrary_1$g.assertIsArbitrary);
        const mappedArbs =
            arbs.map((arb) => new AlwaysShrinkableArbitrary_1.AlwaysShrinkableArbitrary(arb));
        return new Property_generic_1$1.Property((0, tuple_1$g.tuple)(...mappedArbs),
                                                 (t) => p(...t));
    }
    Property$1.property = property;

    var Runner = {};

    var QualifiedParameters$1 = {};

    var pureRand = {};

    var pureRandDefault = {};

    var RandomGenerator = {};

    RandomGenerator.__esModule = true;
    RandomGenerator.skipN = RandomGenerator.unsafeSkipN = RandomGenerator.generateN =
        RandomGenerator.unsafeGenerateN = void 0;
    function unsafeGenerateN(rng, num) {
        var out = [];
        for (var idx = 0; idx != num; ++idx) {
            out.push(rng.unsafeNext());
        }
        return out;
    }
    RandomGenerator.unsafeGenerateN = unsafeGenerateN;
    function generateN(rng, num) {
        var nextRng = rng.clone();
        var out = unsafeGenerateN(nextRng, num);
        return [out, nextRng];
    }
    RandomGenerator.generateN = generateN;
    function unsafeSkipN(rng, num) {
        for (var idx = 0; idx != num; ++idx) {
            rng.unsafeNext();
        }
    }
    RandomGenerator.unsafeSkipN = unsafeSkipN;
    function skipN(rng, num) {
        var nextRng = rng.clone();
        unsafeSkipN(nextRng, num);
        return nextRng;
    }
    RandomGenerator.skipN = skipN;

    var LinearCongruential$1 = {};

    LinearCongruential$1.__esModule = true;
    LinearCongruential$1.congruential32 = LinearCongruential$1.congruential = void 0;
    var MULTIPLIER = 0x000343fd;
    var INCREMENT = 0x00269ec3;
    var MASK = 0xffffffff;
    var MASK_2 = (1 << 31) - 1;
    var computeNextSeed = function(seed) {
        return (seed * MULTIPLIER + INCREMENT) & MASK;
    };
    var computeValueFromNextSeed = function(nextseed) {
        return (nextseed & MASK_2) >> 16;
    };
    var LinearCongruential = (function() {
        function LinearCongruential(seed) {
            this.seed = seed;
        }
        LinearCongruential.prototype.min = function() {
            return LinearCongruential.min;
        };
        LinearCongruential.prototype.max = function() {
            return LinearCongruential.max;
        };
        LinearCongruential.prototype.clone = function() {
            return new LinearCongruential(this.seed);
        };
        LinearCongruential.prototype.next = function() {
            var nextRng = new LinearCongruential(this.seed);
            var out = nextRng.unsafeNext();
            return [out, nextRng];
        };
        LinearCongruential.prototype.unsafeNext = function() {
            this.seed = computeNextSeed(this.seed);
            return computeValueFromNextSeed(this.seed);
        };
        LinearCongruential.min = 0;
        LinearCongruential.max = Math.pow(2, 15) - 1;
        return LinearCongruential;
    }());
    var LinearCongruential32 = (function() {
        function LinearCongruential32(seed) {
            this.seed = seed;
        }
        LinearCongruential32.prototype.min = function() {
            return LinearCongruential32.min;
        };
        LinearCongruential32.prototype.max = function() {
            return LinearCongruential32.max;
        };
        LinearCongruential32.prototype.clone = function() {
            return new LinearCongruential32(this.seed);
        };
        LinearCongruential32.prototype.next = function() {
            var nextRng = new LinearCongruential32(this.seed);
            var out = nextRng.unsafeNext();
            return [out, nextRng];
        };
        LinearCongruential32.prototype.unsafeNext = function() {
            var s1 = computeNextSeed(this.seed);
            var v1 = computeValueFromNextSeed(s1);
            var s2 = computeNextSeed(s1);
            var v2 = computeValueFromNextSeed(s2);
            this.seed = computeNextSeed(s2);
            var v3 = computeValueFromNextSeed(this.seed);
            var vnext = v3 + ((v2 + (v1 << 15)) << 15);
            return ((vnext + 0x80000000) | 0) + 0x80000000;
        };
        LinearCongruential32.min = 0;
        LinearCongruential32.max = 0xffffffff;
        return LinearCongruential32;
    }());
    var congruential = function(seed) {
        return new LinearCongruential(seed);
    };
    LinearCongruential$1.congruential = congruential;
    var congruential32 = function(seed) {
        return new LinearCongruential32(seed);
    };
    LinearCongruential$1.congruential32 = congruential32;

    var MersenneTwister = {};

    (function(exports) {
        exports.__esModule = true;
        var MersenneTwister = (function() {
            function MersenneTwister(states, index) {
                this.states = states;
                this.index = index;
            }
            MersenneTwister.twist = function(prev) {
                var mt = prev.slice();
                for (var idx = 0; idx !== MersenneTwister.N - MersenneTwister.M; ++idx) {
                    var y_1 = (mt[idx] & MersenneTwister.MASK_UPPER) +
                        (mt[idx + 1] & MersenneTwister.MASK_LOWER);
                    mt[idx] = mt[idx + MersenneTwister.M] ^ (y_1 >>> 1) ^
                        (-(y_1 & 1) & MersenneTwister.A);
                }
                for (var idx = MersenneTwister.N - MersenneTwister.M; idx !== MersenneTwister.N - 1;
                     ++idx) {
                    var y_2 = (mt[idx] & MersenneTwister.MASK_UPPER) +
                        (mt[idx + 1] & MersenneTwister.MASK_LOWER);
                    mt[idx] = mt[idx + MersenneTwister.M - MersenneTwister.N] ^ (y_2 >>> 1) ^
                        (-(y_2 & 1) & MersenneTwister.A);
                }
                var y = (mt[MersenneTwister.N - 1] & MersenneTwister.MASK_UPPER) +
                    (mt[0] & MersenneTwister.MASK_LOWER);
                mt[MersenneTwister.N - 1] =
                    mt[MersenneTwister.M - 1] ^ (y >>> 1) ^ (-(y & 1) & MersenneTwister.A);
                return mt;
            };
            MersenneTwister.seeded = function(seed) {
                var out = Array(MersenneTwister.N);
                out[0] = seed;
                for (var idx = 1; idx !== MersenneTwister.N; ++idx) {
                    var xored = out[idx - 1] ^ (out[idx - 1] >>> 30);
                    out[idx] = (Math.imul(MersenneTwister.F, xored) + idx) | 0;
                }
                return out;
            };
            MersenneTwister.from = function(seed) {
                return new MersenneTwister(MersenneTwister.twist(MersenneTwister.seeded(seed)), 0);
            };
            MersenneTwister.prototype.min = function() {
                return MersenneTwister.min;
            };
            MersenneTwister.prototype.max = function() {
                return MersenneTwister.max;
            };
            MersenneTwister.prototype.clone = function() {
                return new MersenneTwister(this.states, this.index);
            };
            MersenneTwister.prototype.next = function() {
                var nextRng = new MersenneTwister(this.states, this.index);
                var out = nextRng.unsafeNext();
                return [out, nextRng];
            };
            MersenneTwister.prototype.unsafeNext = function() {
                var y = this.states[this.index];
                y ^= this.states[this.index] >>> MersenneTwister.U;
                y ^= (y << MersenneTwister.S) & MersenneTwister.B;
                y ^= (y << MersenneTwister.T) & MersenneTwister.C;
                y ^= y >>> MersenneTwister.L;
                if (++this.index >= MersenneTwister.N) {
                    this.states = MersenneTwister.twist(this.states);
                    this.index = 0;
                }
                return y >>> 0;
            };
            MersenneTwister.min = 0;
            MersenneTwister.max = 0xffffffff;
            MersenneTwister.N = 624;
            MersenneTwister.M = 397;
            MersenneTwister.R = 31;
            MersenneTwister.A = 0x9908b0df;
            MersenneTwister.F = 1812433253;
            MersenneTwister.U = 11;
            MersenneTwister.S = 7;
            MersenneTwister.B = 0x9d2c5680;
            MersenneTwister.T = 15;
            MersenneTwister.C = 0xefc60000;
            MersenneTwister.L = 18;
            MersenneTwister.MASK_LOWER = Math.pow(2, MersenneTwister.R) - 1;
            MersenneTwister.MASK_UPPER = Math.pow(2, MersenneTwister.R);
            return MersenneTwister;
        }());
        function default_1(seed) {
            return MersenneTwister.from(seed);
        }
        exports["default"] = default_1;
    }(MersenneTwister));

    var XorShift = {};

    XorShift.__esModule = true;
    XorShift.xorshift128plus = void 0;
    var XorShift128Plus = (function() {
        function XorShift128Plus(s01, s00, s11, s10) {
            this.s01 = s01;
            this.s00 = s00;
            this.s11 = s11;
            this.s10 = s10;
        }
        XorShift128Plus.prototype.min = function() {
            return -0x80000000;
        };
        XorShift128Plus.prototype.max = function() {
            return 0x7fffffff;
        };
        XorShift128Plus.prototype.clone = function() {
            return new XorShift128Plus(this.s01, this.s00, this.s11, this.s10);
        };
        XorShift128Plus.prototype.next = function() {
            var nextRng = new XorShift128Plus(this.s01, this.s00, this.s11, this.s10);
            var out = nextRng.unsafeNext();
            return [out, nextRng];
        };
        XorShift128Plus.prototype.unsafeNext = function() {
            var a0 = this.s00 ^ (this.s00 << 23);
            var a1 = this.s01 ^ ((this.s01 << 23) | (this.s00 >>> 9));
            var b0 =
                a0 ^ this.s10 ^ ((a0 >>> 18) | (a1 << 14)) ^ ((this.s10 >>> 5) | (this.s11 << 27));
            var b1 = a1 ^ this.s11 ^ (a1 >>> 18) ^ (this.s11 >>> 5);
            var out = (this.s00 + this.s10) | 0;
            this.s01 = this.s11;
            this.s00 = this.s10;
            this.s11 = b1;
            this.s10 = b0;
            return out;
        };
        XorShift128Plus.prototype.jump = function() {
            var nextRng = new XorShift128Plus(this.s01, this.s00, this.s11, this.s10);
            nextRng.unsafeJump();
            return nextRng;
        };
        XorShift128Plus.prototype.unsafeJump = function() {
            var ns01 = 0;
            var ns00 = 0;
            var ns11 = 0;
            var ns10 = 0;
            var jump = [0x635d2dff, 0x8a5cd789, 0x5c472f96, 0x121fd215];
            for (var i = 0; i !== 4; ++i) {
                for (var mask = 1; mask; mask <<= 1) {
                    if (jump[i] & mask) {
                        ns01 ^= this.s01;
                        ns00 ^= this.s00;
                        ns11 ^= this.s11;
                        ns10 ^= this.s10;
                    }
                    this.unsafeNext();
                }
            }
            this.s01 = ns01;
            this.s00 = ns00;
            this.s11 = ns11;
            this.s10 = ns10;
        };
        return XorShift128Plus;
    }());
    var xorshift128plus = function(seed) {
        return new XorShift128Plus(-1, ~seed, seed | 0, 0);
    };
    XorShift.xorshift128plus = xorshift128plus;

    var XoroShiro = {};

    XoroShiro.__esModule = true;
    XoroShiro.xoroshiro128plus = void 0;
    var XoroShiro128Plus = (function() {
        function XoroShiro128Plus(s01, s00, s11, s10) {
            this.s01 = s01;
            this.s00 = s00;
            this.s11 = s11;
            this.s10 = s10;
        }
        XoroShiro128Plus.prototype.min = function() {
            return -0x80000000;
        };
        XoroShiro128Plus.prototype.max = function() {
            return 0x7fffffff;
        };
        XoroShiro128Plus.prototype.clone = function() {
            return new XoroShiro128Plus(this.s01, this.s00, this.s11, this.s10);
        };
        XoroShiro128Plus.prototype.next = function() {
            var nextRng = new XoroShiro128Plus(this.s01, this.s00, this.s11, this.s10);
            var out = nextRng.unsafeNext();
            return [out, nextRng];
        };
        XoroShiro128Plus.prototype.unsafeNext = function() {
            var out = (this.s00 + this.s10) | 0;
            var a0 = this.s10 ^ this.s00;
            var a1 = this.s11 ^ this.s01;
            var s00 = this.s00;
            var s01 = this.s01;
            this.s00 = (s00 << 24) ^ (s01 >>> 8) ^ a0 ^ (a0 << 16);
            this.s01 = (s01 << 24) ^ (s00 >>> 8) ^ a1 ^ ((a1 << 16) | (a0 >>> 16));
            this.s10 = (a1 << 5) ^ (a0 >>> 27);
            this.s11 = (a0 << 5) ^ (a1 >>> 27);
            return out;
        };
        XoroShiro128Plus.prototype.jump = function() {
            var nextRng = new XoroShiro128Plus(this.s01, this.s00, this.s11, this.s10);
            nextRng.unsafeJump();
            return nextRng;
        };
        XoroShiro128Plus.prototype.unsafeJump = function() {
            var ns01 = 0;
            var ns00 = 0;
            var ns11 = 0;
            var ns10 = 0;
            var jump = [0xd8f554a5, 0xdf900294, 0x4b3201fc, 0x170865df];
            for (var i = 0; i !== 4; ++i) {
                for (var mask = 1; mask; mask <<= 1) {
                    if (jump[i] & mask) {
                        ns01 ^= this.s01;
                        ns00 ^= this.s00;
                        ns11 ^= this.s11;
                        ns10 ^= this.s10;
                    }
                    this.unsafeNext();
                }
            }
            this.s01 = ns01;
            this.s00 = ns00;
            this.s11 = ns11;
            this.s10 = ns10;
        };
        return XoroShiro128Plus;
    }());
    var xoroshiro128plus = function(seed) {
        return new XoroShiro128Plus(-1, ~seed, seed | 0, 0);
    };
    XoroShiro.xoroshiro128plus = xoroshiro128plus;

    var UniformArrayIntDistribution = {};

    var UnsafeUniformArrayIntDistribution = {};

    var ArrayInt = {};

    ArrayInt.__esModule = true;
    ArrayInt.substractArrayInt64 = ArrayInt.fromNumberToArrayInt64 = ArrayInt.trimArrayIntInplace =
        ArrayInt.substractArrayIntToNew = ArrayInt.addOneToPositiveArrayInt =
            ArrayInt.addArrayIntToNew = void 0;
    function addArrayIntToNew(arrayIntA, arrayIntB) {
        if (arrayIntA.sign !== arrayIntB.sign) {
            return substractArrayIntToNew(arrayIntA, {sign: -arrayIntB.sign, data: arrayIntB.data});
        }
        var data = [];
        var reminder = 0;
        var dataA = arrayIntA.data;
        var dataB = arrayIntB.data;
        for (var indexA = dataA.length - 1, indexB = dataB.length - 1; indexA >= 0 || indexB >= 0;
             --indexA, --indexB) {
            var vA = indexA >= 0 ? dataA[indexA] : 0;
            var vB = indexB >= 0 ? dataB[indexB] : 0;
            var current = vA + vB + reminder;
            data.push(current >>> 0);
            reminder = ~~(current / 0x100000000);
        }
        if (reminder !== 0) {
            data.push(reminder);
        }
        return {sign: arrayIntA.sign, data: data.reverse()};
    }
    ArrayInt.addArrayIntToNew = addArrayIntToNew;
    function addOneToPositiveArrayInt(arrayInt) {
        arrayInt.sign = 1;
        var data = arrayInt.data;
        for (var index = data.length - 1; index >= 0; --index) {
            if (data[index] === 0xffffffff) {
                data[index] = 0;
            } else {
                data[index] += 1;
                return arrayInt;
            }
        }
        data.unshift(1);
        return arrayInt;
    }
    ArrayInt.addOneToPositiveArrayInt = addOneToPositiveArrayInt;
    function isStrictlySmaller(dataA, dataB) {
        var maxLength = Math.max(dataA.length, dataB.length);
        for (var index = 0; index < maxLength; ++index) {
            var indexA = index + dataA.length - maxLength;
            var indexB = index + dataB.length - maxLength;
            var vA = indexA >= 0 ? dataA[indexA] : 0;
            var vB = indexB >= 0 ? dataB[indexB] : 0;
            if (vA < vB)
                return true;
            if (vA > vB)
                return false;
        }
        return false;
    }
    function substractArrayIntToNew(arrayIntA, arrayIntB) {
        if (arrayIntA.sign !== arrayIntB.sign) {
            return addArrayIntToNew(arrayIntA, {sign: -arrayIntB.sign, data: arrayIntB.data});
        }
        var dataA = arrayIntA.data;
        var dataB = arrayIntB.data;
        if (isStrictlySmaller(dataA, dataB)) {
            var out = substractArrayIntToNew(arrayIntB, arrayIntA);
            out.sign = -out.sign;
            return out;
        }
        var data = [];
        var reminder = 0;
        for (var indexA = dataA.length - 1, indexB = dataB.length - 1; indexA >= 0 || indexB >= 0;
             --indexA, --indexB) {
            var vA = indexA >= 0 ? dataA[indexA] : 0;
            var vB = indexB >= 0 ? dataB[indexB] : 0;
            var current = vA - vB - reminder;
            data.push(current >>> 0);
            reminder = current < 0 ? 1 : 0;
        }
        return {sign: arrayIntA.sign, data: data.reverse()};
    }
    ArrayInt.substractArrayIntToNew = substractArrayIntToNew;
    function trimArrayIntInplace(arrayInt) {
        var data = arrayInt.data;
        var firstNonZero = 0;
        for (; firstNonZero !== data.length && data[firstNonZero] === 0; ++firstNonZero) {
        }
        if (firstNonZero === data.length) {
            arrayInt.sign = 1;
            arrayInt.data = [0];
            return arrayInt;
        }
        data.splice(0, firstNonZero);
        return arrayInt;
    }
    ArrayInt.trimArrayIntInplace = trimArrayIntInplace;
    function fromNumberToArrayInt64(out, n) {
        if (n < 0) {
            var posN = -n;
            out.sign = -1;
            out.data[0] = ~~(posN / 0x100000000);
            out.data[1] = posN >>> 0;
        } else {
            out.sign = 1;
            out.data[0] = ~~(n / 0x100000000);
            out.data[1] = n >>> 0;
        }
        return out;
    }
    ArrayInt.fromNumberToArrayInt64 = fromNumberToArrayInt64;
    function substractArrayInt64(out, arrayIntA, arrayIntB) {
        var lowA = arrayIntA.data[1];
        var highA = arrayIntA.data[0];
        var signA = arrayIntA.sign;
        var lowB = arrayIntB.data[1];
        var highB = arrayIntB.data[0];
        var signB = arrayIntB.sign;
        out.sign = 1;
        if (signA === 1 && signB === -1) {
            var low_1 = lowA + lowB;
            var high = highA + highB + (low_1 > 0xffffffff ? 1 : 0);
            out.data[0] = high >>> 0;
            out.data[1] = low_1 >>> 0;
            return out;
        }
        var lowFirst = lowA;
        var highFirst = highA;
        var lowSecond = lowB;
        var highSecond = highB;
        if (signA === -1) {
            lowFirst = lowB;
            highFirst = highB;
            lowSecond = lowA;
            highSecond = highA;
        }
        var reminderLow = 0;
        var low = lowFirst - lowSecond;
        if (low < 0) {
            reminderLow = 1;
            low = low >>> 0;
        }
        out.data[0] = highFirst - highSecond - reminderLow;
        out.data[1] = low;
        return out;
    }
    ArrayInt.substractArrayInt64 = substractArrayInt64;

    var UnsafeUniformArrayIntDistributionInternal = {};

    var UnsafeUniformIntDistributionInternal = {};

    UnsafeUniformIntDistributionInternal.__esModule = true;
    UnsafeUniformIntDistributionInternal.unsafeUniformIntDistributionInternal = void 0;
    function unsafeUniformIntDistributionInternal(rangeSize, rng) {
        var MinRng = rng.min();
        var NumValues = rng.max() - rng.min() + 1;
        if (rangeSize <= NumValues) {
            var nrng_1 = rng;
            var MaxAllowed = NumValues - (NumValues % rangeSize);
            while (true) {
                var out = nrng_1.unsafeNext();
                var deltaV = out - MinRng;
                if (deltaV < MaxAllowed) {
                    return deltaV % rangeSize;
                }
            }
        }
        var FinalNumValues = NumValues * NumValues;
        var NumIterations = 2;
        while (FinalNumValues < rangeSize) {
            FinalNumValues *= NumValues;
            ++NumIterations;
        }
        var MaxAcceptedRandom = rangeSize * Math.floor((1 * FinalNumValues) / rangeSize);
        var nrng = rng;
        while (true) {
            var value = 0;
            for (var num = 0; num !== NumIterations; ++num) {
                var out = nrng.unsafeNext();
                value = NumValues * value + (out - MinRng);
            }
            if (value < MaxAcceptedRandom) {
                var inDiff = value - rangeSize * Math.floor((1 * value) / rangeSize);
                return inDiff;
            }
        }
    }
    UnsafeUniformIntDistributionInternal.unsafeUniformIntDistributionInternal =
        unsafeUniformIntDistributionInternal;

    UnsafeUniformArrayIntDistributionInternal.__esModule = true;
    UnsafeUniformArrayIntDistributionInternal.unsafeUniformArrayIntDistributionInternal = void 0;
    var UnsafeUniformIntDistributionInternal_1$1 = UnsafeUniformIntDistributionInternal;
    function unsafeUniformArrayIntDistributionInternal(out, rangeSize, rng) {
        var rangeLength = rangeSize.length;
        while (true) {
            for (var index = 0; index !== rangeLength; ++index) {
                var indexRangeSize = index === 0 ? rangeSize[0] + 1 : 0x100000000;
                var g =
                    (0,
                     UnsafeUniformIntDistributionInternal_1$1.unsafeUniformIntDistributionInternal)(
                        indexRangeSize, rng);
                out[index] = g;
            }
            for (var index = 0; index !== rangeLength; ++index) {
                var current = out[index];
                var currentInRange = rangeSize[index];
                if (current < currentInRange) {
                    return out;
                } else if (current > currentInRange) {
                    break;
                }
            }
        }
    }
    UnsafeUniformArrayIntDistributionInternal.unsafeUniformArrayIntDistributionInternal =
        unsafeUniformArrayIntDistributionInternal;

    UnsafeUniformArrayIntDistribution.__esModule = true;
    UnsafeUniformArrayIntDistribution.unsafeUniformArrayIntDistribution = void 0;
    var ArrayInt_1$1 = ArrayInt;
    var UnsafeUniformArrayIntDistributionInternal_1$1 = UnsafeUniformArrayIntDistributionInternal;
    function unsafeUniformArrayIntDistribution(from, to, rng) {
        var rangeSize =
            (0, ArrayInt_1$1.trimArrayIntInplace)((0, ArrayInt_1$1.addOneToPositiveArrayInt)(
                (0, ArrayInt_1$1.substractArrayIntToNew)(to, from)));
        var emptyArrayIntData = rangeSize.data.slice(0);
        var g = (0,
                 UnsafeUniformArrayIntDistributionInternal_1$1
                     .unsafeUniformArrayIntDistributionInternal)(
            emptyArrayIntData, rangeSize.data, rng);
        return (0, ArrayInt_1$1.trimArrayIntInplace)(
            (0, ArrayInt_1$1.addArrayIntToNew)({sign: 1, data: g}, from));
    }
    UnsafeUniformArrayIntDistribution.unsafeUniformArrayIntDistribution =
        unsafeUniformArrayIntDistribution;

    UniformArrayIntDistribution.__esModule = true;
    UniformArrayIntDistribution.uniformArrayIntDistribution = void 0;
    var UnsafeUniformArrayIntDistribution_1$1 = UnsafeUniformArrayIntDistribution;
    function uniformArrayIntDistribution(from, to, rng) {
        if (rng != null) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformArrayIntDistribution_1$1.unsafeUniformArrayIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        }
        return function(rng) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformArrayIntDistribution_1$1.unsafeUniformArrayIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        };
    }
    UniformArrayIntDistribution.uniformArrayIntDistribution = uniformArrayIntDistribution;

    var UniformBigIntDistribution = {};

    var UnsafeUniformBigIntDistribution = {};

    UnsafeUniformBigIntDistribution.__esModule = true;
    UnsafeUniformBigIntDistribution.unsafeUniformBigIntDistribution = void 0;
    function unsafeUniformBigIntDistribution(from, to, rng) {
        var diff = to - from + BigInt(1);
        var MinRng = BigInt(rng.min());
        var NumValues = BigInt(rng.max() - rng.min() + 1);
        var FinalNumValues = NumValues;
        var NumIterations = BigInt(1);
        while (FinalNumValues < diff) {
            FinalNumValues *= NumValues;
            ++NumIterations;
        }
        var MaxAcceptedRandom = FinalNumValues - (FinalNumValues % diff);
        while (true) {
            var value = BigInt(0);
            for (var num = BigInt(0); num !== NumIterations; ++num) {
                var out = rng.unsafeNext();
                value = NumValues * value + (BigInt(out) - MinRng);
            }
            if (value < MaxAcceptedRandom) {
                var inDiff = value % diff;
                return inDiff + from;
            }
        }
    }
    UnsafeUniformBigIntDistribution.unsafeUniformBigIntDistribution =
        unsafeUniformBigIntDistribution;

    UniformBigIntDistribution.__esModule = true;
    UniformBigIntDistribution.uniformBigIntDistribution = void 0;
    var UnsafeUniformBigIntDistribution_1$1 = UnsafeUniformBigIntDistribution;
    function uniformBigIntDistribution(from, to, rng) {
        if (rng != null) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformBigIntDistribution_1$1.unsafeUniformBigIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        }
        return function(rng) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformBigIntDistribution_1$1.unsafeUniformBigIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        };
    }
    UniformBigIntDistribution.uniformBigIntDistribution = uniformBigIntDistribution;

    var UniformIntDistribution = {};

    var UnsafeUniformIntDistribution = {};

    UnsafeUniformIntDistribution.__esModule = true;
    UnsafeUniformIntDistribution.unsafeUniformIntDistribution = void 0;
    var UnsafeUniformIntDistributionInternal_1 = UnsafeUniformIntDistributionInternal;
    var ArrayInt_1 = ArrayInt;
    var UnsafeUniformArrayIntDistributionInternal_1 = UnsafeUniformArrayIntDistributionInternal;
    var sharedA = {sign: 1, data: [0, 0]};
    var sharedB = {sign: 1, data: [0, 0]};
    var sharedC = {sign: 1, data: [0, 0]};
    var sharedData = [0, 0];
    function uniformLargeIntInternal(from, to, rangeSize, rng) {
        var rangeSizeArrayIntValue = rangeSize <= Number.MAX_SAFE_INTEGER
            ? (0, ArrayInt_1.fromNumberToArrayInt64)(sharedC, rangeSize)
            : (0, ArrayInt_1.substractArrayInt64)(
                  sharedC,
                  (0, ArrayInt_1.fromNumberToArrayInt64)(sharedA, to),
                  (0, ArrayInt_1.fromNumberToArrayInt64)(sharedB, from));
        if (rangeSizeArrayIntValue.data[1] === 0xffffffff) {
            rangeSizeArrayIntValue.data[0] += 1;
            rangeSizeArrayIntValue.data[1] = 0;
        } else {
            rangeSizeArrayIntValue.data[1] += 1;
        }
        (0, UnsafeUniformArrayIntDistributionInternal_1.unsafeUniformArrayIntDistributionInternal)(
            sharedData, rangeSizeArrayIntValue.data, rng);
        return sharedData[0] * 0x100000000 + sharedData[1] + from;
    }
    function unsafeUniformIntDistribution(from, to, rng) {
        var rangeSize = to - from;
        if (rangeSize <= 0xffffffff) {
            var g =
                (0, UnsafeUniformIntDistributionInternal_1.unsafeUniformIntDistributionInternal)(
                    rangeSize + 1, rng);
            return g + from;
        }
        return uniformLargeIntInternal(from, to, rangeSize, rng);
    }
    UnsafeUniformIntDistribution.unsafeUniformIntDistribution = unsafeUniformIntDistribution;

    UniformIntDistribution.__esModule = true;
    UniformIntDistribution.uniformIntDistribution = void 0;
    var UnsafeUniformIntDistribution_1$1 = UnsafeUniformIntDistribution;
    function uniformIntDistribution(from, to, rng) {
        if (rng != null) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformIntDistribution_1$1.unsafeUniformIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        }
        return function(rng) {
            var nextRng = rng.clone();
            return [
                (0, UnsafeUniformIntDistribution_1$1.unsafeUniformIntDistribution)(
                    from, to, nextRng),
                nextRng
            ];
        };
    }
    UniformIntDistribution.uniformIntDistribution = uniformIntDistribution;

    pureRandDefault.__esModule = true;
    pureRandDefault.unsafeUniformIntDistribution = pureRandDefault.unsafeUniformBigIntDistribution =
        pureRandDefault.unsafeUniformArrayIntDistribution = pureRandDefault.uniformIntDistribution =
            pureRandDefault.uniformBigIntDistribution =
                pureRandDefault.uniformArrayIntDistribution = pureRandDefault.xoroshiro128plus =
                    pureRandDefault.xorshift128plus = pureRandDefault.mersenne =
                        pureRandDefault.congruential32 = pureRandDefault.congruential =
                            pureRandDefault.unsafeSkipN = pureRandDefault.unsafeGenerateN =
                                pureRandDefault.skipN = pureRandDefault.generateN =
                                    pureRandDefault.__commitHash = pureRandDefault.__version =
                                        pureRandDefault.__type = void 0;
    var RandomGenerator_1 = RandomGenerator;
    pureRandDefault.generateN = RandomGenerator_1.generateN;
    pureRandDefault.skipN = RandomGenerator_1.skipN;
    pureRandDefault.unsafeGenerateN = RandomGenerator_1.unsafeGenerateN;
    pureRandDefault.unsafeSkipN = RandomGenerator_1.unsafeSkipN;
    var LinearCongruential_1 = LinearCongruential$1;
    pureRandDefault.congruential = LinearCongruential_1.congruential;
    pureRandDefault.congruential32 = LinearCongruential_1.congruential32;
    var MersenneTwister_1 = MersenneTwister;
    pureRandDefault.mersenne = MersenneTwister_1["default"];
    var XorShift_1 = XorShift;
    pureRandDefault.xorshift128plus = XorShift_1.xorshift128plus;
    var XoroShiro_1 = XoroShiro;
    pureRandDefault.xoroshiro128plus = XoroShiro_1.xoroshiro128plus;
    var UniformArrayIntDistribution_1 = UniformArrayIntDistribution;
    pureRandDefault.uniformArrayIntDistribution =
        UniformArrayIntDistribution_1.uniformArrayIntDistribution;
    var UniformBigIntDistribution_1 = UniformBigIntDistribution;
    pureRandDefault.uniformBigIntDistribution =
        UniformBigIntDistribution_1.uniformBigIntDistribution;
    var UniformIntDistribution_1 = UniformIntDistribution;
    pureRandDefault.uniformIntDistribution = UniformIntDistribution_1.uniformIntDistribution;
    var UnsafeUniformArrayIntDistribution_1 = UnsafeUniformArrayIntDistribution;
    pureRandDefault.unsafeUniformArrayIntDistribution =
        UnsafeUniformArrayIntDistribution_1.unsafeUniformArrayIntDistribution;
    var UnsafeUniformBigIntDistribution_1 = UnsafeUniformBigIntDistribution;
    pureRandDefault.unsafeUniformBigIntDistribution =
        UnsafeUniformBigIntDistribution_1.unsafeUniformBigIntDistribution;
    var UnsafeUniformIntDistribution_1 = UnsafeUniformIntDistribution;
    pureRandDefault.unsafeUniformIntDistribution =
        UnsafeUniformIntDistribution_1.unsafeUniformIntDistribution;
    var __type = 'commonjs';
    pureRandDefault.__type = __type;
    var __version = '5.0.1';
    pureRandDefault.__version = __version;
    var __commitHash = '229c91e5c41acd30afc2cccabe9ba93c99db5df7';
    pureRandDefault.__commitHash = __commitHash;

    (function(exports) {
        var __createBinding = (commonjsGlobal && commonjsGlobal.__createBinding) ||
            (Object.create ? (function(o, m, k, k2) {
                                  if (k2 === undefined)
                                      k2 = k;
                                  Object.defineProperty(o, k2, {
                                      enumerable: true,
                                      get: function() {
                                          return m[k];
                                      }
                                  });
                              })
                           : (function(o, m, k, k2) {
                                 if (k2 === undefined)
                                     k2 = k;
                                 o[k2] = m[k];
                             }));
        var __exportStar = (commonjsGlobal && commonjsGlobal.__exportStar) || function(m, exports) {
            for (var p in m)
                if (p !== "default" && !Object.prototype.hasOwnProperty.call(exports, p))
                    __createBinding(exports, m, p);
        };
        exports.__esModule = true;
        var prand = pureRandDefault;
        exports["default"] = prand;
        __exportStar(pureRandDefault, exports);
    }(pureRand));

    var VerbosityLevel = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.VerbosityLevel = void 0;
        (function(VerbosityLevel) {
            VerbosityLevel[VerbosityLevel["None"] = 0] = "None";
            VerbosityLevel[VerbosityLevel["Verbose"] = 1] = "Verbose";
            VerbosityLevel[VerbosityLevel["VeryVerbose"] = 2] = "VeryVerbose";
        })(exports.VerbosityLevel || (exports.VerbosityLevel = {}));
    }(VerbosityLevel));

    Object.defineProperty(QualifiedParameters$1, "__esModule", {value: true});
    QualifiedParameters$1.QualifiedParameters = void 0;
    const pure_rand_1$2 = pureRand;
    const VerbosityLevel_1$2 = VerbosityLevel;
    class QualifiedParameters {
        constructor(op) {
            const p = op || {};
            this.seed = QualifiedParameters.readSeed(p);
            this.randomType = QualifiedParameters.readRandomType(p);
            this.numRuns = QualifiedParameters.readNumRuns(p);
            this.verbose = QualifiedParameters.readVerbose(p);
            this.maxSkipsPerRun = QualifiedParameters.readOrDefault(p, 'maxSkipsPerRun', 100);
            this.timeout = QualifiedParameters.readOrDefault(p, 'timeout', null);
            this.skipAllAfterTimeLimit =
                QualifiedParameters.readOrDefault(p, 'skipAllAfterTimeLimit', null);
            this.interruptAfterTimeLimit =
                QualifiedParameters.readOrDefault(p, 'interruptAfterTimeLimit', null);
            this.markInterruptAsFailure =
                QualifiedParameters.readBoolean(p, 'markInterruptAsFailure');
            this.skipEqualValues = QualifiedParameters.readBoolean(p, 'skipEqualValues');
            this.ignoreEqualValues = QualifiedParameters.readBoolean(p, 'ignoreEqualValues');
            this.logger = QualifiedParameters.readOrDefault(p, 'logger', (v) => {
                console.log(v);
            });
            this.path = QualifiedParameters.readOrDefault(p, 'path', '');
            this.unbiased = QualifiedParameters.readBoolean(p, 'unbiased');
            this.examples = QualifiedParameters.readOrDefault(p, 'examples', []);
            this.endOnFailure = QualifiedParameters.readBoolean(p, 'endOnFailure');
            this.reporter = QualifiedParameters.readOrDefault(p, 'reporter', null);
            this.asyncReporter = QualifiedParameters.readOrDefault(p, 'asyncReporter', null);
        }
        toParameters() {
            const orUndefined = (value) => (value !== null ? value : undefined);
            return {
                seed: this.seed,
                randomType: this.randomType,
                numRuns: this.numRuns,
                maxSkipsPerRun: this.maxSkipsPerRun,
                timeout: orUndefined(this.timeout),
                skipAllAfterTimeLimit: orUndefined(this.skipAllAfterTimeLimit),
                interruptAfterTimeLimit: orUndefined(this.interruptAfterTimeLimit),
                markInterruptAsFailure: this.markInterruptAsFailure,
                skipEqualValues: this.skipEqualValues,
                ignoreEqualValues: this.ignoreEqualValues,
                path: this.path,
                logger: this.logger,
                unbiased: this.unbiased,
                verbose: this.verbose,
                examples: this.examples,
                endOnFailure: this.endOnFailure,
                reporter: orUndefined(this.reporter),
                asyncReporter: orUndefined(this.asyncReporter),
            };
        }
        static read(op) {
            return new QualifiedParameters(op);
        }
    }
    QualifiedParameters$1.QualifiedParameters = QualifiedParameters;
    QualifiedParameters.readSeed = (p) => {
        if (p.seed == null)
            return Date.now() ^ (Math.random() * 0x100000000);
        const seed32 = p.seed | 0;
        if (p.seed === seed32)
            return seed32;
        const gap = p.seed - seed32;
        return seed32 ^ (gap * 0x100000000);
    };
    QualifiedParameters.readRandomType = (p) => {
        if (p.randomType == null)
            return pure_rand_1$2.default.xorshift128plus;
        if (typeof p.randomType === 'string') {
            switch (p.randomType) {
                case 'mersenne':
                    return pure_rand_1$2.default.mersenne;
                case 'congruential':
                    return pure_rand_1$2.default.congruential;
                case 'congruential32':
                    return pure_rand_1$2.default.congruential32;
                case 'xorshift128plus':
                    return pure_rand_1$2.default.xorshift128plus;
                case 'xoroshiro128plus':
                    return pure_rand_1$2.default.xoroshiro128plus;
                default:
                    throw new Error(`Invalid random specified: '${p.randomType}'`);
            }
        }
        return p.randomType;
    };
    QualifiedParameters.readNumRuns = (p) => {
        const defaultValue = 100;
        if (p.numRuns != null)
            return p.numRuns;
        if (p.num_runs != null)
            return p.num_runs;
        return defaultValue;
    };
    QualifiedParameters.readVerbose = (p) => {
        if (p.verbose == null)
            return VerbosityLevel_1$2.VerbosityLevel.None;
        if (typeof p.verbose === 'boolean') {
            return p.verbose === true ? VerbosityLevel_1$2.VerbosityLevel.Verbose
                                      : VerbosityLevel_1$2.VerbosityLevel.None;
        }
        if (p.verbose <= VerbosityLevel_1$2.VerbosityLevel.None) {
            return VerbosityLevel_1$2.VerbosityLevel.None;
        }
        if (p.verbose >= VerbosityLevel_1$2.VerbosityLevel.VeryVerbose) {
            return VerbosityLevel_1$2.VerbosityLevel.VeryVerbose;
        }
        return p.verbose | 0;
    };
    QualifiedParameters.readBoolean = (p, key) => p[key] === true;
    QualifiedParameters.readOrDefault = (p, key, defaultValue) => {
        const value = p[key];
        return value != null ? value : defaultValue;
    };

    var DecorateProperty = {};

    var SkipAfterProperty$1 = {};

    Object.defineProperty(SkipAfterProperty$1, "__esModule", {value: true});
    SkipAfterProperty$1.SkipAfterProperty = void 0;
    const PreconditionFailure_1$2 = PreconditionFailure$1;
    class SkipAfterProperty {
        constructor(property, getTime, timeLimit, interruptExecution) {
            this.property = property;
            this.getTime = getTime;
            this.interruptExecution = interruptExecution;
            this.skipAfterTime = this.getTime() + timeLimit;
        }
        isAsync() {
            return this.property.isAsync();
        }
        generate(mrng, runId) {
            return this.property.generate(mrng, runId);
        }
        shrink(value) {
            return this.property.shrink(value);
        }
        run(v) {
            if (this.getTime() >= this.skipAfterTime) {
                const preconditionFailure =
                    new PreconditionFailure_1$2.PreconditionFailure(this.interruptExecution);
                if (this.isAsync()) {
                    return Promise.resolve(preconditionFailure);
                } else {
                    return preconditionFailure;
                }
            }
            return this.property.run(v);
        }
    }
    SkipAfterProperty$1.SkipAfterProperty = SkipAfterProperty;

    var TimeoutProperty$1 = {};

    Object.defineProperty(TimeoutProperty$1, "__esModule", {value: true});
    TimeoutProperty$1.TimeoutProperty = void 0;
    const timeoutAfter = (timeMs) => {
        let timeoutHandle = null;
        const promise = new Promise((resolve) => {
            timeoutHandle = setTimeout(() => {
                resolve({
                    error: undefined,
                    errorMessage: `Property timeout: exceeded limit of ${timeMs} milliseconds`
                });
            }, timeMs);
        });
        return {
            clear: () => clearTimeout(timeoutHandle),
            promise,
        };
    };
    class TimeoutProperty {
        constructor(property, timeMs) {
            this.property = property;
            this.timeMs = timeMs;
        }
        isAsync() {
            return true;
        }
        generate(mrng, runId) {
            return this.property.generate(mrng, runId);
        }
        shrink(value) {
            return this.property.shrink(value);
        }
        async run(v) {
            const t = timeoutAfter(this.timeMs);
            const propRun = Promise.race([this.property.run(v), t.promise]);
            propRun.then(t.clear, t.clear);
            return propRun;
        }
    }
    TimeoutProperty$1.TimeoutProperty = TimeoutProperty;

    var UnbiasedProperty$1 = {};

    Object.defineProperty(UnbiasedProperty$1, "__esModule", {value: true});
    UnbiasedProperty$1.UnbiasedProperty = void 0;
    class UnbiasedProperty {
        constructor(property) {
            this.property = property;
        }
        isAsync() {
            return this.property.isAsync();
        }
        generate(mrng, _runId) {
            return this.property.generate(mrng, undefined);
        }
        shrink(value) {
            return this.property.shrink(value);
        }
        run(v) {
            return this.property.run(v);
        }
    }
    UnbiasedProperty$1.UnbiasedProperty = UnbiasedProperty;

    var IgnoreEqualValuesProperty$1 = {};

    var stringify = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.asyncStringify = exports.possiblyAsyncStringify = exports.stringify =
            exports.stringifyInternal = exports.hasAsyncToStringMethod =
                exports.asyncToStringMethod = exports.hasToStringMethod = exports.toStringMethod =
                    void 0;
        exports.toStringMethod = Symbol('fast-check/toStringMethod');
        function hasToStringMethod(instance) {
            return (instance !== null &&
                    (typeof instance === 'object' || typeof instance === 'function') &&
                    exports.toStringMethod in instance &&
                    typeof instance[exports.toStringMethod] === 'function');
        }
        exports.hasToStringMethod = hasToStringMethod;
        exports.asyncToStringMethod = Symbol('fast-check/asyncToStringMethod');
        function hasAsyncToStringMethod(instance) {
            return (instance !== null &&
                    (typeof instance === 'object' || typeof instance === 'function') &&
                    exports.asyncToStringMethod in instance &&
                    typeof instance[exports.asyncToStringMethod] === 'function');
        }
        exports.hasAsyncToStringMethod = hasAsyncToStringMethod;
        const findSymbolNameRegex = /^Symbol\((.*)\)$/;
        function getSymbolDescription(s) {
            if (s.description !== undefined)
                return s.description;
            const m = findSymbolNameRegex.exec(String(s));
            return m && m[1].length ? m[1] : null;
        }
        function stringifyNumber(numValue) {
            switch (numValue) {
                case 0:
                    return 1 / numValue === Number.NEGATIVE_INFINITY ? '-0' : '0';
                case Number.NEGATIVE_INFINITY:
                    return 'Number.NEGATIVE_INFINITY';
                case Number.POSITIVE_INFINITY:
                    return 'Number.POSITIVE_INFINITY';
                default:
                    return numValue === numValue ? String(numValue) : 'Number.NaN';
            }
        }
        function isSparseArray(arr) {
            let previousNumberedIndex = -1;
            for (const index in arr) {
                const numberedIndex = Number(index);
                if (numberedIndex !== previousNumberedIndex + 1)
                    return true;
                previousNumberedIndex = numberedIndex;
            }
            return previousNumberedIndex + 1 !== arr.length;
        }
        function stringifyInternal(value, previousValues, getAsyncContent) {
            const currentValues = previousValues.concat([value]);
            if (typeof value === 'object') {
                if (previousValues.indexOf(value) !== -1) {
                    return '[cyclic]';
                }
            }
            if (hasAsyncToStringMethod(value)) {
                const content = getAsyncContent(value);
                if (content.state === 'fulfilled') {
                    return content.value;
                }
            }
            if (hasToStringMethod(value)) {
                try {
                    return value[exports.toStringMethod]();
                } catch (err) {
                }
            }
            switch (Object.prototype.toString.call(value)) {
                case '[object Array]': {
                    const arr = value;
                    if (arr.length >= 50 && isSparseArray(arr)) {
                        const assignments = [];
                        for (const index in arr) {
                            if (!Number.isNaN(Number(index)))
                                assignments.push(`${index}:${
                                    stringifyInternal(
                                        arr[index], currentValues, getAsyncContent)}`);
                        }
                        return assignments.length !== 0
                            ? `Object.assign(Array(${arr.length}),{${assignments.join(',')}})`
                            : `Array(${arr.length})`;
                    }
                    const stringifiedArray =
                        arr.map((v) => stringifyInternal(v, currentValues, getAsyncContent))
                            .join(',');
                    return arr.length === 0 || arr.length - 1 in arr ? `[${stringifiedArray}]`
                                                                     : `[${stringifiedArray},]`;
                }
                case '[object BigInt]':
                    return `${value}n`;
                case '[object Boolean]':
                    return typeof value === 'boolean' ? JSON.stringify(value)
                                                      : `new Boolean(${JSON.stringify(value)})`;
                case '[object Date]': {
                    const d = value;
                    return Number.isNaN(d.getTime())
                        ? `new Date(NaN)`
                        : `new Date(${JSON.stringify(d.toISOString())})`;
                }
                case '[object Map]':
                    return `new Map(${
                        stringifyInternal(Array.from(value), currentValues, getAsyncContent)})`;
                case '[object Null]':
                    return `null`;
                case '[object Number]':
                    return typeof value === 'number'
                        ? stringifyNumber(value)
                        : `new Number(${stringifyNumber(Number(value))})`;
                case '[object Object]': {
                    try {
                        const toStringAccessor = value.toString;
                        if (typeof toStringAccessor === 'function' &&
                            toStringAccessor !== Object.prototype.toString) {
                            return value.toString();
                        }
                    } catch (err) {
                        return '[object Object]';
                    }
                    const mapper = (k) => `${
                        k === '__proto__' ? '["__proto__"]'
                                          : typeof k === 'symbol'
                                ? `[${stringifyInternal(k, currentValues, getAsyncContent)}]`
                                : JSON.stringify(k)}:${
                        stringifyInternal(value[k], currentValues, getAsyncContent)}`;
                    const stringifiedProperties = [
                        ...Object.keys(value).map(mapper),
                        ...Object.getOwnPropertySymbols(value)
                            .filter((s) => {
                                const descriptor = Object.getOwnPropertyDescriptor(value, s);
                                return descriptor && descriptor.enumerable;
                            })
                            .map(mapper),
                    ];
                    const rawRepr = '{' + stringifiedProperties.join(',') + '}';
                    if (Object.getPrototypeOf(value) === null) {
                        return rawRepr === '{}' ? 'Object.create(null)'
                                                : `Object.assign(Object.create(null),${rawRepr})`;
                    }
                    return rawRepr;
                }
                case '[object Set]':
                    return `new Set(${
                        stringifyInternal(Array.from(value), currentValues, getAsyncContent)})`;
                case '[object String]':
                    return typeof value === 'string' ? JSON.stringify(value)
                                                     : `new String(${JSON.stringify(value)})`;
                case '[object Symbol]': {
                    const s = value;
                    if (Symbol.keyFor(s) !== undefined) {
                        return `Symbol.for(${JSON.stringify(Symbol.keyFor(s))})`;
                    }
                    const desc = getSymbolDescription(s);
                    if (desc === null) {
                        return 'Symbol()';
                    }
                    const knownSymbol = desc.startsWith('Symbol.') && Symbol[desc.substring(7)];
                    return s === knownSymbol ? desc : `Symbol(${JSON.stringify(desc)})`;
                }
                case '[object Promise]': {
                    const promiseContent = getAsyncContent(value);
                    switch (promiseContent.state) {
                        case 'fulfilled':
                            return `Promise.resolve(${
                                stringifyInternal(
                                    promiseContent.value, currentValues, getAsyncContent)})`;
                        case 'rejected':
                            return `Promise.reject(${
                                stringifyInternal(
                                    promiseContent.value, currentValues, getAsyncContent)})`;
                        case 'pending':
                            return `new Promise(() => {/*pending*/})`;
                        case 'unknown':
                        default:
                            return `new Promise(() => {/*unknown*/})`;
                    }
                }
                case '[object Error]':
                    if (value instanceof Error) {
                        return `new Error(${
                            stringifyInternal(value.message, currentValues, getAsyncContent)})`;
                    }
                    break;
                case '[object Undefined]':
                    return `undefined`;
                case '[object Int8Array]':
                case '[object Uint8Array]':
                case '[object Uint8ClampedArray]':
                case '[object Int16Array]':
                case '[object Uint16Array]':
                case '[object Int32Array]':
                case '[object Uint32Array]':
                case '[object Float32Array]':
                case '[object Float64Array]':
                case '[object BigInt64Array]':
                case '[object BigUint64Array]': {
                    if (typeof Buffer !== 'undefined' && typeof Buffer.isBuffer === 'function' &&
                        Buffer.isBuffer(value)) {
                        return `Buffer.from(${
                            stringifyInternal(
                                Array.from(value.values()), currentValues, getAsyncContent)})`;
                    }
                    const valuePrototype = Object.getPrototypeOf(value);
                    const className = valuePrototype && valuePrototype.constructor &&
                        valuePrototype.constructor.name;
                    if (typeof className === 'string') {
                        const typedArray = value;
                        const valuesFromTypedArr = typedArray.values();
                        return `${className}.from(${
                            stringifyInternal(
                                Array.from(valuesFromTypedArr), currentValues, getAsyncContent)})`;
                    }
                    break;
                }
            }
            try {
                return value.toString();
            } catch (_a) {
                return Object.prototype.toString.call(value);
            }
        }
        exports.stringifyInternal = stringifyInternal;
        function stringify(value) {
            return stringifyInternal(value, [], () => ({state: 'unknown', value: undefined}));
        }
        exports.stringify = stringify;
        function possiblyAsyncStringify(value) {
            const stillPendingMarker = Symbol();
            const pendingPromisesForCache = [];
            const cache = new Map();
            function createDelay0() {
                let handleId = null;
                const cancel = () => {
                    if (handleId !== null) {
                        clearTimeout(handleId);
                    }
                };
                const delay = new Promise((resolve) => {
                    handleId = setTimeout(() => {
                        handleId = null;
                        resolve(stillPendingMarker);
                    }, 0);
                });
                return {delay, cancel};
            }
            const unknownState = {state: 'unknown', value: undefined};
            const getAsyncContent = function getAsyncContent(data) {
                const cacheKey = data;
                if (cache.has(cacheKey)) {
                    return cache.get(cacheKey);
                }
                const delay0 = createDelay0();
                const p = exports.asyncToStringMethod in data
                    ? Promise.resolve().then(() => data[exports.asyncToStringMethod]())
                    : data;
                p.catch(() => {});
                pendingPromisesForCache.push(
                    Promise.race([p, delay0.delay])
                        .then(
                            (successValue) => {
                                if (successValue === stillPendingMarker)
                                    cache.set(cacheKey, {state: 'pending', value: undefined});
                                else
                                    cache.set(cacheKey, {state: 'fulfilled', value: successValue});
                                delay0.cancel();
                            },
                            (errorValue) => {
                                cache.set(cacheKey, {state: 'rejected', value: errorValue});
                                delay0.cancel();
                            }));
                cache.set(cacheKey, unknownState);
                return unknownState;
            };
            function loop() {
                const stringifiedValue = stringifyInternal(value, [], getAsyncContent);
                if (pendingPromisesForCache.length === 0) {
                    return stringifiedValue;
                }
                return Promise.all(pendingPromisesForCache.splice(0)).then(loop);
            }
            return loop();
        }
        exports.possiblyAsyncStringify = possiblyAsyncStringify;
        async function asyncStringify(value) {
            return Promise.resolve(possiblyAsyncStringify(value));
        }
        exports.asyncStringify = asyncStringify;
    }(stringify));

    Object.defineProperty(IgnoreEqualValuesProperty$1, "__esModule", {value: true});
    IgnoreEqualValuesProperty$1.IgnoreEqualValuesProperty = void 0;
    const stringify_1$7 = stringify;
    const PreconditionFailure_1$1 = PreconditionFailure$1;
    function fromSyncCached(cachedValue) {
        return cachedValue === null ? new PreconditionFailure_1$1.PreconditionFailure()
                                    : cachedValue;
    }
    function fromCached(...data) {
        if (data[1])
            return data[0].then(fromSyncCached);
        return fromSyncCached(data[0]);
    }
    function fromCachedUnsafe(cachedValue, isAsync) {
        return fromCached(cachedValue, isAsync);
    }
    class IgnoreEqualValuesProperty {
        constructor(property, skipRuns) {
            this.property = property;
            this.skipRuns = skipRuns;
            this.coveredCases = new Map();
        }
        isAsync() {
            return this.property.isAsync();
        }
        generate(mrng, runId) {
            return this.property.generate(mrng, runId);
        }
        shrink(value) {
            return this.property.shrink(value);
        }
        run(v) {
            const stringifiedValue = (0, stringify_1$7.stringify)(v);
            if (this.coveredCases.has(stringifiedValue)) {
                const lastOutput = this.coveredCases.get(stringifiedValue);
                if (!this.skipRuns) {
                    return lastOutput;
                }
                return fromCachedUnsafe(lastOutput, this.property.isAsync());
            }
            const out = this.property.run(v);
            this.coveredCases.set(stringifiedValue, out);
            return out;
        }
    }
    IgnoreEqualValuesProperty$1.IgnoreEqualValuesProperty = IgnoreEqualValuesProperty;

    Object.defineProperty(DecorateProperty, "__esModule", {value: true});
    DecorateProperty.decorateProperty = void 0;
    const SkipAfterProperty_1 = SkipAfterProperty$1;
    const TimeoutProperty_1 = TimeoutProperty$1;
    const UnbiasedProperty_1$1 = UnbiasedProperty$1;
    const IgnoreEqualValuesProperty_1 = IgnoreEqualValuesProperty$1;
    function decorateProperty(rawProperty, qParams) {
        let prop = rawProperty;
        if (rawProperty.isAsync() && qParams.timeout != null) {
            prop = new TimeoutProperty_1.TimeoutProperty(prop, qParams.timeout);
        }
        if (qParams.unbiased) {
            prop = new UnbiasedProperty_1$1.UnbiasedProperty(prop);
        }
        if (qParams.skipAllAfterTimeLimit != null) {
            prop = new SkipAfterProperty_1.SkipAfterProperty(
                prop, Date.now, qParams.skipAllAfterTimeLimit, false);
        }
        if (qParams.interruptAfterTimeLimit != null) {
            prop = new SkipAfterProperty_1.SkipAfterProperty(
                prop, Date.now, qParams.interruptAfterTimeLimit, true);
        }
        if (qParams.skipEqualValues) {
            prop = new IgnoreEqualValuesProperty_1.IgnoreEqualValuesProperty(prop, true);
        }
        if (qParams.ignoreEqualValues) {
            prop = new IgnoreEqualValuesProperty_1.IgnoreEqualValuesProperty(prop, false);
        }
        return prop;
    }
    DecorateProperty.decorateProperty = decorateProperty;

    var RunnerIterator$1 = {};

    var RunExecution$1 = {};

    var ExecutionStatus = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.ExecutionStatus = void 0;
        (function(ExecutionStatus) {
            ExecutionStatus[ExecutionStatus["Success"] = 0] = "Success";
            ExecutionStatus[ExecutionStatus["Skipped"] = -1] = "Skipped";
            ExecutionStatus[ExecutionStatus["Failure"] = 1] = "Failure";
        })(exports.ExecutionStatus || (exports.ExecutionStatus = {}));
    }(ExecutionStatus));

    Object.defineProperty(RunExecution$1, "__esModule", {value: true});
    RunExecution$1.RunExecution = void 0;
    const VerbosityLevel_1$1 = VerbosityLevel;
    const ExecutionStatus_1$1 = ExecutionStatus;
    class RunExecution {
        constructor(verbosity, interruptedAsFailure) {
            this.verbosity = verbosity;
            this.interruptedAsFailure = interruptedAsFailure;
            this.isSuccess = () => this.pathToFailure == null;
            this.firstFailure = () => (this.pathToFailure ? +this.pathToFailure.split(':')[0] : -1);
            this.numShrinks = () =>
                (this.pathToFailure ? this.pathToFailure.split(':').length - 1 : 0);
            this.rootExecutionTrees = [];
            this.currentLevelExecutionTrees = this.rootExecutionTrees;
            this.failure = null;
            this.numSkips = 0;
            this.numSuccesses = 0;
            this.interrupted = false;
        }
        appendExecutionTree(status, value) {
            const currentTree = {status, value, children: []};
            this.currentLevelExecutionTrees.push(currentTree);
            return currentTree;
        }
        fail(value, id, failure) {
            if (this.verbosity >= VerbosityLevel_1$1.VerbosityLevel.Verbose) {
                const currentTree =
                    this.appendExecutionTree(ExecutionStatus_1$1.ExecutionStatus.Failure, value);
                this.currentLevelExecutionTrees = currentTree.children;
            }
            if (this.pathToFailure == null)
                this.pathToFailure = `${id}`;
            else
                this.pathToFailure += `:${id}`;
            this.value = value;
            this.failure = failure;
        }
        skip(value) {
            if (this.verbosity >= VerbosityLevel_1$1.VerbosityLevel.VeryVerbose) {
                this.appendExecutionTree(ExecutionStatus_1$1.ExecutionStatus.Skipped, value);
            }
            if (this.pathToFailure == null) {
                ++this.numSkips;
            }
        }
        success(value) {
            if (this.verbosity >= VerbosityLevel_1$1.VerbosityLevel.VeryVerbose) {
                this.appendExecutionTree(ExecutionStatus_1$1.ExecutionStatus.Success, value);
            }
            if (this.pathToFailure == null) {
                ++this.numSuccesses;
            }
        }
        interrupt() {
            this.interrupted = true;
        }
        extractFailures() {
            if (this.isSuccess()) {
                return [];
            }
            const failures = [];
            let cursor = this.rootExecutionTrees;
            while (cursor.length > 0 &&
                   cursor[cursor.length - 1].status ===
                       ExecutionStatus_1$1.ExecutionStatus.Failure) {
                const failureTree = cursor[cursor.length - 1];
                failures.push(failureTree.value);
                cursor = failureTree.children;
            }
            return failures;
        }
        toRunDetails(seed, basePath, maxSkips, qParams) {
            if (!this.isSuccess()) {
                return {
                    failed: true,
                    interrupted: this.interrupted,
                    numRuns: this.firstFailure() + 1 - this.numSkips,
                    numSkips: this.numSkips,
                    numShrinks: this.numShrinks(),
                    seed,
                    counterexample: this.value,
                    counterexamplePath: RunExecution.mergePaths(basePath, this.pathToFailure),
                    error: this.failure.errorMessage,
                    errorInstance: this.failure.error,
                    failures: this.extractFailures(),
                    executionSummary: this.rootExecutionTrees,
                    verbose: this.verbosity,
                    runConfiguration: qParams.toParameters(),
                };
            }
            const failed =
                this.numSkips > maxSkips || (this.interrupted && this.interruptedAsFailure);
            return {
                failed,
                interrupted: this.interrupted,
                numRuns: this.numSuccesses,
                numSkips: this.numSkips,
                numShrinks: 0,
                seed,
                counterexample: null,
                counterexamplePath: null,
                error: null,
                errorInstance: null,
                failures: [],
                executionSummary: this.rootExecutionTrees,
                verbose: this.verbosity,
                runConfiguration: qParams.toParameters(),
            };
        }
    }
    RunExecution$1.RunExecution = RunExecution;
    RunExecution.mergePaths = (offsetPath, path) => {
        if (offsetPath.length === 0)
            return path;
        const offsetItems = offsetPath.split(':');
        const remainingItems = path.split(':');
        const middle = +offsetItems[offsetItems.length - 1] + +remainingItems[0];
        return [
            ...offsetItems.slice(0, offsetItems.length - 1),
            `${middle}`,
            ...remainingItems.slice(1)
        ].join(':');
    };

    Object.defineProperty(RunnerIterator$1, "__esModule", {value: true});
    RunnerIterator$1.RunnerIterator = void 0;
    const PreconditionFailure_1 = PreconditionFailure$1;
    const RunExecution_1 = RunExecution$1;
    class RunnerIterator {
        constructor(sourceValues, shrink, verbose, interruptedAsFailure) {
            this.sourceValues = sourceValues;
            this.shrink = shrink;
            this.runExecution = new RunExecution_1.RunExecution(verbose, interruptedAsFailure);
            this.currentIdx = -1;
            this.nextValues = sourceValues;
        }
        [Symbol.iterator]() {
            return this;
        }
        next() {
            const nextValue = this.nextValues.next();
            if (nextValue.done || this.runExecution.interrupted) {
                return {done: true, value: undefined};
            }
            this.currentValue = nextValue.value;
            ++this.currentIdx;
            return {done: false, value: nextValue.value.value_};
        }
        handleResult(result) {
            if (result != null && typeof result === 'object' &&
                !PreconditionFailure_1.PreconditionFailure.isFailure(result)) {
                this.runExecution.fail(this.currentValue.value_, this.currentIdx, result);
                this.currentIdx = -1;
                this.nextValues = this.shrink(this.currentValue);
            } else if (result != null) {
                if (!result.interruptExecution) {
                    this.runExecution.skip(this.currentValue.value_);
                    this.sourceValues.skippedOne();
                } else {
                    this.runExecution.interrupt();
                }
            } else {
                this.runExecution.success(this.currentValue.value_);
            }
        }
    }
    RunnerIterator$1.RunnerIterator = RunnerIterator;

    var SourceValuesIterator$1 = {};

    Object.defineProperty(SourceValuesIterator$1, "__esModule", {value: true});
    SourceValuesIterator$1.SourceValuesIterator = void 0;
    class SourceValuesIterator {
        constructor(initialValues, maxInitialIterations, remainingSkips) {
            this.initialValues = initialValues;
            this.maxInitialIterations = maxInitialIterations;
            this.remainingSkips = remainingSkips;
        }
        [Symbol.iterator]() {
            return this;
        }
        next() {
            if (--this.maxInitialIterations !== -1 && this.remainingSkips >= 0) {
                const n = this.initialValues.next();
                if (!n.done)
                    return {value: n.value(), done: false};
            }
            return {value: undefined, done: true};
        }
        skippedOne() {
            --this.remainingSkips;
            ++this.maxInitialIterations;
        }
    }
    SourceValuesIterator$1.SourceValuesIterator = SourceValuesIterator;

    var Tosser = {};

    var Random$1 = {};

    Object.defineProperty(Random$1, "__esModule", {value: true});
    Random$1.Random = void 0;
    const pure_rand_1$1 = pureRand;
    class Random {
        constructor(sourceRng) {
            this.internalRng = sourceRng.clone();
        }
        clone() {
            return new Random(this.internalRng);
        }
        next(bits) {
            return (0, pure_rand_1$1.unsafeUniformIntDistribution)(
                0, (1 << bits) - 1, this.internalRng);
        }
        nextBoolean() {
            return (0, pure_rand_1$1.unsafeUniformIntDistribution)(0, 1, this.internalRng) == 1;
        }
        nextInt(min, max) {
            return (0, pure_rand_1$1.unsafeUniformIntDistribution)(
                min == null ? Random.MIN_INT : min,
                max == null ? Random.MAX_INT : max,
                this.internalRng);
        }
        nextBigInt(min, max) {
            return (0, pure_rand_1$1.unsafeUniformBigIntDistribution)(min, max, this.internalRng);
        }
        nextArrayInt(min, max) {
            return (0, pure_rand_1$1.unsafeUniformArrayIntDistribution)(min, max, this.internalRng);
        }
        nextDouble() {
            const a = this.next(26);
            const b = this.next(27);
            return (a * Random.DBL_FACTOR + b) * Random.DBL_DIVISOR;
        }
    }
    Random$1.Random = Random;
    Random.MIN_INT = 0x80000000 | 0;
    Random.MAX_INT = 0x7fffffff | 0;
    Random.DBL_FACTOR = Math.pow(2, 27);
    Random.DBL_DIVISOR = Math.pow(2, -53);

    Object.defineProperty(Tosser, "__esModule", {value: true});
    Tosser.toss = void 0;
    const pure_rand_1 = pureRand;
    const Random_1 = Random$1;
    const Value_1$h = Value$1;
    function lazyGenerate(generator, rng, idx) {
        return () => generator.generate(new Random_1.Random(rng), idx);
    }
    function* toss(generator, seed, random, examples) {
        yield* examples.map((e) => () => new Value_1$h.Value(e, undefined));
        let idx = 0;
        let rng = random(seed);
        for (;;) {
            rng = rng.jump ? rng.jump() : (0, pure_rand_1.skipN)(rng, 42);
            yield lazyGenerate(generator, rng, idx++);
        }
    }
    Tosser.toss = toss;

    var PathWalker = {};

    Object.defineProperty(PathWalker, "__esModule", {value: true});
    PathWalker.pathWalk = void 0;
    const Stream_1$g = Stream$1;
    function pathWalk(path, initialValues, shrink) {
        let values = (0, Stream_1$g.stream)(initialValues);
        const segments = path.split(':').map((text) => +text);
        if (segments.length === 0)
            return values;
        if (!segments.every((v) => !Number.isNaN(v))) {
            throw new Error(`Unable to replay, got invalid path=${path}`);
        }
        values = values.drop(segments[0]);
        for (const s of segments.slice(1)) {
            const valueToShrink = values.getNthOrLast(0);
            if (valueToShrink == null) {
                throw new Error(`Unable to replay, got wrong path=${path}`);
            }
            values = shrink(valueToShrink).drop(s);
        }
        return values;
    }
    PathWalker.pathWalk = pathWalk;

    var RunDetailsFormatter = {};

    Object.defineProperty(RunDetailsFormatter, "__esModule", {value: true});
    RunDetailsFormatter.asyncDefaultReportMessage = RunDetailsFormatter.defaultReportMessage =
        RunDetailsFormatter.asyncReportRunDetails = RunDetailsFormatter.reportRunDetails = void 0;
    const stringify_1$6 = stringify;
    const VerbosityLevel_1 = VerbosityLevel;
    const ExecutionStatus_1 = ExecutionStatus;
    function formatHints(hints) {
        if (hints.length === 1) {
            return `Hint: ${hints[0]}`;
        }
        return hints.map((h, idx) => `Hint (${idx + 1}): ${h}`).join('\n');
    }
    function formatFailures(failures, stringifyOne) {
        return `Encountered failures were:\n- ${failures.map(stringifyOne).join('\n- ')}`;
    }
    function formatExecutionSummary(executionTrees, stringifyOne) {
        const summaryLines = [];
        const remainingTreesAndDepth = [];
        for (const tree of executionTrees.slice().reverse()) {
            remainingTreesAndDepth.push({depth: 1, tree});
        }
        while (remainingTreesAndDepth.length !== 0) {
            const currentTreeAndDepth = remainingTreesAndDepth.pop();
            const currentTree = currentTreeAndDepth.tree;
            const currentDepth = currentTreeAndDepth.depth;
            const statusIcon = currentTree.status === ExecutionStatus_1.ExecutionStatus.Success
                ? '\x1b[32m\u221A\x1b[0m'
                : currentTree.status === ExecutionStatus_1.ExecutionStatus.Failure
                    ? '\x1b[31m\xD7\x1b[0m'
                    : '\x1b[33m!\x1b[0m';
            const leftPadding = Array(currentDepth).join('. ');
            summaryLines.push(`${leftPadding}${statusIcon} ${stringifyOne(currentTree.value)}`);
            for (const tree of currentTree.children.slice().reverse()) {
                remainingTreesAndDepth.push({depth: currentDepth + 1, tree});
            }
        }
        return `Execution summary:\n${summaryLines.join('\n')}`;
    }
    function preFormatTooManySkipped(out, stringifyOne) {
        const message =
            `Failed to run property, too many pre-condition failures encountered\n{ seed: ${
                out.seed} }\n\nRan ${out.numRuns} time(s)\nSkipped ${out.numSkips} time(s)`;
        let details = null;
        const hints = [
            'Try to reduce the number of rejected values by combining map, flatMap and built-in arbitraries',
            'Increase failure tolerance by setting maxSkipsPerRun to an higher value',
        ];
        if (out.verbose >= VerbosityLevel_1.VerbosityLevel.VeryVerbose) {
            details = formatExecutionSummary(out.executionSummary, stringifyOne);
        } else {
            hints.push(
                'Enable verbose mode at level VeryVerbose in order to check all generated values and their associated status');
        }
        return {message, details, hints};
    }
    function preFormatFailure(out, stringifyOne) {
        const message = `Property failed after ${out.numRuns} tests\n{ seed: ${out.seed}, path: "${
            out.counterexamplePath}", endOnFailure: true }\nCounterexample: ${
            stringifyOne(
                out.counterexample)}\nShrunk ${out.numShrinks} time(s)\nGot error: ${out.error}`;
        let details = null;
        const hints = [];
        if (out.verbose >= VerbosityLevel_1.VerbosityLevel.VeryVerbose) {
            details = formatExecutionSummary(out.executionSummary, stringifyOne);
        } else if (out.verbose === VerbosityLevel_1.VerbosityLevel.Verbose) {
            details = formatFailures(out.failures, stringifyOne);
        } else {
            hints.push(
                'Enable verbose mode in order to have the list of all failing values encountered during the run');
        }
        return {message, details, hints};
    }
    function preFormatEarlyInterrupted(out, stringifyOne) {
        const message = `Property interrupted after ${out.numRuns} tests\n{ seed: ${out.seed} }`;
        let details = null;
        const hints = [];
        if (out.verbose >= VerbosityLevel_1.VerbosityLevel.VeryVerbose) {
            details = formatExecutionSummary(out.executionSummary, stringifyOne);
        } else {
            hints.push(
                'Enable verbose mode at level VeryVerbose in order to check all generated values and their associated status');
        }
        return {message, details, hints};
    }
    function defaultReportMessageInternal(out, stringifyOne) {
        if (!out.failed)
            return;
        const {message, details, hints} = out.counterexamplePath === null
            ? out.interrupted ? preFormatEarlyInterrupted(out, stringifyOne)
                              : preFormatTooManySkipped(out, stringifyOne)
            : preFormatFailure(out, stringifyOne);
        let errorMessage = message;
        if (details != null)
            errorMessage += `\n\n${details}`;
        if (hints.length > 0)
            errorMessage += `\n\n${formatHints(hints)}`;
        return errorMessage;
    }
    function defaultReportMessage(out) {
        return defaultReportMessageInternal(out, stringify_1$6.stringify);
    }
    RunDetailsFormatter.defaultReportMessage = defaultReportMessage;
    async function asyncDefaultReportMessage(out) {
        const pendingStringifieds = [];
        function stringifyOne(value) {
            const stringified = (0, stringify_1$6.possiblyAsyncStringify)(value);
            if (typeof stringified === 'string') {
                return stringified;
            }
            pendingStringifieds.push(Promise.all([value, stringified]));
            return '\u2026';
        }
        const firstTryMessage = defaultReportMessageInternal(out, stringifyOne);
        if (pendingStringifieds.length === 0) {
            return firstTryMessage;
        }
        const registeredValues = new Map(await Promise.all(pendingStringifieds));
        function stringifySecond(value) {
            const asyncStringifiedIfRegistered = registeredValues.get(value);
            if (asyncStringifiedIfRegistered !== undefined) {
                return asyncStringifiedIfRegistered;
            }
            return (0, stringify_1$6.stringify)(value);
        }
        return defaultReportMessageInternal(out, stringifySecond);
    }
    RunDetailsFormatter.asyncDefaultReportMessage = asyncDefaultReportMessage;
    function throwIfFailed(out) {
        if (!out.failed)
            return;
        throw new Error(defaultReportMessage(out));
    }
    async function asyncThrowIfFailed(out) {
        if (!out.failed)
            return;
        throw new Error(await asyncDefaultReportMessage(out));
    }
    function reportRunDetails(out) {
        if (out.runConfiguration.asyncReporter)
            return out.runConfiguration.asyncReporter(out);
        else if (out.runConfiguration.reporter)
            return out.runConfiguration.reporter(out);
        else
            return throwIfFailed(out);
    }
    RunDetailsFormatter.reportRunDetails = reportRunDetails;
    async function asyncReportRunDetails(out) {
        if (out.runConfiguration.asyncReporter)
            return out.runConfiguration.asyncReporter(out);
        else if (out.runConfiguration.reporter)
            return out.runConfiguration.reporter(out);
        else
            return asyncThrowIfFailed(out);
    }
    RunDetailsFormatter.asyncReportRunDetails = asyncReportRunDetails;

    Object.defineProperty(Runner, "__esModule", {value: true});
    Runner.assert = Runner.check = void 0;
    const Stream_1$f = Stream$1;
    const GlobalParameters_1$1 = GlobalParameters;
    const QualifiedParameters_1$1 = QualifiedParameters$1;
    const DecorateProperty_1 = DecorateProperty;
    const RunnerIterator_1 = RunnerIterator$1;
    const SourceValuesIterator_1 = SourceValuesIterator$1;
    const Tosser_1$1 = Tosser;
    const PathWalker_1$1 = PathWalker;
    const RunDetailsFormatter_1 = RunDetailsFormatter;
    function runIt(property, shrink, sourceValues, verbose, interruptedAsFailure) {
        const runner = new RunnerIterator_1.RunnerIterator(
            sourceValues, shrink, verbose, interruptedAsFailure);
        for (const v of runner) {
            const out = property.run(v);
            runner.handleResult(out);
        }
        return runner.runExecution;
    }
    async function asyncRunIt(property, shrink, sourceValues, verbose, interruptedAsFailure) {
        const runner = new RunnerIterator_1.RunnerIterator(
            sourceValues, shrink, verbose, interruptedAsFailure);
        for (const v of runner) {
            const out = await property.run(v);
            runner.handleResult(out);
        }
        return runner.runExecution;
    }
    function runnerPathWalker(valueProducers, shrink, path) {
        const pathPoints = path.split(':');
        const pathStream = (0, Stream_1$f.stream)(valueProducers)
                               .drop(pathPoints.length > 0 ? +pathPoints[0] : 0)
                               .map((producer) => producer());
        const adaptedPath = ['0', ...pathPoints.slice(1)].join(':');
        return (0, Stream_1$f.stream)((0, PathWalker_1$1.pathWalk)(adaptedPath, pathStream, shrink))
            .map((v) => () => v);
    }
    function buildInitialValues(valueProducers, shrink, qParams) {
        if (qParams.path.length === 0) {
            return (0, Stream_1$f.stream)(valueProducers);
        }
        return runnerPathWalker(valueProducers, shrink, qParams.path);
    }
    function check(rawProperty, params) {
        if (rawProperty == null || rawProperty.generate == null)
            throw new Error('Invalid property encountered, please use a valid property');
        if (rawProperty.run == null)
            throw new Error(
                'Invalid property encountered, please use a valid property not an arbitrary');
        const qParams = QualifiedParameters_1$1.QualifiedParameters.read(Object.assign(
            Object.assign({}, (0, GlobalParameters_1$1.readConfigureGlobal)()), params));
        if (qParams.reporter !== null && qParams.asyncReporter !== null)
            throw new Error(
                'Invalid parameters encountered, reporter and asyncReporter cannot be specified together');
        if (qParams.asyncReporter !== null && !rawProperty.isAsync())
            throw new Error(
                'Invalid parameters encountered, only asyncProperty can be used when asyncReporter specified');
        const property = (0, DecorateProperty_1.decorateProperty)(rawProperty, qParams);
        const generator =
            (0, Tosser_1$1.toss)(property, qParams.seed, qParams.randomType, qParams.examples);
        const maxInitialIterations = qParams.path.indexOf(':') === -1 ? qParams.numRuns : -1;
        const maxSkips = qParams.numRuns * qParams.maxSkipsPerRun;
        const shrink = property.shrink.bind(property);
        const initialValues = buildInitialValues(generator, shrink, qParams);
        const sourceValues = new SourceValuesIterator_1.SourceValuesIterator(
            initialValues, maxInitialIterations, maxSkips);
        const finalShrink = !qParams.endOnFailure ? shrink : Stream_1$f.Stream.nil;
        return property.isAsync()
            ? asyncRunIt(property,
                         finalShrink,
                         sourceValues,
                         qParams.verbose,
                         qParams.markInterruptAsFailure)
                  .then((e) => e.toRunDetails(qParams.seed, qParams.path, maxSkips, qParams))
            : runIt(property,
                    finalShrink,
                    sourceValues,
                    qParams.verbose,
                    qParams.markInterruptAsFailure)
                  .toRunDetails(qParams.seed, qParams.path, maxSkips, qParams);
    }
    Runner.check = check;
    function assert(property, params) {
        const out = check(property, params);
        if (property.isAsync())
            return out.then(RunDetailsFormatter_1.asyncReportRunDetails);
        else
            (0, RunDetailsFormatter_1.reportRunDetails)(out);
    }
    Runner.assert = assert;

    var Sampler = {};

    Object.defineProperty(Sampler, "__esModule", {value: true});
    Sampler.statistics = Sampler.sample = void 0;
    const Stream_1$e = Stream$1;
    const Property_generic_1 = Property_generic;
    const UnbiasedProperty_1 = UnbiasedProperty$1;
    const GlobalParameters_1 = GlobalParameters;
    const QualifiedParameters_1 = QualifiedParameters$1;
    const Tosser_1 = Tosser;
    const PathWalker_1 = PathWalker;
    function toProperty(generator, qParams) {
        const prop = !Object.prototype.hasOwnProperty.call(generator, 'isAsync')
            ? new Property_generic_1.Property(generator, () => true)
            : generator;
        return qParams.unbiased === true ? new UnbiasedProperty_1.UnbiasedProperty(prop) : prop;
    }
    function streamSample(generator, params) {
        const extendedParams = typeof params === 'number'
            ? Object.assign(Object.assign({}, (0, GlobalParameters_1.readConfigureGlobal)()),
                            {numRuns: params})
            : Object.assign(Object.assign({}, (0, GlobalParameters_1.readConfigureGlobal)()),
                            params);
        const qParams = QualifiedParameters_1.QualifiedParameters.read(extendedParams);
        const nextProperty = toProperty(generator, qParams);
        const shrink = nextProperty.shrink.bind(nextProperty);
        const tossedValues = (0, Stream_1$e.stream)(
            (0, Tosser_1.toss)(nextProperty, qParams.seed, qParams.randomType, qParams.examples));
        if (qParams.path.length === 0) {
            return tossedValues.take(qParams.numRuns).map((s) => s().value_);
        }
        return (0, Stream_1$e.stream)(
                   (0, PathWalker_1.pathWalk)(qParams.path, tossedValues.map((s) => s()), shrink))
            .take(qParams.numRuns)
            .map((s) => s.value_);
    }
    function sample(generator, params) {
        return [...streamSample(generator, params)];
    }
    Sampler.sample = sample;
    function round2(n) {
        return (Math.round(n * 100) / 100).toFixed(2);
    }
    function statistics(generator, classify, params) {
        const extendedParams = typeof params === 'number'
            ? Object.assign(Object.assign({}, (0, GlobalParameters_1.readConfigureGlobal)()),
                            {numRuns: params})
            : Object.assign(Object.assign({}, (0, GlobalParameters_1.readConfigureGlobal)()),
                            params);
        const qParams = QualifiedParameters_1.QualifiedParameters.read(extendedParams);
        const recorded = {};
        for (const g of streamSample(generator, params)) {
            const out = classify(g);
            const categories = Array.isArray(out) ? out : [out];
            for (const c of categories) {
                recorded[c] = (recorded[c] || 0) + 1;
            }
        }
        const data = Object.entries(recorded)
                         .sort((a, b) => b[1] - a[1])
                         .map((i) => [i[0], `${round2((i[1] * 100.0) / qParams.numRuns)}%`]);
        const longestName = data.map((i) => i[0].length).reduce((p, c) => Math.max(p, c), 0);
        const longestPercent = data.map((i) => i[1].length).reduce((p, c) => Math.max(p, c), 0);
        for (const item of data) {
            qParams.logger(
                `${item[0].padEnd(longestName, '.')}..${item[1].padStart(longestPercent, '.')}`);
        }
    }
    Sampler.statistics = statistics;

    var array$1 = {};

    var ArrayArbitrary$1 = {};

    var integer$1 = {};

    var IntegerArbitrary$1 = {};

    var BiasNumericRange = {};

    Object.defineProperty(BiasNumericRange, "__esModule", {value: true});
    BiasNumericRange.biasNumericRange = BiasNumericRange.bigIntLogLike =
        BiasNumericRange.integerLogLike = void 0;
    function integerLogLike(v) {
        return Math.floor(Math.log(v) / Math.log(2));
    }
    BiasNumericRange.integerLogLike = integerLogLike;
    function bigIntLogLike(v) {
        if (v === BigInt(0))
            return BigInt(0);
        return BigInt(v.toString().length);
    }
    BiasNumericRange.bigIntLogLike = bigIntLogLike;
    function biasNumericRange(min, max, logLike) {
        if (min === max) {
            return [{min: min, max: max}];
        }
        if (min < 0 && max > 0) {
            const logMin = logLike(-min);
            const logMax = logLike(max);
            return [
                {min: -logMin, max: logMax},
                {min: (max - logMax), max: max},
                {min: min, max: min + logMin},
            ];
        }
        const logGap = logLike((max - min));
        const arbCloseToMin = {min: min, max: min + logGap};
        const arbCloseToMax = {min: (max - logGap), max: max};
        return min < 0 ? [arbCloseToMax, arbCloseToMin] : [arbCloseToMin, arbCloseToMax];
    }
    BiasNumericRange.biasNumericRange = biasNumericRange;

    var ShrinkInteger = {};

    Object.defineProperty(ShrinkInteger, "__esModule", {value: true});
    ShrinkInteger.shrinkInteger = void 0;
    const Value_1$g = Value$1;
    const Stream_1$d = Stream$1;
    function halvePosInteger(n) {
        return Math.floor(n / 2);
    }
    function halveNegInteger(n) {
        return Math.ceil(n / 2);
    }
    function shrinkInteger(current, target, tryTargetAsap) {
        const realGap = current - target;
        function* shrinkDecr() {
            let previous = tryTargetAsap ? undefined : target;
            const gap = tryTargetAsap ? realGap : halvePosInteger(realGap);
            for (let toremove = gap; toremove > 0; toremove = halvePosInteger(toremove)) {
                const next = toremove === realGap ? target : current - toremove;
                yield new Value_1$g.Value(next, previous);
                previous = next;
            }
        }
        function* shrinkIncr() {
            let previous = tryTargetAsap ? undefined : target;
            const gap = tryTargetAsap ? realGap : halveNegInteger(realGap);
            for (let toremove = gap; toremove < 0; toremove = halveNegInteger(toremove)) {
                const next = toremove === realGap ? target : current - toremove;
                yield new Value_1$g.Value(next, previous);
                previous = next;
            }
        }
        return realGap > 0 ? (0, Stream_1$d.stream)(shrinkDecr())
                           : (0, Stream_1$d.stream)(shrinkIncr());
    }
    ShrinkInteger.shrinkInteger = shrinkInteger;

    Object.defineProperty(IntegerArbitrary$1, "__esModule", {value: true});
    IntegerArbitrary$1.IntegerArbitrary = void 0;
    const Arbitrary_1$f = Arbitrary$1;
    const Value_1$f = Value$1;
    const Stream_1$c = Stream$1;
    const BiasNumericRange_1$1 = BiasNumericRange;
    const ShrinkInteger_1 = ShrinkInteger;
    class IntegerArbitrary extends Arbitrary_1$f.Arbitrary {
        constructor(min, max) {
            super();
            this.min = min;
            this.max = max;
        }
        generate(mrng, biasFactor) {
            const range = this.computeGenerateRange(mrng, biasFactor);
            return new Value_1$f.Value(mrng.nextInt(range.min, range.max), undefined);
        }
        canShrinkWithoutContext(value) {
            return (typeof value === 'number' && Number.isInteger(value) && !Object.is(value, -0) &&
                    this.min <= value && value <= this.max);
        }
        shrink(current, context) {
            if (!IntegerArbitrary.isValidContext(current, context)) {
                const target = this.defaultTarget();
                return (0, ShrinkInteger_1.shrinkInteger)(current, target, true);
            }
            if (this.isLastChanceTry(current, context)) {
                return Stream_1$c.Stream.of(new Value_1$f.Value(context, undefined));
            }
            return (0, ShrinkInteger_1.shrinkInteger)(current, context, false);
        }
        defaultTarget() {
            if (this.min <= 0 && this.max >= 0) {
                return 0;
            }
            return this.min < 0 ? this.max : this.min;
        }
        computeGenerateRange(mrng, biasFactor) {
            if (biasFactor === undefined || mrng.nextInt(1, biasFactor) !== 1) {
                return {min: this.min, max: this.max};
            }
            const ranges = (0, BiasNumericRange_1$1.biasNumericRange)(
                this.min, this.max, BiasNumericRange_1$1.integerLogLike);
            if (ranges.length === 1) {
                return ranges[0];
            }
            const id = mrng.nextInt(-2 * (ranges.length - 1), ranges.length - 2);
            return id < 0 ? ranges[0] : ranges[id + 1];
        }
        isLastChanceTry(current, context) {
            if (current > 0)
                return current === context + 1 && current > this.min;
            if (current < 0)
                return current === context - 1 && current < this.max;
            return false;
        }
        static isValidContext(current, context) {
            if (context === undefined) {
                return false;
            }
            if (typeof context !== 'number') {
                throw new Error(`Invalid context type passed to IntegerArbitrary (#1)`);
            }
            if (context !== 0 && Math.sign(current) !== Math.sign(context)) {
                throw new Error(`Invalid context value passed to IntegerArbitrary (#2)`);
            }
            return true;
        }
    }
    IntegerArbitrary$1.IntegerArbitrary = IntegerArbitrary;

    Object.defineProperty(integer$1, "__esModule", {value: true});
    integer$1.integer = void 0;
    const IntegerArbitrary_1$4 = IntegerArbitrary$1;
    function buildCompleteIntegerConstraints(constraints) {
        const min = constraints.min !== undefined ? constraints.min : -0x80000000;
        const max = constraints.max !== undefined ? constraints.max : 0x7fffffff;
        return {min, max};
    }
    function integer(constraints = {}) {
        const fullConstraints = buildCompleteIntegerConstraints(constraints);
        if (fullConstraints.min > fullConstraints.max) {
            throw new Error(
                'fc.integer maximum value should be equal or greater than the minimum one');
        }
        if (!Number.isInteger(fullConstraints.min)) {
            throw new Error('fc.integer minimum value should be an integer');
        }
        if (!Number.isInteger(fullConstraints.max)) {
            throw new Error('fc.integer maximum value should be an integer');
        }
        return new IntegerArbitrary_1$4.IntegerArbitrary(fullConstraints.min, fullConstraints.max);
    }
    integer$1.integer = integer;

    var LazyIterableIterator$1 = {};

    Object.defineProperty(LazyIterableIterator$1, "__esModule", {value: true});
    LazyIterableIterator$1.makeLazy = void 0;
    class LazyIterableIterator {
        constructor(producer) {
            this.producer = producer;
        }
        [Symbol.iterator]() {
            if (this.it === undefined) {
                this.it = this.producer();
            }
            return this.it;
        }
        next() {
            if (this.it === undefined) {
                this.it = this.producer();
            }
            return this.it.next();
        }
    }
    function makeLazy(producer) {
        return new LazyIterableIterator(producer);
    }
    LazyIterableIterator$1.makeLazy = makeLazy;

    var DepthContext = {};

    Object.defineProperty(DepthContext, "__esModule", {value: true});
    DepthContext.createDepthIdentifier = DepthContext.getDepthContextFor = void 0;
    const depthContextCache = new Map();
    function getDepthContextFor(contextMeta) {
        if (contextMeta === undefined) {
            return {depth: 0};
        }
        if (typeof contextMeta !== 'string') {
            return contextMeta;
        }
        const cachedContext = depthContextCache.get(contextMeta);
        if (cachedContext !== undefined) {
            return cachedContext;
        }
        const context = {depth: 0};
        depthContextCache.set(contextMeta, context);
        return context;
    }
    DepthContext.getDepthContextFor = getDepthContextFor;
    function createDepthIdentifier() {
        const identifier = {depth: 0};
        return identifier;
    }
    DepthContext.createDepthIdentifier = createDepthIdentifier;

    var BuildSlicedGenerator = {};

    var NoopSlicedGenerator$1 = {};

    Object.defineProperty(NoopSlicedGenerator$1, "__esModule", {value: true});
    NoopSlicedGenerator$1.NoopSlicedGenerator = void 0;
    class NoopSlicedGenerator {
        constructor(arb, mrng, biasFactor) {
            this.arb = arb;
            this.mrng = mrng;
            this.biasFactor = biasFactor;
        }
        attemptExact() {
            return;
        }
        next() {
            return this.arb.generate(this.mrng, this.biasFactor);
        }
    }
    NoopSlicedGenerator$1.NoopSlicedGenerator = NoopSlicedGenerator;

    var SlicedBasedGenerator$1 = {};

    Object.defineProperty(SlicedBasedGenerator$1, "__esModule", {value: true});
    SlicedBasedGenerator$1.SlicedBasedGenerator = void 0;
    const Value_1$e = Value$1;
    class SlicedBasedGenerator {
        constructor(arb, mrng, slices, biasFactor) {
            this.arb = arb;
            this.mrng = mrng;
            this.slices = slices;
            this.biasFactor = biasFactor;
            this.activeSliceIndex = 0;
            this.nextIndexInSlice = 0;
            this.lastIndexInSlice = -1;
        }
        attemptExact(targetLength) {
            if (targetLength !== 0 && this.mrng.nextInt(1, this.biasFactor) === 1) {
                const eligibleIndices = [];
                for (let index = 0; index !== this.slices.length; ++index) {
                    const slice = this.slices[index];
                    if (slice.length === targetLength) {
                        eligibleIndices.push(index);
                    }
                }
                if (eligibleIndices.length === 0) {
                    return;
                }
                this.activeSliceIndex =
                    eligibleIndices[this.mrng.nextInt(0, eligibleIndices.length - 1)];
                this.nextIndexInSlice = 0;
                this.lastIndexInSlice = targetLength - 1;
            }
        }
        next() {
            if (this.nextIndexInSlice <= this.lastIndexInSlice) {
                return new Value_1$e.Value(
                    this.slices[this.activeSliceIndex][this.nextIndexInSlice++], undefined);
            }
            if (this.mrng.nextInt(1, this.biasFactor) !== 1) {
                return this.arb.generate(this.mrng, this.biasFactor);
            }
            this.activeSliceIndex = this.mrng.nextInt(0, this.slices.length - 1);
            const slice = this.slices[this.activeSliceIndex];
            if (this.mrng.nextInt(1, this.biasFactor) !== 1) {
                this.nextIndexInSlice = 1;
                this.lastIndexInSlice = slice.length - 1;
                return new Value_1$e.Value(slice[0], undefined);
            }
            const rangeBoundaryA = this.mrng.nextInt(0, slice.length - 1);
            const rangeBoundaryB = this.mrng.nextInt(0, slice.length - 1);
            this.nextIndexInSlice = Math.min(rangeBoundaryA, rangeBoundaryB);
            this.lastIndexInSlice = Math.max(rangeBoundaryA, rangeBoundaryB);
            return new Value_1$e.Value(slice[this.nextIndexInSlice++], undefined);
        }
    }
    SlicedBasedGenerator$1.SlicedBasedGenerator = SlicedBasedGenerator;

    Object.defineProperty(BuildSlicedGenerator, "__esModule", {value: true});
    BuildSlicedGenerator.buildSlicedGenerator = void 0;
    const NoopSlicedGenerator_1 = NoopSlicedGenerator$1;
    const SlicedBasedGenerator_1 = SlicedBasedGenerator$1;
    function buildSlicedGenerator(arb, mrng, slices, biasFactor) {
        if (biasFactor === undefined || slices.length === 0 || mrng.nextInt(1, biasFactor) !== 1) {
            return new NoopSlicedGenerator_1.NoopSlicedGenerator(arb, mrng, biasFactor);
        }
        return new SlicedBasedGenerator_1.SlicedBasedGenerator(arb, mrng, slices, biasFactor);
    }
    BuildSlicedGenerator.buildSlicedGenerator = buildSlicedGenerator;

    Object.defineProperty(ArrayArbitrary$1, "__esModule", {value: true});
    ArrayArbitrary$1.ArrayArbitrary = void 0;
    const Stream_1$b = Stream$1;
    const symbols_1$9 = symbols;
    const integer_1$f = integer$1;
    const LazyIterableIterator_1$3 = LazyIterableIterator$1;
    const Arbitrary_1$e = Arbitrary$1;
    const Value_1$d = Value$1;
    const DepthContext_1$2 = DepthContext;
    const BuildSlicedGenerator_1 = BuildSlicedGenerator;
    function biasedMaxLength(minLength, maxLength) {
        if (minLength === maxLength) {
            return minLength;
        }
        return minLength + Math.floor(Math.log(maxLength - minLength) / Math.log(2));
    }
    class ArrayArbitrary extends Arbitrary_1$e.Arbitrary {
        constructor(arb,
                    minLength,
                    maxGeneratedLength,
                    maxLength,
                    depthIdentifier,
                    setBuilder,
                    customSlices) {
            super();
            this.arb = arb;
            this.minLength = minLength;
            this.maxGeneratedLength = maxGeneratedLength;
            this.maxLength = maxLength;
            this.setBuilder = setBuilder;
            this.customSlices = customSlices;
            this.lengthArb = (0, integer_1$f.integer)({min: minLength, max: maxGeneratedLength});
            this.depthContext = (0, DepthContext_1$2.getDepthContextFor)(depthIdentifier);
        }
        preFilter(tab) {
            if (this.setBuilder === undefined) {
                return tab;
            }
            const s = this.setBuilder();
            for (let index = 0; index !== tab.length; ++index) {
                s.tryAdd(tab[index]);
            }
            return s.getData();
        }
        static makeItCloneable(vs, shrinkables) {
            vs[symbols_1$9.cloneMethod] = () => {
                const cloned = [];
                for (let idx = 0; idx !== shrinkables.length; ++idx) {
                    cloned.push(shrinkables[idx].value);
                }
                this.makeItCloneable(cloned, shrinkables);
                return cloned;
            };
            return vs;
        }
        generateNItemsNoDuplicates(setBuilder, N, mrng, biasFactorItems) {
            let numSkippedInRow = 0;
            const s = setBuilder();
            const slicedGenerator = (0, BuildSlicedGenerator_1.buildSlicedGenerator)(
                this.arb, mrng, this.customSlices, biasFactorItems);
            while (s.size() < N && numSkippedInRow < this.maxGeneratedLength) {
                const current = slicedGenerator.next();
                if (s.tryAdd(current)) {
                    numSkippedInRow = 0;
                } else {
                    numSkippedInRow += 1;
                }
            }
            return s.getData();
        }
        safeGenerateNItemsNoDuplicates(setBuilder, N, mrng, biasFactorItems) {
            const depthImpact =
                Math.max(0, N - biasedMaxLength(this.minLength, this.maxGeneratedLength));
            this.depthContext.depth += depthImpact;
            try {
                return this.generateNItemsNoDuplicates(setBuilder, N, mrng, biasFactorItems);
            } finally {
                this.depthContext.depth -= depthImpact;
            }
        }
        generateNItems(N, mrng, biasFactorItems) {
            const items = [];
            const slicedGenerator = (0, BuildSlicedGenerator_1.buildSlicedGenerator)(
                this.arb, mrng, this.customSlices, biasFactorItems);
            slicedGenerator.attemptExact(N);
            for (let index = 0; index !== N; ++index) {
                const current = slicedGenerator.next();
                items.push(current);
            }
            return items;
        }
        safeGenerateNItems(N, mrng, biasFactorItems) {
            const depthImpact =
                Math.max(0, N - biasedMaxLength(this.minLength, this.maxGeneratedLength));
            this.depthContext.depth += depthImpact;
            try {
                return this.generateNItems(N, mrng, biasFactorItems);
            } finally {
                this.depthContext.depth -= depthImpact;
            }
        }
        wrapper(itemsRaw, shrunkOnce, itemsRawLengthContext, startIndex) {
            const items = shrunkOnce ? this.preFilter(itemsRaw) : itemsRaw;
            let cloneable = false;
            const vs = [];
            const itemsContexts = [];
            for (let idx = 0; idx !== items.length; ++idx) {
                const s = items[idx];
                cloneable = cloneable || s.hasToBeCloned;
                vs.push(s.value);
                itemsContexts.push(s.context);
            }
            if (cloneable) {
                ArrayArbitrary.makeItCloneable(vs, items);
            }
            const context = {
                shrunkOnce,
                lengthContext:
                    itemsRaw.length === items.length && itemsRawLengthContext !== undefined
                    ? itemsRawLengthContext
                    : undefined,
                itemsContexts,
                startIndex,
            };
            return new Value_1$d.Value(vs, context);
        }
        generate(mrng, biasFactor) {
            const biasMeta = this.applyBias(mrng, biasFactor);
            const targetSize = biasMeta.size;
            const items = this.setBuilder !== undefined
                ? this.safeGenerateNItemsNoDuplicates(
                      this.setBuilder, targetSize, mrng, biasMeta.biasFactorItems)
                : this.safeGenerateNItems(targetSize, mrng, biasMeta.biasFactorItems);
            return this.wrapper(items, false, undefined, 0);
        }
        applyBias(mrng, biasFactor) {
            if (biasFactor === undefined) {
                return {size: this.lengthArb.generate(mrng, undefined).value};
            }
            if (this.minLength === this.maxGeneratedLength) {
                return {
                    size: this.lengthArb.generate(mrng, undefined).value,
                    biasFactorItems: biasFactor
                };
            }
            if (mrng.nextInt(1, biasFactor) !== 1) {
                return {size: this.lengthArb.generate(mrng, undefined).value};
            }
            if (mrng.nextInt(1, biasFactor) !== 1 || this.minLength === this.maxGeneratedLength) {
                return {
                    size: this.lengthArb.generate(mrng, undefined).value,
                    biasFactorItems: biasFactor
                };
            }
            const maxBiasedLength = biasedMaxLength(this.minLength, this.maxGeneratedLength);
            const targetSizeValue = (0, integer_1$f.integer)({
                                        min: this.minLength,
                                        max: maxBiasedLength
                                    }).generate(mrng, undefined);
            return {size: targetSizeValue.value, biasFactorItems: biasFactor};
        }
        canShrinkWithoutContext(value) {
            if (!Array.isArray(value) || this.minLength > value.length ||
                value.length > this.maxLength) {
                return false;
            }
            for (let index = 0; index !== value.length; ++index) {
                if (!(index in value)) {
                    return false;
                }
                if (!this.arb.canShrinkWithoutContext(value[index])) {
                    return false;
                }
            }
            const filtered =
                this.preFilter(value.map((item) => new Value_1$d.Value(item, undefined)));
            return filtered.length === value.length;
        }
        shrinkItemByItem(value, safeContext, endIndex) {
            let shrinks = Stream_1$b.Stream.nil();
            for (let index = safeContext.startIndex; index < endIndex; ++index) {
                shrinks = shrinks.join((0, LazyIterableIterator_1$3.makeLazy)(
                    () =>
                        this.arb.shrink(value[index], safeContext.itemsContexts[index]).map((v) => {
                            const beforeCurrent = value.slice(0, index).map(
                                (v, i) => new Value_1$d.Value((0, symbols_1$9.cloneIfNeeded)(v),
                                                              safeContext.itemsContexts[i]));
                            const afterCurrent = value.slice(index + 1).map(
                                (v, i) =>
                                    new Value_1$d.Value((0, symbols_1$9.cloneIfNeeded)(v),
                                                        safeContext.itemsContexts[i + index + 1]));
                            return [
                                beforeCurrent.concat(v).concat(afterCurrent),
                                undefined,
                                index,
                            ];
                        })));
            }
            return shrinks;
        }
        shrinkImpl(value, context) {
            if (value.length === 0) {
                return Stream_1$b.Stream.nil();
            }
            const safeContext = context !== undefined
                ? context
                : {shrunkOnce: false, lengthContext: undefined, itemsContexts: [], startIndex: 0};
            return (this.lengthArb.shrink(value.length, safeContext.lengthContext)
                        .drop(safeContext.shrunkOnce && safeContext.lengthContext === undefined &&
                                      value.length > this.minLength + 1
                                  ? 1
                                  : 0)
                        .map((lengthValue) => {
                            const sliceStart = value.length - lengthValue.value;
                            return [
                                value.slice(sliceStart)
                                    .map((v, index) => new Value_1$d.Value(
                                             (0, symbols_1$9.cloneIfNeeded)(v),
                                             safeContext.itemsContexts[index + sliceStart])),
                                lengthValue.context,
                                0,
                            ];
                        })
                        .join((0, LazyIterableIterator_1$3.makeLazy)(
                            () => value.length > this.minLength
                                ? this.shrinkItemByItem(value, safeContext, 1)
                                : this.shrinkItemByItem(value, safeContext, value.length)))
                        .join(value.length > this.minLength
                                  ? (0, LazyIterableIterator_1$3.makeLazy)(() => {
                                        const subContext = {
                                            shrunkOnce: false,
                                            lengthContext: undefined,
                                            itemsContexts: safeContext.itemsContexts.slice(1),
                                            startIndex: 0,
                                        };
                                        return this.shrinkImpl(value.slice(1), subContext)
                                            .filter((v) => this.minLength <= v[0].length + 1)
                                            .map((v) => {
                                                return [
                                                    [new Value_1$d.Value(
                                                         (0, symbols_1$9.cloneIfNeeded)(value[0]),
                                                         safeContext.itemsContexts[0])]
                                                        .concat(v[0]),
                                                    undefined,
                                                    0,
                                                ];
                                            });
                                    })
                                  : Stream_1$b.Stream.nil()));
        }
        shrink(value, context) {
            return this.shrinkImpl(value, context)
                .map((contextualValue) => this.wrapper(
                         contextualValue[0], true, contextualValue[1], contextualValue[2]));
        }
    }
    ArrayArbitrary$1.ArrayArbitrary = ArrayArbitrary;

    var MaxLengthFromMinLength = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.resolveSize = exports.depthBiasFromSizeForArbitrary =
            exports.maxGeneratedLengthFromSizeForArbitrary = exports.relativeSizeToSize =
                exports.maxLengthFromMinLength = exports.DefaultSize = exports.MaxLengthUpperBound =
                    void 0;
        const GlobalParameters_1 = GlobalParameters;
        exports.MaxLengthUpperBound = 0x7fffffff;
        const orderedSize = ['xsmall', 'small', 'medium', 'large', 'xlarge'];
        const orderedRelativeSize = ['-4', '-3', '-2', '-1', '=', '+1', '+2', '+3', '+4'];
        exports.DefaultSize = 'small';
        function maxLengthFromMinLength(minLength, size) {
            switch (size) {
                case 'xsmall':
                    return Math.floor(1.1 * minLength) + 1;
                case 'small':
                    return 2 * minLength + 10;
                case 'medium':
                    return 11 * minLength + 100;
                case 'large':
                    return 101 * minLength + 1000;
                case 'xlarge':
                    return 1001 * minLength + 10000;
                default:
                    throw new Error(`Unable to compute lengths based on received size: ${size}`);
            }
        }
        exports.maxLengthFromMinLength = maxLengthFromMinLength;
        function relativeSizeToSize(size, defaultSize) {
            const sizeInRelative = orderedRelativeSize.indexOf(size);
            if (sizeInRelative === -1) {
                return size;
            }
            const defaultSizeInSize = orderedSize.indexOf(defaultSize);
            if (defaultSizeInSize === -1) {
                throw new Error(
                    `Unable to offset size based on the unknown defaulted one: ${defaultSize}`);
            }
            const resultingSizeInSize = defaultSizeInSize + sizeInRelative - 4;
            return resultingSizeInSize < 0
                ? orderedSize[0]
                : resultingSizeInSize >= orderedSize.length ? orderedSize[orderedSize.length - 1]
                                                            : orderedSize[resultingSizeInSize];
        }
        exports.relativeSizeToSize = relativeSizeToSize;
        function maxGeneratedLengthFromSizeForArbitrary(
            size, minLength, maxLength, specifiedMaxLength) {
            const {baseSize: defaultSize = exports.DefaultSize, defaultSizeToMaxWhenMaxSpecified} =
                (0, GlobalParameters_1.readConfigureGlobal)() || {};
            const definedSize = size !== undefined
                ? size
                : specifiedMaxLength && defaultSizeToMaxWhenMaxSpecified ? 'max' : defaultSize;
            if (definedSize === 'max') {
                return maxLength;
            }
            const finalSize = relativeSizeToSize(definedSize, defaultSize);
            return Math.min(maxLengthFromMinLength(minLength, finalSize), maxLength);
        }
        exports.maxGeneratedLengthFromSizeForArbitrary = maxGeneratedLengthFromSizeForArbitrary;
        function depthBiasFromSizeForArbitrary(depthSizeOrSize, specifiedMaxDepth) {
            if (typeof depthSizeOrSize === 'number') {
                return 1 / depthSizeOrSize;
            }
            const {baseSize: defaultSize = exports.DefaultSize, defaultSizeToMaxWhenMaxSpecified} =
                (0, GlobalParameters_1.readConfigureGlobal)() || {};
            const definedSize = depthSizeOrSize !== undefined
                ? depthSizeOrSize
                : specifiedMaxDepth && defaultSizeToMaxWhenMaxSpecified ? 'max' : defaultSize;
            if (definedSize === 'max') {
                return 0;
            }
            const finalSize = relativeSizeToSize(definedSize, defaultSize);
            switch (finalSize) {
                case 'xsmall':
                    return 1;
                case 'small':
                    return 0.5;
                case 'medium':
                    return 0.25;
                case 'large':
                    return 0.125;
                case 'xlarge':
                    return 0.0625;
            }
        }
        exports.depthBiasFromSizeForArbitrary = depthBiasFromSizeForArbitrary;
        function resolveSize(size) {
            const {baseSize: defaultSize = exports.DefaultSize} =
                (0, GlobalParameters_1.readConfigureGlobal)() || {};
            if (size === undefined) {
                return defaultSize;
            }
            return relativeSizeToSize(size, defaultSize);
        }
        exports.resolveSize = resolveSize;
    }(MaxLengthFromMinLength));

    Object.defineProperty(array$1, "__esModule", {value: true});
    array$1.array = void 0;
    const ArrayArbitrary_1$1 = ArrayArbitrary$1;
    const MaxLengthFromMinLength_1$8 = MaxLengthFromMinLength;
    function array(arb, constraints = {}) {
        const size = constraints.size;
        const minLength = constraints.minLength || 0;
        const maxLengthOrUnset = constraints.maxLength;
        const depthIdentifier = constraints.depthIdentifier;
        const maxLength = maxLengthOrUnset !== undefined
            ? maxLengthOrUnset
            : MaxLengthFromMinLength_1$8.MaxLengthUpperBound;
        const specifiedMaxLength = maxLengthOrUnset !== undefined;
        const maxGeneratedLength =
            (0, MaxLengthFromMinLength_1$8.maxGeneratedLengthFromSizeForArbitrary)(
                size, minLength, maxLength, specifiedMaxLength);
        const customSlices = constraints.experimentalCustomSlices || [];
        return new ArrayArbitrary_1$1.ArrayArbitrary(arb,
                                                     minLength,
                                                     maxGeneratedLength,
                                                     maxLength,
                                                     depthIdentifier,
                                                     undefined,
                                                     customSlices);
    }
    array$1.array = array;

    var bigInt$1 = {};

    var BigIntArbitrary$1 = {};

    var ShrinkBigInt = {};

    Object.defineProperty(ShrinkBigInt, "__esModule", {value: true});
    ShrinkBigInt.shrinkBigInt = void 0;
    const Stream_1$a = Stream$1;
    const Value_1$c = Value$1;
    function halveBigInt(n) {
        return n / BigInt(2);
    }
    function shrinkBigInt(current, target, tryTargetAsap) {
        const realGap = current - target;
        function* shrinkDecr() {
            let previous = tryTargetAsap ? undefined : target;
            const gap = tryTargetAsap ? realGap : halveBigInt(realGap);
            for (let toremove = gap; toremove > 0; toremove = halveBigInt(toremove)) {
                const next = current - toremove;
                yield new Value_1$c.Value(next, previous);
                previous = next;
            }
        }
        function* shrinkIncr() {
            let previous = tryTargetAsap ? undefined : target;
            const gap = tryTargetAsap ? realGap : halveBigInt(realGap);
            for (let toremove = gap; toremove < 0; toremove = halveBigInt(toremove)) {
                const next = current - toremove;
                yield new Value_1$c.Value(next, previous);
                previous = next;
            }
        }
        return realGap > 0 ? (0, Stream_1$a.stream)(shrinkDecr())
                           : (0, Stream_1$a.stream)(shrinkIncr());
    }
    ShrinkBigInt.shrinkBigInt = shrinkBigInt;

    Object.defineProperty(BigIntArbitrary$1, "__esModule", {value: true});
    BigIntArbitrary$1.BigIntArbitrary = void 0;
    const Stream_1$9 = Stream$1;
    const Arbitrary_1$d = Arbitrary$1;
    const Value_1$b = Value$1;
    const BiasNumericRange_1 = BiasNumericRange;
    const ShrinkBigInt_1 = ShrinkBigInt;
    class BigIntArbitrary extends Arbitrary_1$d.Arbitrary {
        constructor(min, max) {
            super();
            this.min = min;
            this.max = max;
        }
        generate(mrng, biasFactor) {
            const range = this.computeGenerateRange(mrng, biasFactor);
            return new Value_1$b.Value(mrng.nextBigInt(range.min, range.max), undefined);
        }
        computeGenerateRange(mrng, biasFactor) {
            if (biasFactor === undefined || mrng.nextInt(1, biasFactor) !== 1) {
                return {min: this.min, max: this.max};
            }
            const ranges = (0, BiasNumericRange_1.biasNumericRange)(
                this.min, this.max, BiasNumericRange_1.bigIntLogLike);
            if (ranges.length === 1) {
                return ranges[0];
            }
            const id = mrng.nextInt(-2 * (ranges.length - 1), ranges.length - 2);
            return id < 0 ? ranges[0] : ranges[id + 1];
        }
        canShrinkWithoutContext(value) {
            return typeof value === 'bigint' && this.min <= value && value <= this.max;
        }
        shrink(current, context) {
            if (!BigIntArbitrary.isValidContext(current, context)) {
                const target = this.defaultTarget();
                return (0, ShrinkBigInt_1.shrinkBigInt)(current, target, true);
            }
            if (this.isLastChanceTry(current, context)) {
                return Stream_1$9.Stream.of(new Value_1$b.Value(context, undefined));
            }
            return (0, ShrinkBigInt_1.shrinkBigInt)(current, context, false);
        }
        defaultTarget() {
            if (this.min <= 0 && this.max >= 0) {
                return BigInt(0);
            }
            return this.min < 0 ? this.max : this.min;
        }
        isLastChanceTry(current, context) {
            if (current > 0)
                return current === context + BigInt(1) && current > this.min;
            if (current < 0)
                return current === context - BigInt(1) && current < this.max;
            return false;
        }
        static isValidContext(current, context) {
            if (context === undefined) {
                return false;
            }
            if (typeof context !== 'bigint') {
                throw new Error(`Invalid context type passed to BigIntArbitrary (#1)`);
            }
            const differentSigns = (current > 0 && context < 0) || (current < 0 && context > 0);
            if (context !== BigInt(0) && differentSigns) {
                throw new Error(`Invalid context value passed to BigIntArbitrary (#2)`);
            }
            return true;
        }
    }
    BigIntArbitrary$1.BigIntArbitrary = BigIntArbitrary;

    Object.defineProperty(bigInt$1, "__esModule", {value: true});
    bigInt$1.bigInt = void 0;
    const BigIntArbitrary_1$3 = BigIntArbitrary$1;
    function buildCompleteBigIntConstraints(constraints) {
        const DefaultPow = 256;
        const DefaultMin = BigInt(-1) << BigInt(DefaultPow - 1);
        const DefaultMax = (BigInt(1) << BigInt(DefaultPow - 1)) - BigInt(1);
        const min = constraints.min;
        const max = constraints.max;
        return {
            min: min !== undefined
                ? min
                : DefaultMin - (max !== undefined && max < BigInt(0) ? max * max : BigInt(0)),
            max: max !== undefined
                ? max
                : DefaultMax + (min !== undefined && min > BigInt(0) ? min * min : BigInt(0)),
        };
    }
    function extractBigIntConstraints(args) {
        if (args[0] === undefined) {
            return {};
        }
        if (args[1] === undefined) {
            const constraints = args[0];
            return constraints;
        }
        return {min: args[0], max: args[1]};
    }
    function bigInt(...args) {
        const constraints = buildCompleteBigIntConstraints(extractBigIntConstraints(args));
        if (constraints.min > constraints.max) {
            throw new Error('fc.bigInt expects max to be greater than or equal to min');
        }
        return new BigIntArbitrary_1$3.BigIntArbitrary(constraints.min, constraints.max);
    }
    bigInt$1.bigInt = bigInt;

    var bigIntN$1 = {};

    Object.defineProperty(bigIntN$1, "__esModule", {value: true});
    bigIntN$1.bigIntN = void 0;
    const BigIntArbitrary_1$2 = BigIntArbitrary$1;
    function bigIntN(n) {
        if (n < 1) {
            throw new Error(
                'fc.bigIntN expects requested number of bits to be superior or equal to 1');
        }
        const min = BigInt(-1) << BigInt(n - 1);
        const max = (BigInt(1) << BigInt(n - 1)) - BigInt(1);
        return new BigIntArbitrary_1$2.BigIntArbitrary(min, max);
    }
    bigIntN$1.bigIntN = bigIntN;

    var bigUint$1 = {};

    Object.defineProperty(bigUint$1, "__esModule", {value: true});
    bigUint$1.bigUint = void 0;
    const BigIntArbitrary_1$1 = BigIntArbitrary$1;
    function computeDefaultMax() {
        return (BigInt(1) << BigInt(256)) - BigInt(1);
    }
    function bigUint(constraints) {
        const requestedMax = typeof constraints === 'object' ? constraints.max : constraints;
        const max = requestedMax !== undefined ? requestedMax : computeDefaultMax();
        if (max < 0) {
            throw new Error('fc.bigUint expects max to be greater than or equal to zero');
        }
        return new BigIntArbitrary_1$1.BigIntArbitrary(BigInt(0), max);
    }
    bigUint$1.bigUint = bigUint;

    var bigUintN$1 = {};

    Object.defineProperty(bigUintN$1, "__esModule", {value: true});
    bigUintN$1.bigUintN = void 0;
    const BigIntArbitrary_1 = BigIntArbitrary$1;
    function bigUintN(n) {
        if (n < 0) {
            throw new Error(
                'fc.bigUintN expects requested number of bits to be superior or equal to 0');
        }
        const min = BigInt(0);
        const max = (BigInt(1) << BigInt(n)) - BigInt(1);
        return new BigIntArbitrary_1.BigIntArbitrary(min, max);
    }
    bigUintN$1.bigUintN = bigUintN;

    var boolean$1 = {};

    Object.defineProperty(boolean$1, "__esModule", {value: true});
    boolean$1.boolean = void 0;
    const integer_1$e = integer$1;
    function booleanMapper(v) {
        return v === 1;
    }
    function booleanUnmapper(v) {
        if (typeof v !== 'boolean')
            throw new Error('Unsupported input type');
        return v === true ? 1 : 0;
    }
    function boolean() {
        return (0, integer_1$e.integer)({min: 0, max: 1})
            .map(booleanMapper, booleanUnmapper)
            .noBias();
    }
    boolean$1.boolean = boolean;

    var falsy$1 = {};

    var constantFrom$1 = {};

    var ConstantArbitrary$1 = {};

    Object.defineProperty(ConstantArbitrary$1, "__esModule", {value: true});
    ConstantArbitrary$1.ConstantArbitrary = void 0;
    const Stream_1$8 = Stream$1;
    const Arbitrary_1$c = Arbitrary$1;
    const Value_1$a = Value$1;
    const symbols_1$8 = symbols;
    class ConstantArbitrary extends Arbitrary_1$c.Arbitrary {
        constructor(values) {
            super();
            this.values = values;
        }
        generate(mrng, _biasFactor) {
            const idx = this.values.length === 1 ? 0 : mrng.nextInt(0, this.values.length - 1);
            const value = this.values[idx];
            if (!(0, symbols_1$8.hasCloneMethod)(value)) {
                return new Value_1$a.Value(value, idx);
            }
            return new Value_1$a.Value(value, idx, () => value[symbols_1$8.cloneMethod]());
        }
        canShrinkWithoutContext(value) {
            for (let idx = 0; idx !== this.values.length; ++idx) {
                if (Object.is(this.values[idx], value)) {
                    return true;
                }
            }
            return false;
        }
        shrink(value, context) {
            if (context === 0 || Object.is(value, this.values[0])) {
                return Stream_1$8.Stream.nil();
            }
            return Stream_1$8.Stream.of(new Value_1$a.Value(this.values[0], 0));
        }
    }
    ConstantArbitrary$1.ConstantArbitrary = ConstantArbitrary;

    Object.defineProperty(constantFrom$1, "__esModule", {value: true});
    constantFrom$1.constantFrom = void 0;
    const ConstantArbitrary_1$1 = ConstantArbitrary$1;
    function constantFrom(...values) {
        if (values.length === 0) {
            throw new Error('fc.constantFrom expects at least one parameter');
        }
        return new ConstantArbitrary_1$1.ConstantArbitrary(values);
    }
    constantFrom$1.constantFrom = constantFrom;

    Object.defineProperty(falsy$1, "__esModule", {value: true});
    falsy$1.falsy = void 0;
    const constantFrom_1$2 = constantFrom$1;
    function falsy(constraints) {
        if (!constraints || !constraints.withBigInt) {
            return (0, constantFrom_1$2.constantFrom)(false, null, undefined, 0, '', NaN);
        }
        return (0, constantFrom_1$2.constantFrom)(false, null, undefined, 0, '', NaN, BigInt(0));
    }
    falsy$1.falsy = falsy;

    var ascii$1 = {};

    var CharacterArbitraryBuilder = {};

    var IndexToCharString = {};

    Object.defineProperty(IndexToCharString, "__esModule", {value: true});
    IndexToCharString.indexToCharStringUnmapper = IndexToCharString.indexToCharStringMapper =
        void 0;
    IndexToCharString.indexToCharStringMapper = String.fromCodePoint;
    function indexToCharStringUnmapper(c) {
        if (typeof c !== 'string') {
            throw new Error('Cannot unmap non-string');
        }
        if (c.length === 0 || c.length > 2) {
            throw new Error('Cannot unmap string with more or less than one character');
        }
        const c1 = c.charCodeAt(0);
        if (c.length === 1) {
            return c1;
        }
        const c2 = c.charCodeAt(1);
        if (c1 < 0xd800 || c1 > 0xdbff || c2 < 0xdc00 || c2 > 0xdfff) {
            throw new Error('Cannot unmap invalid surrogate pairs');
        }
        return c.codePointAt(0);
    }
    IndexToCharString.indexToCharStringUnmapper = indexToCharStringUnmapper;

    Object.defineProperty(CharacterArbitraryBuilder, "__esModule", {value: true});
    CharacterArbitraryBuilder.buildCharacterArbitrary = void 0;
    const integer_1$d = integer$1;
    const IndexToCharString_1 = IndexToCharString;
    function buildCharacterArbitrary(min, max, mapToCode, unmapFromCode) {
        return (0, integer_1$d.integer)({min, max})
            .map((n) => (0, IndexToCharString_1.indexToCharStringMapper)(mapToCode(n)),
                 (c) => unmapFromCode((0, IndexToCharString_1.indexToCharStringUnmapper)(c)));
    }
    CharacterArbitraryBuilder.buildCharacterArbitrary = buildCharacterArbitrary;

    var IndexToPrintableIndex = {};

    Object.defineProperty(IndexToPrintableIndex, "__esModule", {value: true});
    IndexToPrintableIndex.indexToPrintableIndexUnmapper =
        IndexToPrintableIndex.indexToPrintableIndexMapper = void 0;
    function indexToPrintableIndexMapper(v) {
        if (v < 95)
            return v + 0x20;
        if (v <= 0x7e)
            return v - 95;
        return v;
    }
    IndexToPrintableIndex.indexToPrintableIndexMapper = indexToPrintableIndexMapper;
    function indexToPrintableIndexUnmapper(v) {
        if (v >= 0x20 && v <= 0x7e)
            return v - 0x20;
        if (v >= 0 && v <= 0x1f)
            return v + 95;
        return v;
    }
    IndexToPrintableIndex.indexToPrintableIndexUnmapper = indexToPrintableIndexUnmapper;

    Object.defineProperty(ascii$1, "__esModule", {value: true});
    ascii$1.ascii = void 0;
    const CharacterArbitraryBuilder_1$6 = CharacterArbitraryBuilder;
    const IndexToPrintableIndex_1$3 = IndexToPrintableIndex;
    function ascii() {
        return (0, CharacterArbitraryBuilder_1$6.buildCharacterArbitrary)(
            0x00,
            0x7f,
            IndexToPrintableIndex_1$3.indexToPrintableIndexMapper,
            IndexToPrintableIndex_1$3.indexToPrintableIndexUnmapper);
    }
    ascii$1.ascii = ascii;

    var base64$1 = {};

    Object.defineProperty(base64$1, "__esModule", {value: true});
    base64$1.base64 = void 0;
    const CharacterArbitraryBuilder_1$5 = CharacterArbitraryBuilder;
    function base64Mapper(v) {
        if (v < 26)
            return v + 65;
        if (v < 52)
            return v + 97 - 26;
        if (v < 62)
            return v + 48 - 52;
        return v === 62 ? 43 : 47;
    }
    function base64Unmapper(v) {
        if (v >= 65 && v <= 90)
            return v - 65;
        if (v >= 97 && v <= 122)
            return v - 97 + 26;
        if (v >= 48 && v <= 57)
            return v - 48 + 52;
        return v === 43 ? 62 : v === 47 ? 63 : -1;
    }
    function base64() {
        return (0, CharacterArbitraryBuilder_1$5.buildCharacterArbitrary)(
            0, 63, base64Mapper, base64Unmapper);
    }
    base64$1.base64 = base64;

    var char$1 = {};

    Object.defineProperty(char$1, "__esModule", {value: true});
    char$1.char = void 0;
    const CharacterArbitraryBuilder_1$4 = CharacterArbitraryBuilder;
    function identity(v) {
        return v;
    }
    function char() {
        return (0, CharacterArbitraryBuilder_1$4.buildCharacterArbitrary)(
            0x20, 0x7e, identity, identity);
    }
    char$1.char = char;

    var char16bits$1 = {};

    Object.defineProperty(char16bits$1, "__esModule", {value: true});
    char16bits$1.char16bits = void 0;
    const CharacterArbitraryBuilder_1$3 = CharacterArbitraryBuilder;
    const IndexToPrintableIndex_1$2 = IndexToPrintableIndex;
    function char16bits() {
        return (0, CharacterArbitraryBuilder_1$3.buildCharacterArbitrary)(
            0x0000,
            0xffff,
            IndexToPrintableIndex_1$2.indexToPrintableIndexMapper,
            IndexToPrintableIndex_1$2.indexToPrintableIndexUnmapper);
    }
    char16bits$1.char16bits = char16bits;

    var fullUnicode$1 = {};

    Object.defineProperty(fullUnicode$1, "__esModule", {value: true});
    fullUnicode$1.fullUnicode = void 0;
    const CharacterArbitraryBuilder_1$2 = CharacterArbitraryBuilder;
    const IndexToPrintableIndex_1$1 = IndexToPrintableIndex;
    const gapSize$1 = 0xdfff + 1 - 0xd800;
    function unicodeMapper$1(v) {
        if (v < 0xd800)
            return (0, IndexToPrintableIndex_1$1.indexToPrintableIndexMapper)(v);
        return v + gapSize$1;
    }
    function unicodeUnmapper$1(v) {
        if (v < 0xd800)
            return (0, IndexToPrintableIndex_1$1.indexToPrintableIndexUnmapper)(v);
        if (v <= 0xdfff)
            return -1;
        return v - gapSize$1;
    }
    function fullUnicode() {
        return (0, CharacterArbitraryBuilder_1$2.buildCharacterArbitrary)(
            0x0000, 0x10ffff - gapSize$1, unicodeMapper$1, unicodeUnmapper$1);
    }
    fullUnicode$1.fullUnicode = fullUnicode;

    var hexa$1 = {};

    Object.defineProperty(hexa$1, "__esModule", {value: true});
    hexa$1.hexa = void 0;
    const CharacterArbitraryBuilder_1$1 = CharacterArbitraryBuilder;
    function hexaMapper(v) {
        return v < 10 ? v + 48 : v + 97 - 10;
    }
    function hexaUnmapper(v) {
        return v < 58 ? v - 48 : v >= 97 && v < 103 ? v - 97 + 10 : -1;
    }
    function hexa() {
        return (0, CharacterArbitraryBuilder_1$1.buildCharacterArbitrary)(
            0, 15, hexaMapper, hexaUnmapper);
    }
    hexa$1.hexa = hexa;

    var unicode$1 = {};

    Object.defineProperty(unicode$1, "__esModule", {value: true});
    unicode$1.unicode = void 0;
    const CharacterArbitraryBuilder_1 = CharacterArbitraryBuilder;
    const IndexToPrintableIndex_1 = IndexToPrintableIndex;
    const gapSize = 0xdfff + 1 - 0xd800;
    function unicodeMapper(v) {
        if (v < 0xd800)
            return (0, IndexToPrintableIndex_1.indexToPrintableIndexMapper)(v);
        return v + gapSize;
    }
    function unicodeUnmapper(v) {
        if (v < 0xd800)
            return (0, IndexToPrintableIndex_1.indexToPrintableIndexUnmapper)(v);
        if (v <= 0xdfff)
            return -1;
        return v - gapSize;
    }
    function unicode() {
        return (0, CharacterArbitraryBuilder_1.buildCharacterArbitrary)(
            0x0000, 0xffff - gapSize, unicodeMapper, unicodeUnmapper);
    }
    unicode$1.unicode = unicode;

    var constant$1 = {};

    Object.defineProperty(constant$1, "__esModule", {value: true});
    constant$1.constant = void 0;
    const ConstantArbitrary_1 = ConstantArbitrary$1;
    function constant(value) {
        return new ConstantArbitrary_1.ConstantArbitrary([value]);
    }
    constant$1.constant = constant;

    var context$1 = {};

    Object.defineProperty(context$1, "__esModule", {value: true});
    context$1.context = void 0;
    const symbols_1$7 = symbols;
    const constant_1$6 = constant$1;
    class ContextImplem {
        constructor() {
            this.receivedLogs = [];
        }
        log(data) {
            this.receivedLogs.push(data);
        }
        size() {
            return this.receivedLogs.length;
        }
        toString() {
            return JSON.stringify({logs: this.receivedLogs});
        }
        [symbols_1$7.cloneMethod]() {
            return new ContextImplem();
        }
    }
    function context() {
        return (0, constant_1$6.constant)(new ContextImplem());
    }
    context$1.context = context;

    var date$1 = {};

    var TimeToDate = {};

    Object.defineProperty(TimeToDate, "__esModule", {value: true});
    TimeToDate.timeToDateUnmapper = TimeToDate.timeToDateMapper = void 0;
    function timeToDateMapper(time) {
        return new Date(time);
    }
    TimeToDate.timeToDateMapper = timeToDateMapper;
    function timeToDateUnmapper(value) {
        if (!(value instanceof Date) || value.constructor !== Date) {
            throw new Error('Not a valid value for date unmapper');
        }
        return value.getTime();
    }
    TimeToDate.timeToDateUnmapper = timeToDateUnmapper;

    Object.defineProperty(date$1, "__esModule", {value: true});
    date$1.date = void 0;
    const integer_1$c = integer$1;
    const TimeToDate_1 = TimeToDate;
    function date(constraints) {
        const intMin = constraints && constraints.min !== undefined ? constraints.min.getTime()
                                                                    : -8640000000000000;
        const intMax = constraints && constraints.max !== undefined ? constraints.max.getTime()
                                                                    : 8640000000000000;
        if (Number.isNaN(intMin))
            throw new Error('fc.date min must be valid instance of Date');
        if (Number.isNaN(intMax))
            throw new Error('fc.date max must be valid instance of Date');
        if (intMin > intMax)
            throw new Error('fc.date max must be greater or equal to min');
        return (0, integer_1$c.integer)({min: intMin, max: intMax})
            .map(TimeToDate_1.timeToDateMapper, TimeToDate_1.timeToDateUnmapper);
    }
    date$1.date = date;

    var clone$1 = {};

    var CloneArbitrary$1 = {};

    Object.defineProperty(CloneArbitrary$1, "__esModule", {value: true});
    CloneArbitrary$1.CloneArbitrary = void 0;
    const Arbitrary_1$b = Arbitrary$1;
    const Value_1$9 = Value$1;
    const symbols_1$6 = symbols;
    const Stream_1$7 = Stream$1;
    class CloneArbitrary extends Arbitrary_1$b.Arbitrary {
        constructor(arb, numValues) {
            super();
            this.arb = arb;
            this.numValues = numValues;
        }
        generate(mrng, biasFactor) {
            const items = [];
            if (this.numValues <= 0) {
                return this.wrapper(items);
            }
            for (let idx = 0; idx !== this.numValues - 1; ++idx) {
                items.push(this.arb.generate(mrng.clone(), biasFactor));
            }
            items.push(this.arb.generate(mrng, biasFactor));
            return this.wrapper(items);
        }
        canShrinkWithoutContext(value) {
            if (!Array.isArray(value) || value.length !== this.numValues) {
                return false;
            }
            if (value.length === 0) {
                return true;
            }
            for (let index = 1; index < value.length; ++index) {
                if (!Object.is(value[0], value[index])) {
                    return false;
                }
            }
            return this.arb.canShrinkWithoutContext(value[0]);
        }
        shrink(value, context) {
            if (value.length === 0) {
                return Stream_1$7.Stream.nil();
            }
            return new Stream_1$7
                .Stream(this.shrinkImpl(value, context !== undefined ? context : []))
                .map((v) => this.wrapper(v));
        }
        * shrinkImpl(value, contexts) {
            const its = value.map((v, idx) => this.arb.shrink(v, contexts[idx])[Symbol.iterator]());
            let cur = its.map((it) => it.next());
            while (!cur[0].done) {
                yield cur.map((c) => c.value);
                cur = its.map((it) => it.next());
            }
        }
        static makeItCloneable(vs, shrinkables) {
            vs[symbols_1$6.cloneMethod] = () => {
                const cloned = [];
                for (let idx = 0; idx !== shrinkables.length; ++idx) {
                    cloned.push(shrinkables[idx].value);
                }
                this.makeItCloneable(cloned, shrinkables);
                return cloned;
            };
            return vs;
        }
        wrapper(items) {
            let cloneable = false;
            const vs = [];
            const contexts = [];
            for (let idx = 0; idx !== items.length; ++idx) {
                const s = items[idx];
                cloneable = cloneable || s.hasToBeCloned;
                vs.push(s.value);
                contexts.push(s.context);
            }
            if (cloneable) {
                CloneArbitrary.makeItCloneable(vs, items);
            }
            return new Value_1$9.Value(vs, contexts);
        }
    }
    CloneArbitrary$1.CloneArbitrary = CloneArbitrary;

    Object.defineProperty(clone$1, "__esModule", {value: true});
    clone$1.clone = void 0;
    const CloneArbitrary_1 = CloneArbitrary$1;
    function clone(arb, numValues) {
        return new CloneArbitrary_1.CloneArbitrary(arb, numValues);
    }
    clone$1.clone = clone;

    var dictionary$1 = {};

    var uniqueArray$1 = {};

    var CustomEqualSet$1 = {};

    Object.defineProperty(CustomEqualSet$1, "__esModule", {value: true});
    CustomEqualSet$1.CustomEqualSet = void 0;
    class CustomEqualSet {
        constructor(isEqual) {
            this.isEqual = isEqual;
            this.data = [];
        }
        tryAdd(value) {
            for (let idx = 0; idx !== this.data.length; ++idx) {
                if (this.isEqual(this.data[idx], value)) {
                    return false;
                }
            }
            this.data.push(value);
            return true;
        }
        size() {
            return this.data.length;
        }
        getData() {
            return this.data.slice();
        }
    }
    CustomEqualSet$1.CustomEqualSet = CustomEqualSet;

    var StrictlyEqualSet$1 = {};

    Object.defineProperty(StrictlyEqualSet$1, "__esModule", {value: true});
    StrictlyEqualSet$1.StrictlyEqualSet = void 0;
    class StrictlyEqualSet {
        constructor(selector) {
            this.selector = selector;
            this.selectedItemsExceptNaN = new Set();
            this.data = [];
        }
        tryAdd(value) {
            const selected = this.selector(value);
            if (Number.isNaN(selected)) {
                this.data.push(value);
                return true;
            }
            const sizeBefore = this.selectedItemsExceptNaN.size;
            this.selectedItemsExceptNaN.add(selected);
            if (sizeBefore !== this.selectedItemsExceptNaN.size) {
                this.data.push(value);
                return true;
            }
            return false;
        }
        size() {
            return this.data.length;
        }
        getData() {
            return this.data;
        }
    }
    StrictlyEqualSet$1.StrictlyEqualSet = StrictlyEqualSet;

    var SameValueSet$1 = {};

    Object.defineProperty(SameValueSet$1, "__esModule", {value: true});
    SameValueSet$1.SameValueSet = void 0;
    class SameValueSet {
        constructor(selector) {
            this.selector = selector;
            this.selectedItemsExceptMinusZero = new Set();
            this.data = [];
            this.hasMinusZero = false;
        }
        tryAdd(value) {
            const selected = this.selector(value);
            if (Object.is(selected, -0)) {
                if (this.hasMinusZero) {
                    return false;
                }
                this.data.push(value);
                this.hasMinusZero = true;
                return true;
            }
            const sizeBefore = this.selectedItemsExceptMinusZero.size;
            this.selectedItemsExceptMinusZero.add(selected);
            if (sizeBefore !== this.selectedItemsExceptMinusZero.size) {
                this.data.push(value);
                return true;
            }
            return false;
        }
        size() {
            return this.data.length;
        }
        getData() {
            return this.data;
        }
    }
    SameValueSet$1.SameValueSet = SameValueSet;

    var SameValueZeroSet$1 = {};

    Object.defineProperty(SameValueZeroSet$1, "__esModule", {value: true});
    SameValueZeroSet$1.SameValueZeroSet = void 0;
    class SameValueZeroSet {
        constructor(selector) {
            this.selector = selector;
            this.selectedItems = new Set();
            this.data = [];
        }
        tryAdd(value) {
            const selected = this.selector(value);
            const sizeBefore = this.selectedItems.size;
            this.selectedItems.add(selected);
            if (sizeBefore !== this.selectedItems.size) {
                this.data.push(value);
                return true;
            }
            return false;
        }
        size() {
            return this.data.length;
        }
        getData() {
            return this.data;
        }
    }
    SameValueZeroSet$1.SameValueZeroSet = SameValueZeroSet;

    Object.defineProperty(uniqueArray$1, "__esModule", {value: true});
    uniqueArray$1.uniqueArray = void 0;
    const ArrayArbitrary_1 = ArrayArbitrary$1;
    const MaxLengthFromMinLength_1$7 = MaxLengthFromMinLength;
    const CustomEqualSet_1 = CustomEqualSet$1;
    const StrictlyEqualSet_1 = StrictlyEqualSet$1;
    const SameValueSet_1 = SameValueSet$1;
    const SameValueZeroSet_1 = SameValueZeroSet$1;
    function buildUniqueArraySetBuilder(constraints) {
        if (typeof constraints.comparator === 'function') {
            if (constraints.selector === undefined) {
                const comparator = constraints.comparator;
                const isEqualForBuilder = (nextA, nextB) => comparator(nextA.value_, nextB.value_);
                return () => new CustomEqualSet_1.CustomEqualSet(isEqualForBuilder);
            }
            const comparator = constraints.comparator;
            const selector = constraints.selector;
            const refinedSelector = (next) => selector(next.value_);
            const isEqualForBuilder = (nextA, nextB) =>
                comparator(refinedSelector(nextA), refinedSelector(nextB));
            return () => new CustomEqualSet_1.CustomEqualSet(isEqualForBuilder);
        }
        const selector = constraints.selector || ((v) => v);
        const refinedSelector = (next) => selector(next.value_);
        switch (constraints.comparator) {
            case 'IsStrictlyEqual':
                return () => new StrictlyEqualSet_1.StrictlyEqualSet(refinedSelector);
            case 'SameValueZero':
                return () => new SameValueZeroSet_1.SameValueZeroSet(refinedSelector);
            case 'SameValue':
            case undefined:
                return () => new SameValueSet_1.SameValueSet(refinedSelector);
        }
    }
    function uniqueArray(arb, constraints = {}) {
        const minLength = constraints.minLength !== undefined ? constraints.minLength : 0;
        const maxLength = constraints.maxLength !== undefined
            ? constraints.maxLength
            : MaxLengthFromMinLength_1$7.MaxLengthUpperBound;
        const maxGeneratedLength =
            (0, MaxLengthFromMinLength_1$7.maxGeneratedLengthFromSizeForArbitrary)(
                constraints.size, minLength, maxLength, constraints.maxLength !== undefined);
        const depthIdentifier = constraints.depthIdentifier;
        const setBuilder = buildUniqueArraySetBuilder(constraints);
        const arrayArb = new ArrayArbitrary_1.ArrayArbitrary(
            arb, minLength, maxGeneratedLength, maxLength, depthIdentifier, setBuilder, []);
        if (minLength === 0)
            return arrayArb;
        return arrayArb.filter((tab) => tab.length >= minLength);
    }
    uniqueArray$1.uniqueArray = uniqueArray;

    var KeyValuePairsToObject = {};

    Object.defineProperty(KeyValuePairsToObject, "__esModule", {value: true});
    KeyValuePairsToObject.keyValuePairsToObjectUnmapper =
        KeyValuePairsToObject.keyValuePairsToObjectMapper = void 0;
    function keyValuePairsToObjectMapper(items) {
        const obj = {};
        for (const keyValue of items) {
            Object.defineProperty(obj, keyValue[0], {
                enumerable: true,
                configurable: true,
                writable: true,
                value: keyValue[1],
            });
        }
        return obj;
    }
    KeyValuePairsToObject.keyValuePairsToObjectMapper = keyValuePairsToObjectMapper;
    function buildInvalidPropertyNameFilter(obj) {
        return function invalidPropertyNameFilter(key) {
            const descriptor = Object.getOwnPropertyDescriptor(obj, key);
            return (descriptor === undefined || !descriptor.configurable ||
                    !descriptor.enumerable || !descriptor.writable ||
                    descriptor.get !== undefined || descriptor.set !== undefined);
        };
    }
    function keyValuePairsToObjectUnmapper(value) {
        if (typeof value !== 'object' || value === null) {
            throw new Error('Incompatible instance received: should be a non-null object');
        }
        if (!('constructor' in value) || value.constructor !== Object) {
            throw new Error('Incompatible instance received: should be of exact type Object');
        }
        if (Object.getOwnPropertySymbols(value).length > 0) {
            throw new Error('Incompatible instance received: should contain symbols');
        }
        if (Object.getOwnPropertyNames(value).find(buildInvalidPropertyNameFilter(value)) !==
            undefined) {
            throw new Error(
                'Incompatible instance received: should contain only c/e/w properties without get/set');
        }
        return Object.entries(value);
    }
    KeyValuePairsToObject.keyValuePairsToObjectUnmapper = keyValuePairsToObjectUnmapper;

    Object.defineProperty(dictionary$1, "__esModule", {value: true});
    dictionary$1.dictionary = void 0;
    const tuple_1$f = tuple$1;
    const uniqueArray_1$2 = uniqueArray$1;
    const KeyValuePairsToObject_1$1 = KeyValuePairsToObject;
    function dictionaryKeyExtractor(entry) {
        return entry[0];
    }
    function dictionary(keyArb, valueArb, constraints = {}) {
        return (0, uniqueArray_1$2.uniqueArray)((0, tuple_1$f.tuple)(keyArb, valueArb), {
                   minLength: constraints.minKeys,
                   maxLength: constraints.maxKeys,
                   size: constraints.size,
                   selector: dictionaryKeyExtractor,
               })
            .map(KeyValuePairsToObject_1$1.keyValuePairsToObjectMapper,
                 KeyValuePairsToObject_1$1.keyValuePairsToObjectUnmapper);
    }
    dictionary$1.dictionary = dictionary;

    var emailAddress$1 = {};

    var CharacterRangeArbitraryBuilder = {};

    var oneof$1 = {};

    var FrequencyArbitrary$1 = {};

    Object.defineProperty(FrequencyArbitrary$1, "__esModule", {value: true});
    FrequencyArbitrary$1.FrequencyArbitrary = void 0;
    const Stream_1$6 = Stream$1;
    const Arbitrary_1$a = Arbitrary$1;
    const Value_1$8 = Value$1;
    const DepthContext_1$1 = DepthContext;
    const MaxLengthFromMinLength_1$6 = MaxLengthFromMinLength;
    class FrequencyArbitrary extends Arbitrary_1$a.Arbitrary {
        constructor(warbs, constraints, context) {
            super();
            this.warbs = warbs;
            this.constraints = constraints;
            this.context = context;
            let currentWeight = 0;
            this.cumulatedWeights = [];
            for (let idx = 0; idx !== warbs.length; ++idx) {
                currentWeight += warbs[idx].weight;
                this.cumulatedWeights.push(currentWeight);
            }
            this.totalWeight = currentWeight;
        }
        static from(warbs, constraints, label) {
            if (warbs.length === 0) {
                throw new Error(`${label} expects at least one weighted arbitrary`);
            }
            let totalWeight = 0;
            for (let idx = 0; idx !== warbs.length; ++idx) {
                const currentArbitrary = warbs[idx].arbitrary;
                if (currentArbitrary === undefined) {
                    throw new Error(`${label} expects arbitraries to be specified`);
                }
                const currentWeight = warbs[idx].weight;
                totalWeight += currentWeight;
                if (!Number.isInteger(currentWeight)) {
                    throw new Error(`${label} expects weights to be integer values`);
                }
                if (currentWeight < 0) {
                    throw new Error(`${label} expects weights to be superior or equal to 0`);
                }
            }
            if (totalWeight <= 0) {
                throw new Error(`${label} expects the sum of weights to be strictly superior to 0`);
            }
            const sanitizedConstraints = {
                depthBias: (0, MaxLengthFromMinLength_1$6.depthBiasFromSizeForArbitrary)(
                    constraints.depthSize, constraints.maxDepth !== undefined),
                maxDepth: constraints.maxDepth != undefined ? constraints.maxDepth
                                                            : Number.POSITIVE_INFINITY,
                withCrossShrink: !!constraints.withCrossShrink,
            };
            return new FrequencyArbitrary(
                warbs,
                sanitizedConstraints,
                (0, DepthContext_1$1.getDepthContextFor)(constraints.depthIdentifier));
        }
        generate(mrng, biasFactor) {
            if (this.mustGenerateFirst()) {
                return this.safeGenerateForIndex(mrng, 0, biasFactor);
            }
            const selected = mrng.nextInt(this.computeNegDepthBenefit(), this.totalWeight - 1);
            for (let idx = 0; idx !== this.cumulatedWeights.length; ++idx) {
                if (selected < this.cumulatedWeights[idx]) {
                    return this.safeGenerateForIndex(mrng, idx, biasFactor);
                }
            }
            throw new Error(`Unable to generate from fc.frequency`);
        }
        canShrinkWithoutContext(value) {
            return this.canShrinkWithoutContextIndex(value) !== -1;
        }
        shrink(value, context) {
            if (context !== undefined) {
                const safeContext = context;
                const selectedIndex = safeContext.selectedIndex;
                const originalBias = safeContext.originalBias;
                const originalArbitrary = this.warbs[selectedIndex].arbitrary;
                const originalShrinks =
                    originalArbitrary.shrink(value, safeContext.originalContext)
                        .map((v) => this.mapIntoValue(selectedIndex, v, null, originalBias));
                if (safeContext.clonedMrngForFallbackFirst !== null) {
                    if (safeContext.cachedGeneratedForFirst === undefined) {
                        safeContext.cachedGeneratedForFirst = this.safeGenerateForIndex(
                            safeContext.clonedMrngForFallbackFirst, 0, originalBias);
                    }
                    const valueFromFirst = safeContext.cachedGeneratedForFirst;
                    return Stream_1$6.Stream.of(valueFromFirst).join(originalShrinks);
                }
                return originalShrinks;
            }
            const potentialSelectedIndex = this.canShrinkWithoutContextIndex(value);
            if (potentialSelectedIndex === -1) {
                return Stream_1$6.Stream.nil();
            }
            return this.defaultShrinkForFirst(potentialSelectedIndex)
                .join(
                    this.warbs[potentialSelectedIndex]
                        .arbitrary.shrink(value, undefined)
                        .map((v) => this.mapIntoValue(potentialSelectedIndex, v, null, undefined)));
        }
        defaultShrinkForFirst(selectedIndex) {
            ++this.context.depth;
            try {
                if (!this.mustFallbackToFirstInShrink(selectedIndex) ||
                    this.warbs[0].fallbackValue === undefined) {
                    return Stream_1$6.Stream.nil();
                }
            } finally {
                --this.context.depth;
            }
            const rawShrinkValue =
                new Value_1$8.Value(this.warbs[0].fallbackValue.default, undefined);
            return Stream_1$6.Stream.of(this.mapIntoValue(0, rawShrinkValue, null, undefined));
        }
        canShrinkWithoutContextIndex(value) {
            if (this.mustGenerateFirst()) {
                return this.warbs[0].arbitrary.canShrinkWithoutContext(value) ? 0 : -1;
            }
            try {
                ++this.context.depth;
                for (let idx = 0; idx !== this.warbs.length; ++idx) {
                    const warb = this.warbs[idx];
                    if (warb.weight !== 0 && warb.arbitrary.canShrinkWithoutContext(value)) {
                        return idx;
                    }
                }
                return -1;
            } finally {
                --this.context.depth;
            }
        }
        mapIntoValue(idx, value, clonedMrngForFallbackFirst, biasFactor) {
            const context = {
                selectedIndex: idx,
                originalBias: biasFactor,
                originalContext: value.context,
                clonedMrngForFallbackFirst,
            };
            return new Value_1$8.Value(value.value, context);
        }
        safeGenerateForIndex(mrng, idx, biasFactor) {
            ++this.context.depth;
            try {
                const value = this.warbs[idx].arbitrary.generate(mrng, biasFactor);
                const clonedMrngForFallbackFirst =
                    this.mustFallbackToFirstInShrink(idx) ? mrng.clone() : null;
                return this.mapIntoValue(idx, value, clonedMrngForFallbackFirst, biasFactor);
            } finally {
                --this.context.depth;
            }
        }
        mustGenerateFirst() {
            return this.constraints.maxDepth <= this.context.depth;
        }
        mustFallbackToFirstInShrink(idx) {
            return idx !== 0 && this.constraints.withCrossShrink && this.warbs[0].weight !== 0;
        }
        computeNegDepthBenefit() {
            const depthBias = this.constraints.depthBias;
            if (depthBias <= 0 || this.warbs[0].weight === 0) {
                return 0;
            }
            const depthBenefit = Math.floor(Math.pow(1 + depthBias, this.context.depth)) - 1;
            return -Math.min(this.totalWeight * depthBenefit, Number.MAX_SAFE_INTEGER) || 0;
        }
    }
    FrequencyArbitrary$1.FrequencyArbitrary = FrequencyArbitrary;

    Object.defineProperty(oneof$1, "__esModule", {value: true});
    oneof$1.oneof = void 0;
    const Arbitrary_1$9 = Arbitrary$1;
    const FrequencyArbitrary_1$1 = FrequencyArbitrary$1;
    function isOneOfContraints(param) {
        return (param != null && typeof param === 'object' && !('generate' in param) &&
                !('arbitrary' in param) && !('weight' in param));
    }
    function toWeightedArbitrary(maybeWeightedArbitrary) {
        if ((0, Arbitrary_1$9.isArbitrary)(maybeWeightedArbitrary)) {
            return {arbitrary: maybeWeightedArbitrary, weight: 1};
        }
        return maybeWeightedArbitrary;
    }
    function oneof(...args) {
        const constraints = args[0];
        if (isOneOfContraints(constraints)) {
            const weightedArbs = args.slice(1).map(toWeightedArbitrary);
            return FrequencyArbitrary_1$1.FrequencyArbitrary.from(
                weightedArbs, constraints, 'fc.oneof');
        }
        const weightedArbs = args.map(toWeightedArbitrary);
        return FrequencyArbitrary_1$1.FrequencyArbitrary.from(weightedArbs, {}, 'fc.oneof');
    }
    oneof$1.oneof = oneof;

    var mapToConstant$1 = {};

    var nat$1 = {};

    Object.defineProperty(nat$1, "__esModule", {value: true});
    nat$1.nat = void 0;
    const IntegerArbitrary_1$3 = IntegerArbitrary$1;
    function nat(arg) {
        const max =
            typeof arg === 'number' ? arg : arg && arg.max !== undefined ? arg.max : 0x7fffffff;
        if (max < 0) {
            throw new Error('fc.nat value should be greater than or equal to 0');
        }
        if (!Number.isInteger(max)) {
            throw new Error('fc.nat maximum value should be an integer');
        }
        return new IntegerArbitrary_1$3.IntegerArbitrary(0, max);
    }
    nat$1.nat = nat;

    var IndexToMappedConstant = {};

    Object.defineProperty(IndexToMappedConstant, "__esModule", {value: true});
    IndexToMappedConstant.indexToMappedConstantUnmapperFor =
        IndexToMappedConstant.indexToMappedConstantMapperFor = void 0;
    function indexToMappedConstantMapperFor(entries) {
        return function indexToMappedConstantMapper(choiceIndex) {
            let idx = -1;
            let numSkips = 0;
            while (choiceIndex >= numSkips) {
                numSkips += entries[++idx].num;
            }
            return entries[idx].build(choiceIndex - numSkips + entries[idx].num);
        };
    }
    IndexToMappedConstant.indexToMappedConstantMapperFor = indexToMappedConstantMapperFor;
    function buildReverseMapping(entries) {
        const reverseMapping = {mapping: new Map(), negativeZeroIndex: undefined};
        let choiceIndex = 0;
        for (let entryIdx = 0; entryIdx !== entries.length; ++entryIdx) {
            const entry = entries[entryIdx];
            for (let idxInEntry = 0; idxInEntry !== entry.num; ++idxInEntry) {
                const value = entry.build(idxInEntry);
                if (value === 0 && 1 / value === Number.NEGATIVE_INFINITY) {
                    reverseMapping.negativeZeroIndex = choiceIndex;
                } else {
                    reverseMapping.mapping.set(value, choiceIndex);
                }
                ++choiceIndex;
            }
        }
        return reverseMapping;
    }
    function indexToMappedConstantUnmapperFor(entries) {
        let reverseMapping = null;
        return function indexToMappedConstantUnmapper(value) {
            if (reverseMapping === null) {
                reverseMapping = buildReverseMapping(entries);
            }
            const choiceIndex = Object.is(value, -0) ? reverseMapping.negativeZeroIndex
                                                     : reverseMapping.mapping.get(value);
            if (choiceIndex === undefined) {
                throw new Error(
                    'Unknown value encountered cannot be built using this mapToConstant');
            }
            return choiceIndex;
        };
    }
    IndexToMappedConstant.indexToMappedConstantUnmapperFor = indexToMappedConstantUnmapperFor;

    Object.defineProperty(mapToConstant$1, "__esModule", {value: true});
    mapToConstant$1.mapToConstant = void 0;
    const nat_1$3 = nat$1;
    const IndexToMappedConstant_1 = IndexToMappedConstant;
    function computeNumChoices(options) {
        if (options.length === 0)
            throw new Error(`fc.mapToConstant expects at least one option`);
        let numChoices = 0;
        for (let idx = 0; idx !== options.length; ++idx) {
            if (options[idx].num < 0)
                throw new Error(
                    `fc.mapToConstant expects all options to have a number of entries greater or equal to zero`);
            numChoices += options[idx].num;
        }
        if (numChoices === 0)
            throw new Error(`fc.mapToConstant expects at least one choice among options`);
        return numChoices;
    }
    function mapToConstant(...entries) {
        const numChoices = computeNumChoices(entries);
        return (0, nat_1$3.nat)({max: numChoices - 1})
            .map((0, IndexToMappedConstant_1.indexToMappedConstantMapperFor)(entries),
                 (0, IndexToMappedConstant_1.indexToMappedConstantUnmapperFor)(entries));
    }
    mapToConstant$1.mapToConstant = mapToConstant;

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.buildAlphaNumericPercentArbitrary = exports.buildAlphaNumericArbitrary =
            exports.buildLowerAlphaNumericArbitrary = exports.buildLowerAlphaArbitrary = void 0;
        const fullUnicode_1 = fullUnicode$1;
        const oneof_1 = oneof$1;
        const mapToConstant_1 = mapToConstant$1;
        const lowerCaseMapper = {num: 26, build: (v) => String.fromCharCode(v + 0x61)};
        const upperCaseMapper = {num: 26, build: (v) => String.fromCharCode(v + 0x41)};
        const numericMapper = {num: 10, build: (v) => String.fromCharCode(v + 0x30)};
        function percentCharArbMapper(c) {
            const encoded = encodeURIComponent(c);
            return c !== encoded ? encoded : `%${c.charCodeAt(0).toString(16)}`;
        }
        function percentCharArbUnmapper(value) {
            if (typeof value !== 'string') {
                throw new Error('Unsupported');
            }
            const decoded = decodeURIComponent(value);
            return decoded;
        }
        const percentCharArb =
            (0, fullUnicode_1.fullUnicode)().map(percentCharArbMapper, percentCharArbUnmapper);
        const buildLowerAlphaArbitrary = (others) => (0, mapToConstant_1.mapToConstant)(
            lowerCaseMapper, {num: others.length, build: (v) => others[v]});
        exports.buildLowerAlphaArbitrary = buildLowerAlphaArbitrary;
        const buildLowerAlphaNumericArbitrary = (others) => (0, mapToConstant_1.mapToConstant)(
            lowerCaseMapper, numericMapper, {num: others.length, build: (v) => others[v]});
        exports.buildLowerAlphaNumericArbitrary = buildLowerAlphaNumericArbitrary;
        const buildAlphaNumericArbitrary = (others) =>
            (0, mapToConstant_1.mapToConstant)(lowerCaseMapper,
                                               upperCaseMapper,
                                               numericMapper,
                                               {num: others.length, build: (v) => others[v]});
        exports.buildAlphaNumericArbitrary = buildAlphaNumericArbitrary;
        const buildAlphaNumericPercentArbitrary = (others) => (0, oneof_1.oneof)(
            {weight: 10, arbitrary: (0, exports.buildAlphaNumericArbitrary)(others)},
            {weight: 1, arbitrary: percentCharArb});
        exports.buildAlphaNumericPercentArbitrary = buildAlphaNumericPercentArbitrary;
    }(CharacterRangeArbitraryBuilder));

    var domain$1 = {};

    var option$1 = {};

    Object.defineProperty(option$1, "__esModule", {value: true});
    option$1.option = void 0;
    const constant_1$5 = constant$1;
    const FrequencyArbitrary_1 = FrequencyArbitrary$1;
    function option(arb, constraints = {}) {
        const freq = constraints.freq == null ? 5 : constraints.freq;
        const nilValue =
            Object.prototype.hasOwnProperty.call(constraints, 'nil') ? constraints.nil : null;
        const nilArb = (0, constant_1$5.constant)(nilValue);
        const weightedArbs = [
            {arbitrary: nilArb, weight: 1, fallbackValue: {default: nilValue}},
            {arbitrary: arb, weight: freq},
        ];
        const frequencyConstraints = {
            withCrossShrink: true,
            depthSize: constraints.depthSize,
            maxDepth: constraints.maxDepth,
            depthIdentifier: constraints.depthIdentifier,
        };
        return FrequencyArbitrary_1.FrequencyArbitrary.from(
            weightedArbs, frequencyConstraints, 'fc.option');
    }
    option$1.option = option;

    var stringOf$1 = {};

    var PatternsToString = {};

    Object.defineProperty(PatternsToString, "__esModule", {value: true});
    PatternsToString.patternsToStringUnmapperFor = PatternsToString.patternsToStringMapper = void 0;
    const MaxLengthFromMinLength_1$5 = MaxLengthFromMinLength;
    function patternsToStringMapper(tab) {
        return tab.join('');
    }
    PatternsToString.patternsToStringMapper = patternsToStringMapper;
    function patternsToStringUnmapperFor(patternsArb, constraints) {
        return function patternsToStringUnmapper(value) {
            if (typeof value !== 'string') {
                throw new Error('Unsupported value');
            }
            const minLength = constraints.minLength !== undefined ? constraints.minLength : 0;
            const maxLength = constraints.maxLength !== undefined
                ? constraints.maxLength
                : MaxLengthFromMinLength_1$5.MaxLengthUpperBound;
            if (value.length === 0) {
                if (minLength > 0) {
                    throw new Error('Unable to unmap received string');
                }
                return [];
            }
            const stack = [{endIndexChunks: 0, nextStartIndex: 1, chunks: []}];
            while (stack.length > 0) {
                const last = stack.pop();
                for (let index = last.nextStartIndex; index <= value.length; ++index) {
                    const chunk = value.substring(last.endIndexChunks, index);
                    if (patternsArb.canShrinkWithoutContext(chunk)) {
                        const newChunks = last.chunks.concat([chunk]);
                        if (index === value.length) {
                            if (newChunks.length < minLength || newChunks.length > maxLength) {
                                break;
                            }
                            return newChunks;
                        }
                        stack.push({
                            endIndexChunks: last.endIndexChunks,
                            nextStartIndex: index + 1,
                            chunks: last.chunks
                        });
                        stack.push(
                            {endIndexChunks: index, nextStartIndex: index + 1, chunks: newChunks});
                        break;
                    }
                }
            }
            throw new Error('Unable to unmap received string');
        };
    }
    PatternsToString.patternsToStringUnmapperFor = patternsToStringUnmapperFor;

    var SlicesForStringBuilder = {};

    Object.defineProperty(SlicesForStringBuilder, "__esModule", {value: true});
    SlicesForStringBuilder.createSlicesForString = void 0;
    const dangerousStrings = [
        '__defineGetter__',
        '__defineSetter__',
        '__lookupGetter__',
        '__lookupSetter__',
        '__proto__',
        'constructor',
        'hasOwnProperty',
        'isPrototypeOf',
        'propertyIsEnumerable',
        'toLocaleString',
        'toString',
        'valueOf',
        'apply',
        'arguments',
        'bind',
        'call',
        'caller',
        'length',
        'name',
        'prototype',
        'key',
        'ref',
    ];
    function computeCandidateString(dangerous, charArbitrary, stringSplitter) {
        let candidate;
        try {
            candidate = stringSplitter(dangerous);
        } catch (err) {
            return undefined;
        }
        for (const entry of candidate) {
            if (!charArbitrary.canShrinkWithoutContext(entry)) {
                return undefined;
            }
        }
        return candidate;
    }
    function createSlicesForString(charArbitrary, stringSplitter) {
        const slicesForString = [];
        for (const dangerous of dangerousStrings) {
            const candidate = computeCandidateString(dangerous, charArbitrary, stringSplitter);
            if (candidate !== undefined) {
                slicesForString.push(candidate);
            }
        }
        return slicesForString;
    }
    SlicesForStringBuilder.createSlicesForString = createSlicesForString;

    Object.defineProperty(stringOf$1, "__esModule", {value: true});
    stringOf$1.stringOf = void 0;
    const array_1$h = array$1;
    const PatternsToString_1 = PatternsToString;
    const SlicesForStringBuilder_1$7 = SlicesForStringBuilder;
    function stringOf(charArb, constraints = {}) {
        const unmapper = (0, PatternsToString_1.patternsToStringUnmapperFor)(charArb, constraints);
        const experimentalCustomSlices =
            (0, SlicesForStringBuilder_1$7.createSlicesForString)(charArb, unmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$h.array)(charArb, enrichedConstraints)
            .map(PatternsToString_1.patternsToStringMapper, unmapper);
    }
    stringOf$1.stringOf = stringOf;

    var InvalidSubdomainLabelFiIter = {};

    Object.defineProperty(InvalidSubdomainLabelFiIter, "__esModule", {value: true});
    InvalidSubdomainLabelFiIter.filterInvalidSubdomainLabel = void 0;
    function filterInvalidSubdomainLabel(subdomainLabel) {
        if (subdomainLabel.length > 63) {
            return false;
        }
        return (subdomainLabel.length < 4 || subdomainLabel[0] !== 'x' ||
                subdomainLabel[1] !== 'n' || subdomainLabel[2] !== '-' ||
                subdomainLabel[3] !== '-');
    }
    InvalidSubdomainLabelFiIter.filterInvalidSubdomainLabel = filterInvalidSubdomainLabel;

    var AdapterArbitrary$1 = {};

    Object.defineProperty(AdapterArbitrary$1, "__esModule", {value: true});
    AdapterArbitrary$1.adapter = void 0;
    const Arbitrary_1$8 = Arbitrary$1;
    const Value_1$7 = Value$1;
    const Stream_1$5 = Stream$1;
    const AdaptedValue = Symbol('adapted-value');
    function toAdapterValue(rawValue, adapter) {
        const adapted = adapter(rawValue.value_);
        if (!adapted.adapted) {
            return rawValue;
        }
        return new Value_1$7.Value(adapted.value, AdaptedValue);
    }
    class AdapterArbitrary extends Arbitrary_1$8.Arbitrary {
        constructor(sourceArb, adapter) {
            super();
            this.sourceArb = sourceArb;
            this.adapter = adapter;
            this.adaptValue = (rawValue) => toAdapterValue(rawValue, adapter);
        }
        generate(mrng, biasFactor) {
            const rawValue = this.sourceArb.generate(mrng, biasFactor);
            return this.adaptValue(rawValue);
        }
        canShrinkWithoutContext(value) {
            return this.sourceArb.canShrinkWithoutContext(value) && !this.adapter(value).adapted;
        }
        shrink(value, context) {
            if (context === AdaptedValue) {
                if (!this.sourceArb.canShrinkWithoutContext(value)) {
                    return Stream_1$5.Stream.nil();
                }
                return this.sourceArb.shrink(value, undefined).map(this.adaptValue);
            }
            return this.sourceArb.shrink(value, context).map(this.adaptValue);
        }
    }
    function adapter(sourceArb, adapter) {
        return new AdapterArbitrary(sourceArb, adapter);
    }
    AdapterArbitrary$1.adapter = adapter;

    Object.defineProperty(domain$1, "__esModule", {value: true});
    domain$1.domain = void 0;
    const array_1$g = array$1;
    const CharacterRangeArbitraryBuilder_1$4 = CharacterRangeArbitraryBuilder;
    const option_1$3 = option$1;
    const stringOf_1$4 = stringOf$1;
    const tuple_1$e = tuple$1;
    const InvalidSubdomainLabelFiIter_1 = InvalidSubdomainLabelFiIter;
    const MaxLengthFromMinLength_1$4 = MaxLengthFromMinLength;
    const AdapterArbitrary_1$1 = AdapterArbitrary$1;
    function toSubdomainLabelMapper([f, d]) {
        return d === null ? f : `${f}${d[0]}${d[1]}`;
    }
    function toSubdomainLabelUnmapper(value) {
        if (typeof value !== 'string' || value.length === 0) {
            throw new Error('Unsupported');
        }
        if (value.length === 1) {
            return [value[0], null];
        }
        return [value[0], [value.substring(1, value.length - 1), value[value.length - 1]]];
    }
    function subdomainLabel(size) {
        const alphaNumericArb =
            (0, CharacterRangeArbitraryBuilder_1$4.buildLowerAlphaNumericArbitrary)([]);
        const alphaNumericHyphenArb =
            (0, CharacterRangeArbitraryBuilder_1$4.buildLowerAlphaNumericArbitrary)(['-']);
        return (0, tuple_1$e.tuple)(
                   alphaNumericArb,
                   (0, option_1$3.option)((0, tuple_1$e.tuple)(
                       (0, stringOf_1$4.stringOf)(alphaNumericHyphenArb, {size, maxLength: 61}),
                       alphaNumericArb)))
            .map(toSubdomainLabelMapper, toSubdomainLabelUnmapper)
            .filter(InvalidSubdomainLabelFiIter_1.filterInvalidSubdomainLabel);
    }
    function labelsMapper(elements) {
        return `${elements[0].join('.')}.${elements[1]}`;
    }
    function labelsUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported type');
        }
        const lastDotIndex = value.lastIndexOf('.');
        return [value.substring(0, lastDotIndex).split('.'), value.substring(lastDotIndex + 1)];
    }
    function labelsAdapter(labels) {
        const [subDomains, suffix] = labels;
        let lengthNotIncludingIndex = suffix.length;
        for (let index = 0; index !== subDomains.length; ++index) {
            lengthNotIncludingIndex += 1 + subDomains[index].length;
            if (lengthNotIncludingIndex > 255) {
                return {adapted: true, value: [subDomains.slice(0, index), suffix]};
            }
        }
        return {adapted: false, value: labels};
    }
    function domain(constraints = {}) {
        const resolvedSize = (0, MaxLengthFromMinLength_1$4.resolveSize)(constraints.size);
        const resolvedSizeMinusOne =
            (0, MaxLengthFromMinLength_1$4.relativeSizeToSize)('-1', resolvedSize);
        const alphaNumericArb =
            (0, CharacterRangeArbitraryBuilder_1$4.buildLowerAlphaArbitrary)([]);
        const publicSuffixArb = (0, stringOf_1$4.stringOf)(
            alphaNumericArb, {minLength: 2, maxLength: 63, size: resolvedSizeMinusOne});
        return ((0, AdapterArbitrary_1$1.adapter)(
                    (0, tuple_1$e.tuple)(
                        (0, array_1$g.array)(
                            subdomainLabel(resolvedSize),
                            {size: resolvedSizeMinusOne, minLength: 1, maxLength: 127}),
                        publicSuffixArb),
                    labelsAdapter)
                    .map(labelsMapper, labelsUnmapper));
    }
    domain$1.domain = domain;

    Object.defineProperty(emailAddress$1, "__esModule", {value: true});
    emailAddress$1.emailAddress = void 0;
    const array_1$f = array$1;
    const CharacterRangeArbitraryBuilder_1$3 = CharacterRangeArbitraryBuilder;
    const domain_1$1 = domain$1;
    const stringOf_1$3 = stringOf$1;
    const tuple_1$d = tuple$1;
    const AdapterArbitrary_1 = AdapterArbitrary$1;
    function dotAdapter(a) {
        let currentLength = a[0].length;
        for (let index = 1; index !== a.length; ++index) {
            currentLength += 1 + a[index].length;
            if (currentLength > 64) {
                return {adapted: true, value: a.slice(0, index)};
            }
        }
        return {adapted: false, value: a};
    }
    function dotMapper(a) {
        return a.join('.');
    }
    function dotUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported');
        }
        return value.split('.');
    }
    function atMapper(data) {
        return `${data[0]}@${data[1]}`;
    }
    function atUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported');
        }
        return value.split('@', 2);
    }
    function emailAddress(constraints = {}) {
        const others = [
            '!',
            '#',
            '$',
            '%',
            '&',
            "'",
            '*',
            '+',
            '-',
            '/',
            '=',
            '?',
            '^',
            '_',
            '`',
            '{',
            '|',
            '}',
            '~'
        ];
        const atextArb =
            (0, CharacterRangeArbitraryBuilder_1$3.buildLowerAlphaNumericArbitrary)(others);
        const localPartArb =
            (0, AdapterArbitrary_1.adapter)(
                (0, array_1$f.array)((0, stringOf_1$3.stringOf)(atextArb, {
                                         minLength: 1,
                                         maxLength: 64,
                                         size: constraints.size,
                                     }),
                                     {minLength: 1, maxLength: 32, size: constraints.size}),
                dotAdapter)
                .map(dotMapper, dotUnmapper);
        return (0, tuple_1$d.tuple)(localPartArb, (0, domain_1$1.domain)({size: constraints.size}))
            .map(atMapper, atUnmapper);
    }
    emailAddress$1.emailAddress = emailAddress;

    var double$1 = {};

    var ArrayInt64 = {};

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.logLike64 = exports.halve64 = exports.add64 = exports.negative64 =
            exports.substract64 = exports.clone64 = exports.isStrictlySmaller64 =
                exports.isEqual64 = exports.isStrictlyPositive64 = exports.isStrictlyNegative64 =
                    exports.isZero64 = exports.Unit64 = exports.Zero64 = void 0;
        exports.Zero64 = {sign: 1, data: [0, 0]};
        exports.Unit64 = {sign: 1, data: [0, 1]};
        function isZero64(a) {
            return a.data[0] === 0 && a.data[1] === 0;
        }
        exports.isZero64 = isZero64;
        function isStrictlyNegative64(a) {
            return a.sign === -1 && !isZero64(a);
        }
        exports.isStrictlyNegative64 = isStrictlyNegative64;
        function isStrictlyPositive64(a) {
            return a.sign === 1 && !isZero64(a);
        }
        exports.isStrictlyPositive64 = isStrictlyPositive64;
        function isEqual64(a, b) {
            if (a.data[0] === b.data[0] && a.data[1] === b.data[1]) {
                return a.sign === b.sign || (a.data[0] === 0 && a.data[1] === 0);
            }
            return false;
        }
        exports.isEqual64 = isEqual64;
        function isStrictlySmaller64Internal(a, b) {
            return a[0] < b[0] || (a[0] === b[0] && a[1] < b[1]);
        }
        function isStrictlySmaller64(a, b) {
            if (a.sign === b.sign) {
                return a.sign === 1 ? isStrictlySmaller64Internal(a.data, b.data)
                                    : isStrictlySmaller64Internal(b.data, a.data);
            }
            return a.sign === -1 && (!isZero64(a) || !isZero64(b));
        }
        exports.isStrictlySmaller64 = isStrictlySmaller64;
        function clone64(a) {
            return {sign: a.sign, data: [a.data[0], a.data[1]]};
        }
        exports.clone64 = clone64;
        function substract64DataInternal(a, b) {
            let reminderLow = 0;
            let low = a[1] - b[1];
            if (low < 0) {
                reminderLow = 1;
                low = low >>> 0;
            }
            return [a[0] - b[0] - reminderLow, low];
        }
        function substract64Internal(a, b) {
            if (a.sign === 1 && b.sign === -1) {
                const low = a.data[1] + b.data[1];
                const high = a.data[0] + b.data[0] + (low > 0xffffffff ? 1 : 0);
                return {sign: 1, data: [high >>> 0, low >>> 0]};
            }
            return {
                sign: 1,
                data: a.sign === 1 ? substract64DataInternal(a.data, b.data)
                                   : substract64DataInternal(b.data, a.data),
            };
        }
        function substract64(arrayIntA, arrayIntB) {
            if (isStrictlySmaller64(arrayIntA, arrayIntB)) {
                const out = substract64Internal(arrayIntB, arrayIntA);
                out.sign = -1;
                return out;
            }
            return substract64Internal(arrayIntA, arrayIntB);
        }
        exports.substract64 = substract64;
        function negative64(arrayIntA) {
            return {
                sign: -arrayIntA.sign,
                data: [arrayIntA.data[0], arrayIntA.data[1]],
            };
        }
        exports.negative64 = negative64;
        function add64(arrayIntA, arrayIntB) {
            if (isZero64(arrayIntB)) {
                if (isZero64(arrayIntA)) {
                    return clone64(exports.Zero64);
                }
                return clone64(arrayIntA);
            }
            return substract64(arrayIntA, negative64(arrayIntB));
        }
        exports.add64 = add64;
        function halve64(a) {
            return {
                sign: a.sign,
                data: [
                    Math.floor(a.data[0] / 2),
                    (a.data[0] % 2 === 1 ? 0x80000000 : 0) + Math.floor(a.data[1] / 2)
                ],
            };
        }
        exports.halve64 = halve64;
        function logLike64(a) {
            return {
                sign: a.sign,
                data: [0, Math.floor(Math.log(a.data[0] * 0x100000000 + a.data[1]) / Math.log(2))],
            };
        }
        exports.logLike64 = logLike64;
    }(ArrayInt64));

    var ArrayInt64Arbitrary$1 = {};

    Object.defineProperty(ArrayInt64Arbitrary$1, "__esModule", {value: true});
    ArrayInt64Arbitrary$1.arrayInt64 = void 0;
    const Stream_1$4 = Stream$1;
    const Arbitrary_1$7 = Arbitrary$1;
    const Value_1$6 = Value$1;
    const ArrayInt64_1$2 = ArrayInt64;
    class ArrayInt64Arbitrary extends Arbitrary_1$7.Arbitrary {
        constructor(min, max) {
            super();
            this.min = min;
            this.max = max;
            this.biasedRanges = null;
        }
        generate(mrng, biasFactor) {
            const range = this.computeGenerateRange(mrng, biasFactor);
            const uncheckedValue = mrng.nextArrayInt(range.min, range.max);
            if (uncheckedValue.data.length === 1) {
                uncheckedValue.data.unshift(0);
            }
            return new Value_1$6.Value(uncheckedValue, undefined);
        }
        computeGenerateRange(mrng, biasFactor) {
            if (biasFactor === undefined || mrng.nextInt(1, biasFactor) !== 1) {
                return {min: this.min, max: this.max};
            }
            const ranges = this.retrieveBiasedRanges();
            if (ranges.length === 1) {
                return ranges[0];
            }
            const id = mrng.nextInt(-2 * (ranges.length - 1), ranges.length - 2);
            return id < 0 ? ranges[0] : ranges[id + 1];
        }
        canShrinkWithoutContext(value) {
            const unsafeValue = value;
            return (typeof value === 'object' && value !== null &&
                    (unsafeValue.sign === -1 || unsafeValue.sign === 1) &&
                    Array.isArray(unsafeValue.data) && unsafeValue.data.length === 2 &&
                    (((0, ArrayInt64_1$2.isStrictlySmaller64)(this.min, unsafeValue) &&
                      (0, ArrayInt64_1$2.isStrictlySmaller64)(unsafeValue, this.max)) ||
                     (0, ArrayInt64_1$2.isEqual64)(this.min, unsafeValue) ||
                     (0, ArrayInt64_1$2.isEqual64)(this.max, unsafeValue)));
        }
        shrinkArrayInt64(value, target, tryTargetAsap) {
            const realGap = (0, ArrayInt64_1$2.substract64)(value, target);
            function* shrinkGen() {
                let previous = tryTargetAsap ? undefined : target;
                const gap = tryTargetAsap ? realGap : (0, ArrayInt64_1$2.halve64)(realGap);
                for (let toremove = gap; !(0, ArrayInt64_1$2.isZero64)(toremove);
                     toremove = (0, ArrayInt64_1$2.halve64)(toremove)) {
                    const next = (0, ArrayInt64_1$2.substract64)(value, toremove);
                    yield new Value_1$6.Value(next, previous);
                    previous = next;
                }
            }
            return (0, Stream_1$4.stream)(shrinkGen());
        }
        shrink(current, context) {
            if (!ArrayInt64Arbitrary.isValidContext(current, context)) {
                const target = this.defaultTarget();
                return this.shrinkArrayInt64(current, target, true);
            }
            if (this.isLastChanceTry(current, context)) {
                return Stream_1$4.Stream.of(new Value_1$6.Value(context, undefined));
            }
            return this.shrinkArrayInt64(current, context, false);
        }
        defaultTarget() {
            if (!(0, ArrayInt64_1$2.isStrictlyPositive64)(this.min) &&
                !(0, ArrayInt64_1$2.isStrictlyNegative64)(this.max)) {
                return ArrayInt64_1$2.Zero64;
            }
            return (0, ArrayInt64_1$2.isStrictlyNegative64)(this.min) ? this.max : this.min;
        }
        isLastChanceTry(current, context) {
            if ((0, ArrayInt64_1$2.isZero64)(current)) {
                return false;
            }
            if (current.sign === 1) {
                return (0, ArrayInt64_1$2.isEqual64)(
                           current, (0, ArrayInt64_1$2.add64)(context, ArrayInt64_1$2.Unit64)) &&
                    (0, ArrayInt64_1$2.isStrictlyPositive64)(
                           (0, ArrayInt64_1$2.substract64)(current, this.min));
            } else {
                return (0, ArrayInt64_1$2.isEqual64)(
                           current,
                           (0, ArrayInt64_1$2.substract64)(context, ArrayInt64_1$2.Unit64)) &&
                    (0, ArrayInt64_1$2.isStrictlyNegative64)(
                           (0, ArrayInt64_1$2.substract64)(current, this.max));
            }
        }
        static isValidContext(_current, context) {
            if (context === undefined) {
                return false;
            }
            if (typeof context !== 'object' || context === null || !('sign' in context) ||
                !('data' in context)) {
                throw new Error(`Invalid context type passed to ArrayInt64Arbitrary (#1)`);
            }
            return true;
        }
        retrieveBiasedRanges() {
            if (this.biasedRanges != null) {
                return this.biasedRanges;
            }
            if ((0, ArrayInt64_1$2.isEqual64)(this.min, this.max)) {
                this.biasedRanges = [{min: this.min, max: this.max}];
                return this.biasedRanges;
            }
            const minStrictlySmallerZero = (0, ArrayInt64_1$2.isStrictlyNegative64)(this.min);
            const maxStrictlyGreaterZero = (0, ArrayInt64_1$2.isStrictlyPositive64)(this.max);
            if (minStrictlySmallerZero && maxStrictlyGreaterZero) {
                const logMin = (0, ArrayInt64_1$2.logLike64)(this.min);
                const logMax = (0, ArrayInt64_1$2.logLike64)(this.max);
                this.biasedRanges = [
                    {min: logMin, max: logMax},
                    {min: (0, ArrayInt64_1$2.substract64)(this.max, logMax), max: this.max},
                    {min: this.min, max: (0, ArrayInt64_1$2.substract64)(this.min, logMin)},
                ];
            } else {
                const logGap = (0, ArrayInt64_1$2.logLike64)(
                    (0, ArrayInt64_1$2.substract64)(this.max, this.min));
                const arbCloseToMin = {
                    min: this.min,
                    max: (0, ArrayInt64_1$2.add64)(this.min, logGap)
                };
                const arbCloseToMax = {
                    min: (0, ArrayInt64_1$2.substract64)(this.max, logGap),
                    max: this.max
                };
                this.biasedRanges = minStrictlySmallerZero ? [arbCloseToMax, arbCloseToMin]
                                                           : [arbCloseToMin, arbCloseToMax];
            }
            return this.biasedRanges;
        }
    }
    function arrayInt64(min, max) {
        const arb = new ArrayInt64Arbitrary(min, max);
        return arb;
    }
    ArrayInt64Arbitrary$1.arrayInt64 = arrayInt64;

    var DoubleHelpers = {};

    Object.defineProperty(DoubleHelpers, "__esModule", {value: true});
    DoubleHelpers.indexToDouble = DoubleHelpers.doubleToIndex = DoubleHelpers.decomposeDouble =
        void 0;
    const ArrayInt64_1$1 = ArrayInt64;
    const INDEX_POSITIVE_INFINITY$1 = {sign: 1, data: [2146435072, 0]};
    const INDEX_NEGATIVE_INFINITY$1 = {sign: -1, data: [2146435072, 1]};
    function decomposeDouble(d) {
        const maxSignificand = 2 - Number.EPSILON;
        for (let exponent = -1022; exponent !== 1024; ++exponent) {
            const powExponent = 2 ** exponent;
            const maxForExponent = maxSignificand * powExponent;
            if (Math.abs(d) <= maxForExponent) {
                return {exponent, significand: d / powExponent};
            }
        }
        return {exponent: Number.NaN, significand: Number.NaN};
    }
    DoubleHelpers.decomposeDouble = decomposeDouble;
    function positiveNumberToInt64(n) {
        return [~~(n / 0x100000000), n >>> 0];
    }
    function indexInDoubleFromDecomp(exponent, significand) {
        if (exponent === -1022) {
            const rescaledSignificand = significand * 2 ** 52;
            return positiveNumberToInt64(rescaledSignificand);
        }
        const rescaledSignificand = (significand - 1) * 2 ** 52;
        const exponentOnlyHigh = (exponent + 1023) * 2 ** 20;
        const index = positiveNumberToInt64(rescaledSignificand);
        index[0] += exponentOnlyHigh;
        return index;
    }
    function doubleToIndex(d) {
        if (d === Number.POSITIVE_INFINITY) {
            return (0, ArrayInt64_1$1.clone64)(INDEX_POSITIVE_INFINITY$1);
        }
        if (d === Number.NEGATIVE_INFINITY) {
            return (0, ArrayInt64_1$1.clone64)(INDEX_NEGATIVE_INFINITY$1);
        }
        const decomp = decomposeDouble(d);
        const exponent = decomp.exponent;
        const significand = decomp.significand;
        if (d > 0 || (d === 0 && 1 / d === Number.POSITIVE_INFINITY)) {
            return {sign: 1, data: indexInDoubleFromDecomp(exponent, significand)};
        } else {
            const indexOpposite = indexInDoubleFromDecomp(exponent, -significand);
            if (indexOpposite[1] === 0xffffffff) {
                indexOpposite[0] += 1;
                indexOpposite[1] = 0;
            } else {
                indexOpposite[1] += 1;
            }
            return {sign: -1, data: indexOpposite};
        }
    }
    DoubleHelpers.doubleToIndex = doubleToIndex;
    function indexToDouble(index) {
        if (index.sign === -1) {
            const indexOpposite = {sign: 1, data: [index.data[0], index.data[1]]};
            if (indexOpposite.data[1] === 0) {
                indexOpposite.data[0] -= 1;
                indexOpposite.data[1] = 0xffffffff;
            } else {
                indexOpposite.data[1] -= 1;
            }
            return -indexToDouble(indexOpposite);
        }
        if ((0, ArrayInt64_1$1.isEqual64)(index, INDEX_POSITIVE_INFINITY$1)) {
            return Number.POSITIVE_INFINITY;
        }
        if (index.data[0] < 0x200000) {
            return (index.data[0] * 0x100000000 + index.data[1]) * 2 ** -1074;
        }
        const postIndexHigh = index.data[0] - 0x200000;
        const exponent = -1021 + (postIndexHigh >> 20);
        const significand =
            1 + ((postIndexHigh & 0xfffff) * 2 ** 32 + index.data[1]) * Number.EPSILON;
        return significand * 2 ** exponent;
    }
    DoubleHelpers.indexToDouble = indexToDouble;

    Object.defineProperty(double$1, "__esModule", {value: true});
    double$1.double = void 0;
    const ArrayInt64_1 = ArrayInt64;
    const ArrayInt64Arbitrary_1 = ArrayInt64Arbitrary$1;
    const DoubleHelpers_1 = DoubleHelpers;
    function safeDoubleToIndex(d, constraintsLabel) {
        if (Number.isNaN(d)) {
            throw new Error('fc.double constraints.' + constraintsLabel +
                            ' must be a 32-bit float');
        }
        return (0, DoubleHelpers_1.doubleToIndex)(d);
    }
    function unmapperDoubleToIndex(value) {
        if (typeof value !== 'number')
            throw new Error('Unsupported type');
        return (0, DoubleHelpers_1.doubleToIndex)(value);
    }
    function double(constraints = {}) {
        const {
            noDefaultInfinity = false,
            noNaN = false,
            min = noDefaultInfinity ? -Number.MAX_VALUE : Number.NEGATIVE_INFINITY,
            max = noDefaultInfinity ? Number.MAX_VALUE : Number.POSITIVE_INFINITY,
        } = constraints;
        const minIndex = safeDoubleToIndex(min, 'min');
        const maxIndex = safeDoubleToIndex(max, 'max');
        if ((0, ArrayInt64_1.isStrictlySmaller64)(maxIndex, minIndex)) {
            throw new Error(
                'fc.double constraints.min must be smaller or equal to constraints.max');
        }
        if (noNaN) {
            return (0, ArrayInt64Arbitrary_1.arrayInt64)(minIndex, maxIndex)
                .map(DoubleHelpers_1.indexToDouble, unmapperDoubleToIndex);
        }
        const positiveMaxIdx = (0, ArrayInt64_1.isStrictlyPositive64)(maxIndex);
        const minIndexWithNaN = positiveMaxIdx
            ? minIndex
            : (0, ArrayInt64_1.substract64)(minIndex, ArrayInt64_1.Unit64);
        const maxIndexWithNaN =
            positiveMaxIdx ? (0, ArrayInt64_1.add64)(maxIndex, ArrayInt64_1.Unit64) : maxIndex;
        return (0, ArrayInt64Arbitrary_1.arrayInt64)(minIndexWithNaN, maxIndexWithNaN)
            .map(
                (index) => {
                    if ((0, ArrayInt64_1.isStrictlySmaller64)(maxIndex, index) ||
                        (0, ArrayInt64_1.isStrictlySmaller64)(index, minIndex))
                        return Number.NaN;
                    else
                        return (0, DoubleHelpers_1.indexToDouble)(index);
                },
                (value) => {
                    if (typeof value !== 'number')
                        throw new Error('Unsupported type');
                    if (Number.isNaN(value))
                        return !(0, ArrayInt64_1.isEqual64)(maxIndex, maxIndexWithNaN)
                            ? maxIndexWithNaN
                            : minIndexWithNaN;
                    return (0, DoubleHelpers_1.doubleToIndex)(value);
                });
    }
    double$1.double = double;

    var float$1 = {};

    var FloatHelpers = {};

    Object.defineProperty(FloatHelpers, "__esModule", {value: true});
    FloatHelpers.indexToFloat = FloatHelpers.floatToIndex = FloatHelpers.decomposeFloat =
        FloatHelpers.EPSILON_32 = FloatHelpers.MAX_VALUE_32 = FloatHelpers.MIN_VALUE_32 = void 0;
    FloatHelpers.MIN_VALUE_32 = 2 ** -126 * 2 ** -23;
    FloatHelpers.MAX_VALUE_32 = 2 ** 127 * (1 + (2 ** 23 - 1) / 2 ** 23);
    FloatHelpers.EPSILON_32 = 2 ** -23;
    const INDEX_POSITIVE_INFINITY = 2139095040;
    const INDEX_NEGATIVE_INFINITY = -2139095041;
    function decomposeFloat(f) {
        const maxSignificand = 1 + (2 ** 23 - 1) / 2 ** 23;
        for (let exponent = -126; exponent !== 128; ++exponent) {
            const powExponent = 2 ** exponent;
            const maxForExponent = maxSignificand * powExponent;
            if (Math.abs(f) <= maxForExponent) {
                return {exponent, significand: f / powExponent};
            }
        }
        return {exponent: Number.NaN, significand: Number.NaN};
    }
    FloatHelpers.decomposeFloat = decomposeFloat;
    function indexInFloatFromDecomp(exponent, significand) {
        if (exponent === -126) {
            return significand * 0x800000;
        }
        return (exponent + 127) * 0x800000 + (significand - 1) * 0x800000;
    }
    function floatToIndex(f) {
        if (f === Number.POSITIVE_INFINITY) {
            return INDEX_POSITIVE_INFINITY;
        }
        if (f === Number.NEGATIVE_INFINITY) {
            return INDEX_NEGATIVE_INFINITY;
        }
        const decomp = decomposeFloat(f);
        const exponent = decomp.exponent;
        const significand = decomp.significand;
        if (Number.isNaN(exponent) || Number.isNaN(significand) ||
            !Number.isInteger(significand * 0x800000)) {
            return Number.NaN;
        }
        if (f > 0 || (f === 0 && 1 / f === Number.POSITIVE_INFINITY)) {
            return indexInFloatFromDecomp(exponent, significand);
        } else {
            return -indexInFloatFromDecomp(exponent, -significand) - 1;
        }
    }
    FloatHelpers.floatToIndex = floatToIndex;
    function indexToFloat(index) {
        if (index < 0) {
            return -indexToFloat(-index - 1);
        }
        if (index === INDEX_POSITIVE_INFINITY) {
            return Number.POSITIVE_INFINITY;
        }
        if (index < 0x1000000) {
            return index * 2 ** -149;
        }
        const postIndex = index - 0x1000000;
        const exponent = -125 + (postIndex >> 23);
        const significand = 1 + (postIndex & 0x7fffff) / 0x800000;
        return significand * 2 ** exponent;
    }
    FloatHelpers.indexToFloat = indexToFloat;

    Object.defineProperty(float$1, "__esModule", {value: true});
    float$1.float = void 0;
    const integer_1$b = integer$1;
    const FloatHelpers_1 = FloatHelpers;
    function safeFloatToIndex(f, constraintsLabel) {
        const conversionTrick =
            'you can convert any double to a 32-bit float by using `new Float32Array([myDouble])[0]`';
        const errorMessage = 'fc.float constraints.' + constraintsLabel +
            ' must be a 32-bit float - ' + conversionTrick;
        if (Number.isNaN(f) ||
            (Number.isFinite(f) &&
             (f < -FloatHelpers_1.MAX_VALUE_32 || f > FloatHelpers_1.MAX_VALUE_32))) {
            throw new Error(errorMessage);
        }
        const index = (0, FloatHelpers_1.floatToIndex)(f);
        if (!Number.isInteger(index)) {
            throw new Error(errorMessage);
        }
        return index;
    }
    function unmapperFloatToIndex(value) {
        if (typeof value !== 'number')
            throw new Error('Unsupported type');
        return (0, FloatHelpers_1.floatToIndex)(value);
    }
    function float(constraints = {}) {
        const {
            noDefaultInfinity = false,
            noNaN = false,
            min = noDefaultInfinity ? -FloatHelpers_1.MAX_VALUE_32 : Number.NEGATIVE_INFINITY,
            max = noDefaultInfinity ? FloatHelpers_1.MAX_VALUE_32 : Number.POSITIVE_INFINITY,
        } = constraints;
        const minIndex = safeFloatToIndex(min, 'min');
        const maxIndex = safeFloatToIndex(max, 'max');
        if (minIndex > maxIndex) {
            throw new Error('fc.float constraints.min must be smaller or equal to constraints.max');
        }
        if (noNaN) {
            return (0, integer_1$b.integer)({min: minIndex, max: maxIndex})
                .map(FloatHelpers_1.indexToFloat, unmapperFloatToIndex);
        }
        const minIndexWithNaN = maxIndex > 0 ? minIndex : minIndex - 1;
        const maxIndexWithNaN = maxIndex > 0 ? maxIndex + 1 : maxIndex;
        return (0, integer_1$b.integer)({min: minIndexWithNaN, max: maxIndexWithNaN})
            .map(
                (index) => {
                    if (index > maxIndex || index < minIndex)
                        return Number.NaN;
                    else
                        return (0, FloatHelpers_1.indexToFloat)(index);
                },
                (value) => {
                    if (typeof value !== 'number')
                        throw new Error('Unsupported type');
                    if (Number.isNaN(value))
                        return maxIndex !== maxIndexWithNaN ? maxIndexWithNaN : minIndexWithNaN;
                    return (0, FloatHelpers_1.floatToIndex)(value);
                });
    }
    float$1.float = float;

    var compareBooleanFunc$1 = {};

    var CompareFunctionArbitraryBuilder = {};

    var TextEscaper = {};

    Object.defineProperty(TextEscaper, "__esModule", {value: true});
    TextEscaper.escapeForMultilineComments = TextEscaper.escapeForTemplateString = void 0;
    function escapeForTemplateString(originalText) {
        return originalText.replace(/([$`\\])/g, '\\$1').replace(/\r/g, '\\r');
    }
    TextEscaper.escapeForTemplateString = escapeForTemplateString;
    function escapeForMultilineComments(originalText) {
        return originalText.replace(/\*\//g, '*\\/');
    }
    TextEscaper.escapeForMultilineComments = escapeForMultilineComments;

    var hash$1 = {};

    Object.defineProperty(hash$1, "__esModule", {value: true});
    hash$1.hash = void 0;
    const crc32Table = [
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535,
        0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd,
        0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d,
        0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
        0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4,
        0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac,
        0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
        0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab,
        0xb6662d3d, 0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f,
        0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb,
        0x086d3d2d, 0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea,
        0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65, 0x4db26158, 0x3ab551ce,
        0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a,
        0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
        0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409,
        0xce61e49f, 0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739,
        0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
        0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 0xf00f9344, 0x8708a3d2, 0x1e01f268,
        0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0,
        0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8,
        0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef,
        0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703,
        0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7,
        0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
        0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae,
        0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 0x88085ae6,
        0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
        0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d,
        0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5,
        0x47b2cf7f, 0x30b5ffe9, 0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605,
        0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
    ];
    function hash(repr) {
        let crc = 0xffffffff;
        for (let idx = 0; idx < repr.length; ++idx) {
            const c = repr.charCodeAt(idx);
            if (c < 0x80) {
                crc = crc32Table[(crc & 0xff) ^ c] ^ (crc >> 8);
            } else if (c < 0x800) {
                crc = crc32Table[(crc & 0xff) ^ (192 | ((c >> 6) & 31))] ^ (crc >> 8);
                crc = crc32Table[(crc & 0xff) ^ (128 | (c & 63))] ^ (crc >> 8);
            } else if (c >= 0xd800 && c < 0xe000) {
                const cNext = repr.charCodeAt(++idx);
                if (c >= 0xdc00 || cNext < 0xdc00 || cNext > 0xdfff || Number.isNaN(cNext)) {
                    idx -= 1;
                    crc = crc32Table[(crc & 0xff) ^ 0xef] ^ (crc >> 8);
                    crc = crc32Table[(crc & 0xff) ^ 0xbf] ^ (crc >> 8);
                    crc = crc32Table[(crc & 0xff) ^ 0xbd] ^ (crc >> 8);
                } else {
                    const c1 = (c & 1023) + 64;
                    const c2 = cNext & 1023;
                    crc = crc32Table[(crc & 0xff) ^ (240 | ((c1 >> 8) & 7))] ^ (crc >> 8);
                    crc = crc32Table[(crc & 0xff) ^ (128 | ((c1 >> 2) & 63))] ^ (crc >> 8);
                    crc = crc32Table[(crc & 0xff) ^ (128 | ((c2 >> 6) & 15) | ((c1 & 3) << 4))] ^
                        (crc >> 8);
                    crc = crc32Table[(crc & 0xff) ^ (128 | (c2 & 63))] ^ (crc >> 8);
                }
            } else {
                crc = crc32Table[(crc & 0xff) ^ (224 | ((c >> 12) & 15))] ^ (crc >> 8);
                crc = crc32Table[(crc & 0xff) ^ (128 | ((c >> 6) & 63))] ^ (crc >> 8);
                crc = crc32Table[(crc & 0xff) ^ (128 | (c & 63))] ^ (crc >> 8);
            }
        }
        return (crc | 0) + 0x80000000;
    }
    hash$1.hash = hash;

    Object.defineProperty(CompareFunctionArbitraryBuilder, "__esModule", {value: true});
    CompareFunctionArbitraryBuilder.buildCompareFunctionArbitrary = void 0;
    const TextEscaper_1$2 = TextEscaper;
    const symbols_1$5 = symbols;
    const hash_1$1 = hash$1;
    const stringify_1$5 = stringify;
    const integer_1$a = integer$1;
    const tuple_1$c = tuple$1;
    function buildCompareFunctionArbitrary(cmp) {
        return (0, tuple_1$c.tuple)((0, integer_1$a.integer)().noShrink(),
                                    (0, integer_1$a.integer)({min: 1, max: 0xffffffff}).noShrink())
            .map(([seed, hashEnvSize]) => {
                const producer = () => {
                    const recorded = {};
                    const f = (a, b) => {
                        const reprA = (0, stringify_1$5.stringify)(a);
                        const reprB = (0, stringify_1$5.stringify)(b);
                        const hA = (0, hash_1$1.hash)(`${seed}${reprA}`) % hashEnvSize;
                        const hB = (0, hash_1$1.hash)(`${seed}${reprB}`) % hashEnvSize;
                        const val = cmp(hA, hB);
                        recorded[`[${reprA},${reprB}]`] = val;
                        return val;
                    };
                    return Object.assign(f, {
                        toString: () => {
                            const seenValues =
                                Object.keys(recorded)
                                    .sort()
                                    .map((k) =>
                                             `${k} => ${(0, stringify_1$5.stringify)(recorded[k])}`)
                                    .map((line) => `/* ${
                                             (0, TextEscaper_1$2.escapeForMultilineComments)(
                                                 line)} */`);
                            return `function(a, b) {
  // With hash and stringify coming from fast-check${seenValues.length !== 0 ? `\n  ${seenValues.join('\n  ')}` : ''}
  const cmp = ${cmp};
  const hA = hash('${seed}' + stringify(a)) % ${hashEnvSize};
  const hB = hash('${seed}' + stringify(b)) % ${hashEnvSize};
  return cmp(hA, hB);
}`;
                        },
                        [symbols_1$5.cloneMethod]: producer,
                    });
                };
                return producer();
            });
    }
    CompareFunctionArbitraryBuilder.buildCompareFunctionArbitrary = buildCompareFunctionArbitrary;

    Object.defineProperty(compareBooleanFunc$1, "__esModule", {value: true});
    compareBooleanFunc$1.compareBooleanFunc = void 0;
    const CompareFunctionArbitraryBuilder_1$1 = CompareFunctionArbitraryBuilder;
    function compareBooleanFunc() {
        return (0, CompareFunctionArbitraryBuilder_1$1.buildCompareFunctionArbitrary)(
            Object.assign((hA, hB) => hA < hB, {
                toString() {
                    return '(hA, hB) => hA < hB';
                },
            }));
    }
    compareBooleanFunc$1.compareBooleanFunc = compareBooleanFunc;

    var compareFunc$1 = {};

    Object.defineProperty(compareFunc$1, "__esModule", {value: true});
    compareFunc$1.compareFunc = void 0;
    const CompareFunctionArbitraryBuilder_1 = CompareFunctionArbitraryBuilder;
    function compareFunc() {
        return (0, CompareFunctionArbitraryBuilder_1.buildCompareFunctionArbitrary)(
            Object.assign((hA, hB) => hA - hB, {
                toString() {
                    return '(hA, hB) => hA - hB';
                },
            }));
    }
    compareFunc$1.compareFunc = compareFunc;

    var func$1 = {};

    Object.defineProperty(func$1, "__esModule", {value: true});
    func$1.func = void 0;
    const hash_1 = hash$1;
    const stringify_1$4 = stringify;
    const symbols_1$4 = symbols;
    const array_1$e = array$1;
    const integer_1$9 = integer$1;
    const tuple_1$b = tuple$1;
    const TextEscaper_1$1 = TextEscaper;
    function func(arb) {
        return (0, tuple_1$b.tuple)((0, array_1$e.array)(arb, {minLength: 1}),
                                    (0, integer_1$9.integer)().noShrink())
            .map(([outs, seed]) => {
                const producer = () => {
                    const recorded = {};
                    const f = (...args) => {
                        const repr = (0, stringify_1$4.stringify)(args);
                        const val = outs[(0, hash_1.hash)(`${seed}${repr}`) % outs.length];
                        recorded[repr] = val;
                        return (0, symbols_1$4.hasCloneMethod)(val) ? val[symbols_1$4.cloneMethod]()
                                                                    : val;
                    };
                    function prettyPrint(stringifiedOuts) {
                        const seenValues =
                            Object.keys(recorded)
                                .sort()
                                .map((k) => `${k} => ${(0, stringify_1$4.stringify)(recorded[k])}`)
                                .map(
                                    (line) => `/* ${
                                        (0, TextEscaper_1$1.escapeForMultilineComments)(line)} */`);
                        return `function(...args) {
  // With hash and stringify coming from fast-check${seenValues.length !== 0 ? `\n  ${seenValues.join('\n  ')}` : ''}
  const outs = ${stringifiedOuts};
  return outs[hash('${seed}' + stringify(args)) % outs.length];
}`;
                    }
                    return Object.defineProperties(f, {
                        toString: {value: () => prettyPrint((0, stringify_1$4.stringify)(outs))},
                        [stringify_1$4.toStringMethod]:
                            {value: () => prettyPrint((0, stringify_1$4.stringify)(outs))},
                        [stringify_1$4.asyncToStringMethod]: {
                            value: async () =>
                                prettyPrint(await (0, stringify_1$4.asyncStringify)(outs))
                        },
                        [symbols_1$4.cloneMethod]: {value: producer, configurable: true},
                    });
                };
                return producer();
            });
    }
    func$1.func = func;

    var maxSafeInteger$1 = {};

    Object.defineProperty(maxSafeInteger$1, "__esModule", {value: true});
    maxSafeInteger$1.maxSafeInteger = void 0;
    const IntegerArbitrary_1$2 = IntegerArbitrary$1;
    function maxSafeInteger() {
        return new IntegerArbitrary_1$2.IntegerArbitrary(Number.MIN_SAFE_INTEGER,
                                                         Number.MAX_SAFE_INTEGER);
    }
    maxSafeInteger$1.maxSafeInteger = maxSafeInteger;

    var maxSafeNat$1 = {};

    Object.defineProperty(maxSafeNat$1, "__esModule", {value: true});
    maxSafeNat$1.maxSafeNat = void 0;
    const IntegerArbitrary_1$1 = IntegerArbitrary$1;
    function maxSafeNat() {
        return new IntegerArbitrary_1$1.IntegerArbitrary(0, Number.MAX_SAFE_INTEGER);
    }
    maxSafeNat$1.maxSafeNat = maxSafeNat;

    var ipV4$1 = {};

    var NatToStringifiedNat = {};

    Object.defineProperty(NatToStringifiedNat, "__esModule", {value: true});
    NatToStringifiedNat.natToStringifiedNatUnmapper = NatToStringifiedNat.tryParseStringifiedNat =
        NatToStringifiedNat.natToStringifiedNatMapper = void 0;
    function natToStringifiedNatMapper(options) {
        const [style, v] = options;
        switch (style) {
            case 'oct':
                return `0${Number(v).toString(8)}`;
            case 'hex':
                return `0x${Number(v).toString(16)}`;
            case 'dec':
            default:
                return `${v}`;
        }
    }
    NatToStringifiedNat.natToStringifiedNatMapper = natToStringifiedNatMapper;
    function tryParseStringifiedNat(stringValue, radix) {
        const parsedNat = Number.parseInt(stringValue, radix);
        if (parsedNat.toString(radix) !== stringValue) {
            throw new Error('Invalid value');
        }
        return parsedNat;
    }
    NatToStringifiedNat.tryParseStringifiedNat = tryParseStringifiedNat;
    function natToStringifiedNatUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Invalid type');
        }
        if (value.length >= 2 && value[0] === '0') {
            if (value[1] === 'x') {
                return ['hex', tryParseStringifiedNat(value.substr(2), 16)];
            }
            return ['oct', tryParseStringifiedNat(value.substr(1), 8)];
        }
        return ['dec', tryParseStringifiedNat(value, 10)];
    }
    NatToStringifiedNat.natToStringifiedNatUnmapper = natToStringifiedNatUnmapper;

    Object.defineProperty(ipV4$1, "__esModule", {value: true});
    ipV4$1.ipV4 = void 0;
    const nat_1$2 = nat$1;
    const tuple_1$a = tuple$1;
    const NatToStringifiedNat_1$1 = NatToStringifiedNat;
    function dotJoinerMapper$1(data) {
        return data.join('.');
    }
    function dotJoinerUnmapper$1(value) {
        if (typeof value !== 'string') {
            throw new Error('Invalid type');
        }
        return value.split('.').map((v) =>
                                        (0, NatToStringifiedNat_1$1.tryParseStringifiedNat)(v, 10));
    }
    function ipV4() {
        return (0, tuple_1$a.tuple)((0, nat_1$2.nat)(255),
                                    (0, nat_1$2.nat)(255),
                                    (0, nat_1$2.nat)(255),
                                    (0, nat_1$2.nat)(255))
            .map(dotJoinerMapper$1, dotJoinerUnmapper$1);
    }
    ipV4$1.ipV4 = ipV4;

    var ipV4Extended$1 = {};

    var StringifiedNatArbitraryBuilder = {};

    Object.defineProperty(StringifiedNatArbitraryBuilder, "__esModule", {value: true});
    StringifiedNatArbitraryBuilder.buildStringifiedNatArbitrary = void 0;
    const constantFrom_1$1 = constantFrom$1;
    const nat_1$1 = nat$1;
    const tuple_1$9 = tuple$1;
    const NatToStringifiedNat_1 = NatToStringifiedNat;
    function buildStringifiedNatArbitrary(maxValue) {
        return (0, tuple_1$9.tuple)((0, constantFrom_1$1.constantFrom)('dec', 'oct', 'hex'),
                                    (0, nat_1$1.nat)(maxValue))
            .map(NatToStringifiedNat_1.natToStringifiedNatMapper,
                 NatToStringifiedNat_1.natToStringifiedNatUnmapper);
    }
    StringifiedNatArbitraryBuilder.buildStringifiedNatArbitrary = buildStringifiedNatArbitrary;

    Object.defineProperty(ipV4Extended$1, "__esModule", {value: true});
    ipV4Extended$1.ipV4Extended = void 0;
    const oneof_1$6 = oneof$1;
    const tuple_1$8 = tuple$1;
    const StringifiedNatArbitraryBuilder_1 = StringifiedNatArbitraryBuilder;
    function dotJoinerMapper(data) {
        return data.join('.');
    }
    function dotJoinerUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Invalid type');
        }
        return value.split('.');
    }
    function ipV4Extended() {
        return (0, oneof_1$6.oneof)(
            (0, tuple_1$8.tuple)(
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255))
                .map(dotJoinerMapper, dotJoinerUnmapper),
            (0, tuple_1$8.tuple)(
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(65535))
                .map(dotJoinerMapper, dotJoinerUnmapper),
            (0, tuple_1$8.tuple)(
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(255),
                (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(16777215))
                .map(dotJoinerMapper, dotJoinerUnmapper),
            (0, StringifiedNatArbitraryBuilder_1.buildStringifiedNatArbitrary)(4294967295));
    }
    ipV4Extended$1.ipV4Extended = ipV4Extended;

    var ipV6$1 = {};

    var hexaString$1 = {};

    var CodePointsToString = {};

    Object.defineProperty(CodePointsToString, "__esModule", {value: true});
    CodePointsToString.codePointsToStringUnmapper = CodePointsToString.codePointsToStringMapper =
        void 0;
    function codePointsToStringMapper(tab) {
        return tab.join('');
    }
    CodePointsToString.codePointsToStringMapper = codePointsToStringMapper;
    function codePointsToStringUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Cannot unmap the passed value');
        }
        return [...value];
    }
    CodePointsToString.codePointsToStringUnmapper = codePointsToStringUnmapper;

    Object.defineProperty(hexaString$1, "__esModule", {value: true});
    hexaString$1.hexaString = void 0;
    const array_1$d = array$1;
    const hexa_1 = hexa$1;
    const CodePointsToString_1$5 = CodePointsToString;
    const SlicesForStringBuilder_1$6 = SlicesForStringBuilder;
    function hexaString(constraints = {}) {
        const charArbitrary = (0, hexa_1.hexa)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$6.createSlicesForString)(
            charArbitrary, CodePointsToString_1$5.codePointsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$d.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1$5.codePointsToStringMapper,
                 CodePointsToString_1$5.codePointsToStringUnmapper);
    }
    hexaString$1.hexaString = hexaString;

    var EntitiesToIPv6 = {};

    Object.defineProperty(EntitiesToIPv6, "__esModule", {value: true});
    EntitiesToIPv6.noTrailingUnmapper = EntitiesToIPv6.noTrailingMapper =
        EntitiesToIPv6.singleTrailingUnmapper = EntitiesToIPv6.singleTrailingMapper =
            EntitiesToIPv6.multiTrailingUnmapperOne = EntitiesToIPv6.multiTrailingMapperOne =
                EntitiesToIPv6.multiTrailingUnmapper = EntitiesToIPv6.multiTrailingMapper =
                    EntitiesToIPv6.onlyTrailingUnmapper = EntitiesToIPv6.onlyTrailingMapper =
                        EntitiesToIPv6.fullySpecifiedUnmapper =
                            EntitiesToIPv6.fullySpecifiedMapper = void 0;
    function readBh(value) {
        if (value.length === 0)
            return [];
        else
            return value.split(':');
    }
    function extractEhAndL(value) {
        const valueSplits = value.split(':');
        if (valueSplits.length >= 2 && valueSplits[valueSplits.length - 1].length <= 4) {
            return [
                valueSplits.slice(0, valueSplits.length - 2),
                `${valueSplits[valueSplits.length - 2]}:${valueSplits[valueSplits.length - 1]}`,
            ];
        }
        return [valueSplits.slice(0, valueSplits.length - 1), valueSplits[valueSplits.length - 1]];
    }
    function fullySpecifiedMapper(data) {
        return `${data[0].join(':')}:${data[1]}`;
    }
    EntitiesToIPv6.fullySpecifiedMapper = fullySpecifiedMapper;
    function fullySpecifiedUnmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        return extractEhAndL(value);
    }
    EntitiesToIPv6.fullySpecifiedUnmapper = fullySpecifiedUnmapper;
    function onlyTrailingMapper(data) {
        return `::${data[0].join(':')}:${data[1]}`;
    }
    EntitiesToIPv6.onlyTrailingMapper = onlyTrailingMapper;
    function onlyTrailingUnmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        if (!value.startsWith('::'))
            throw new Error('Invalid value');
        return extractEhAndL(value.substring(2));
    }
    EntitiesToIPv6.onlyTrailingUnmapper = onlyTrailingUnmapper;
    function multiTrailingMapper(data) {
        return `${data[0].join(':')}::${data[1].join(':')}:${data[2]}`;
    }
    EntitiesToIPv6.multiTrailingMapper = multiTrailingMapper;
    function multiTrailingUnmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        const [bhString, trailingString] = value.split('::', 2);
        const [eh, l] = extractEhAndL(trailingString);
        return [readBh(bhString), eh, l];
    }
    EntitiesToIPv6.multiTrailingUnmapper = multiTrailingUnmapper;
    function multiTrailingMapperOne(data) {
        return multiTrailingMapper([data[0], [data[1]], data[2]]);
    }
    EntitiesToIPv6.multiTrailingMapperOne = multiTrailingMapperOne;
    function multiTrailingUnmapperOne(value) {
        const out = multiTrailingUnmapper(value);
        return [out[0], out[1].join(':'), out[2]];
    }
    EntitiesToIPv6.multiTrailingUnmapperOne = multiTrailingUnmapperOne;
    function singleTrailingMapper(data) {
        return `${data[0].join(':')}::${data[1]}`;
    }
    EntitiesToIPv6.singleTrailingMapper = singleTrailingMapper;
    function singleTrailingUnmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        const [bhString, trailing] = value.split('::', 2);
        return [readBh(bhString), trailing];
    }
    EntitiesToIPv6.singleTrailingUnmapper = singleTrailingUnmapper;
    function noTrailingMapper(data) {
        return `${data[0].join(':')}::`;
    }
    EntitiesToIPv6.noTrailingMapper = noTrailingMapper;
    function noTrailingUnmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        if (!value.endsWith('::'))
            throw new Error('Invalid value');
        return [readBh(value.substring(0, value.length - 2))];
    }
    EntitiesToIPv6.noTrailingUnmapper = noTrailingUnmapper;

    Object.defineProperty(ipV6$1, "__esModule", {value: true});
    ipV6$1.ipV6 = void 0;
    const array_1$c = array$1;
    const oneof_1$5 = oneof$1;
    const hexaString_1 = hexaString$1;
    const tuple_1$7 = tuple$1;
    const ipV4_1$1 = ipV4$1;
    const EntitiesToIPv6_1 = EntitiesToIPv6;
    function h16sTol32Mapper([a, b]) {
        return `${a}:${b}`;
    }
    function h16sTol32Unmapper(value) {
        if (typeof value !== 'string')
            throw new Error('Invalid type');
        if (!value.includes(':'))
            throw new Error('Invalid value');
        return value.split(':', 2);
    }
    function ipV6() {
        const h16Arb = (0, hexaString_1.hexaString)({minLength: 1, maxLength: 4, size: 'max'});
        const ls32Arb = (0, oneof_1$5.oneof)(
            (0, tuple_1$7.tuple)(h16Arb, h16Arb).map(h16sTol32Mapper, h16sTol32Unmapper),
            (0, ipV4_1$1.ipV4)());
        return (0, oneof_1$5.oneof)(
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 6, maxLength: 6, size: 'max'}), ls32Arb)
                .map(EntitiesToIPv6_1.fullySpecifiedMapper,
                     EntitiesToIPv6_1.fullySpecifiedUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 5, maxLength: 5, size: 'max'}), ls32Arb)
                .map(EntitiesToIPv6_1.onlyTrailingMapper, EntitiesToIPv6_1.onlyTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 1, size: 'max'}),
                (0, array_1$c.array)(h16Arb, {minLength: 4, maxLength: 4, size: 'max'}),
                ls32Arb)
                .map(EntitiesToIPv6_1.multiTrailingMapper, EntitiesToIPv6_1.multiTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 2, size: 'max'}),
                (0, array_1$c.array)(h16Arb, {minLength: 3, maxLength: 3, size: 'max'}),
                ls32Arb)
                .map(EntitiesToIPv6_1.multiTrailingMapper, EntitiesToIPv6_1.multiTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 3, size: 'max'}),
                (0, array_1$c.array)(h16Arb, {minLength: 2, maxLength: 2, size: 'max'}),
                ls32Arb)
                .map(EntitiesToIPv6_1.multiTrailingMapper, EntitiesToIPv6_1.multiTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 4, size: 'max'}),
                h16Arb,
                ls32Arb)
                .map(EntitiesToIPv6_1.multiTrailingMapperOne,
                     EntitiesToIPv6_1.multiTrailingUnmapperOne),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 5, size: 'max'}), ls32Arb)
                .map(EntitiesToIPv6_1.singleTrailingMapper,
                     EntitiesToIPv6_1.singleTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 6, size: 'max'}), h16Arb)
                .map(EntitiesToIPv6_1.singleTrailingMapper,
                     EntitiesToIPv6_1.singleTrailingUnmapper),
            (0, tuple_1$7.tuple)(
                (0, array_1$c.array)(h16Arb, {minLength: 0, maxLength: 7, size: 'max'}))
                .map(EntitiesToIPv6_1.noTrailingMapper, EntitiesToIPv6_1.noTrailingUnmapper));
    }
    ipV6$1.ipV6 = ipV6;

    var letrec$1 = {};

    var LazyArbitrary$1 = {};

    Object.defineProperty(LazyArbitrary$1, "__esModule", {value: true});
    LazyArbitrary$1.LazyArbitrary = void 0;
    const Arbitrary_1$6 = Arbitrary$1;
    class LazyArbitrary extends Arbitrary_1$6.Arbitrary {
        constructor(name) {
            super();
            this.name = name;
            this.underlying = null;
        }
        generate(mrng, biasFactor) {
            if (!this.underlying) {
                throw new Error(
                    `Lazy arbitrary ${JSON.stringify(this.name)} not correctly initialized`);
            }
            return this.underlying.generate(mrng, biasFactor);
        }
        canShrinkWithoutContext(value) {
            if (!this.underlying) {
                throw new Error(
                    `Lazy arbitrary ${JSON.stringify(this.name)} not correctly initialized`);
            }
            return this.underlying.canShrinkWithoutContext(value);
        }
        shrink(value, context) {
            if (!this.underlying) {
                throw new Error(
                    `Lazy arbitrary ${JSON.stringify(this.name)} not correctly initialized`);
            }
            return this.underlying.shrink(value, context);
        }
    }
    LazyArbitrary$1.LazyArbitrary = LazyArbitrary;

    Object.defineProperty(letrec$1, "__esModule", {value: true});
    letrec$1.letrec = void 0;
    const LazyArbitrary_1 = LazyArbitrary$1;
    function letrec(builder) {
        const lazyArbs = Object.create(null);
        const tie = (key) => {
            if (!Object.prototype.hasOwnProperty.call(lazyArbs, key)) {
                lazyArbs[key] = new LazyArbitrary_1.LazyArbitrary(String(key));
            }
            return lazyArbs[key];
        };
        const strictArbs = builder(tie);
        for (const key in strictArbs) {
            if (!Object.prototype.hasOwnProperty.call(strictArbs, key)) {
                continue;
            }
            const lazyAtKey = lazyArbs[key];
            const lazyArb =
                lazyAtKey !== undefined ? lazyAtKey : new LazyArbitrary_1.LazyArbitrary(key);
            lazyArb.underlying = strictArbs[key];
            lazyArbs[key] = lazyArb;
        }
        return strictArbs;
    }
    letrec$1.letrec = letrec;

    var lorem$1 = {};

    var WordsToLorem = {};

    Object.defineProperty(WordsToLorem, "__esModule", {value: true});
    WordsToLorem.sentencesToParagraphUnmapper = WordsToLorem.sentencesToParagraphMapper =
        WordsToLorem.wordsToSentenceUnmapperFor = WordsToLorem.wordsToSentenceMapper =
            WordsToLorem.wordsToJoinedStringUnmapperFor = WordsToLorem.wordsToJoinedStringMapper =
                void 0;
    function wordsToJoinedStringMapper(words) {
        return words.map((w) => (w[w.length - 1] === ',' ? w.substr(0, w.length - 1) : w))
            .join(' ');
    }
    WordsToLorem.wordsToJoinedStringMapper = wordsToJoinedStringMapper;
    function wordsToJoinedStringUnmapperFor(wordsArbitrary) {
        return function wordsToJoinedStringUnmapper(value) {
            if (typeof value !== 'string') {
                throw new Error('Unsupported type');
            }
            const words = [];
            for (const candidate of value.split(' ')) {
                if (wordsArbitrary.canShrinkWithoutContext(candidate))
                    words.push(candidate);
                else if (wordsArbitrary.canShrinkWithoutContext(candidate + ','))
                    words.push(candidate + ',');
                else
                    throw new Error('Unsupported word');
            }
            return words;
        };
    }
    WordsToLorem.wordsToJoinedStringUnmapperFor = wordsToJoinedStringUnmapperFor;
    function wordsToSentenceMapper(words) {
        let sentence = words.join(' ');
        if (sentence[sentence.length - 1] === ',') {
            sentence = sentence.substr(0, sentence.length - 1);
        }
        return sentence[0].toUpperCase() + sentence.substring(1) + '.';
    }
    WordsToLorem.wordsToSentenceMapper = wordsToSentenceMapper;
    function wordsToSentenceUnmapperFor(wordsArbitrary) {
        return function wordsToSentenceUnmapper(value) {
            if (typeof value !== 'string') {
                throw new Error('Unsupported type');
            }
            if (value.length < 2 || value[value.length - 1] !== '.' ||
                value[value.length - 2] === ',' ||
                value[0].toLowerCase().toUpperCase() !== value[0]) {
                throw new Error('Unsupported value');
            }
            const adaptedValue = value[0].toLowerCase() + value.substring(1, value.length - 1);
            const words = [];
            const candidates = adaptedValue.split(' ');
            for (let idx = 0; idx !== candidates.length; ++idx) {
                const candidate = candidates[idx];
                if (wordsArbitrary.canShrinkWithoutContext(candidate))
                    words.push(candidate);
                else if (idx === candidates.length - 1 &&
                         wordsArbitrary.canShrinkWithoutContext(candidate + ','))
                    words.push(candidate + ',');
                else
                    throw new Error('Unsupported word');
            }
            return words;
        };
    }
    WordsToLorem.wordsToSentenceUnmapperFor = wordsToSentenceUnmapperFor;
    function sentencesToParagraphMapper(sentences) {
        return sentences.join(' ');
    }
    WordsToLorem.sentencesToParagraphMapper = sentencesToParagraphMapper;
    function sentencesToParagraphUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported type');
        }
        const sentences = value.split('. ');
        for (let idx = 0; idx < sentences.length - 1; ++idx) {
            sentences[idx] += '.';
        }
        return sentences;
    }
    WordsToLorem.sentencesToParagraphUnmapper = sentencesToParagraphUnmapper;

    Object.defineProperty(lorem$1, "__esModule", {value: true});
    lorem$1.lorem = void 0;
    const array_1$b = array$1;
    const constant_1$4 = constant$1;
    const oneof_1$4 = oneof$1;
    const WordsToLorem_1 = WordsToLorem;
    const h = (v, w) => {
        return {arbitrary: (0, constant_1$4.constant)(v), weight: w};
    };
    function loremWord() {
        return (0, oneof_1$4.oneof)(h('non', 6),
                                    h('adipiscing', 5),
                                    h('ligula', 5),
                                    h('enim', 5),
                                    h('pellentesque', 5),
                                    h('in', 5),
                                    h('augue', 5),
                                    h('et', 5),
                                    h('nulla', 5),
                                    h('lorem', 4),
                                    h('sit', 4),
                                    h('sed', 4),
                                    h('diam', 4),
                                    h('fermentum', 4),
                                    h('ut', 4),
                                    h('eu', 4),
                                    h('aliquam', 4),
                                    h('mauris', 4),
                                    h('vitae', 4),
                                    h('felis', 4),
                                    h('ipsum', 3),
                                    h('dolor', 3),
                                    h('amet,', 3),
                                    h('elit', 3),
                                    h('euismod', 3),
                                    h('mi', 3),
                                    h('orci', 3),
                                    h('erat', 3),
                                    h('praesent', 3),
                                    h('egestas', 3),
                                    h('leo', 3),
                                    h('vel', 3),
                                    h('sapien', 3),
                                    h('integer', 3),
                                    h('curabitur', 3),
                                    h('convallis', 3),
                                    h('purus', 3),
                                    h('risus', 2),
                                    h('suspendisse', 2),
                                    h('lectus', 2),
                                    h('nec,', 2),
                                    h('ultricies', 2),
                                    h('sed,', 2),
                                    h('cras', 2),
                                    h('elementum', 2),
                                    h('ultrices', 2),
                                    h('maecenas', 2),
                                    h('massa,', 2),
                                    h('varius', 2),
                                    h('a,', 2),
                                    h('semper', 2),
                                    h('proin', 2),
                                    h('nec', 2),
                                    h('nisl', 2),
                                    h('amet', 2),
                                    h('duis', 2),
                                    h('congue', 2),
                                    h('libero', 2),
                                    h('vestibulum', 2),
                                    h('pede', 2),
                                    h('blandit', 2),
                                    h('sodales', 2),
                                    h('ante', 2),
                                    h('nibh', 2),
                                    h('ac', 2),
                                    h('aenean', 2),
                                    h('massa', 2),
                                    h('suscipit', 2),
                                    h('sollicitudin', 2),
                                    h('fusce', 2),
                                    h('tempus', 2),
                                    h('aliquam,', 2),
                                    h('nunc', 2),
                                    h('ullamcorper', 2),
                                    h('rhoncus', 2),
                                    h('metus', 2),
                                    h('faucibus,', 2),
                                    h('justo', 2),
                                    h('magna', 2),
                                    h('at', 2),
                                    h('tincidunt', 2),
                                    h('consectetur', 1),
                                    h('tortor,', 1),
                                    h('dignissim', 1),
                                    h('congue,', 1),
                                    h('non,', 1),
                                    h('porttitor,', 1),
                                    h('nonummy', 1),
                                    h('molestie,', 1),
                                    h('est', 1),
                                    h('eleifend', 1),
                                    h('mi,', 1),
                                    h('arcu', 1),
                                    h('scelerisque', 1),
                                    h('vitae,', 1),
                                    h('consequat', 1),
                                    h('in,', 1),
                                    h('pretium', 1),
                                    h('volutpat', 1),
                                    h('pharetra', 1),
                                    h('tempor', 1),
                                    h('bibendum', 1),
                                    h('odio', 1),
                                    h('dui', 1),
                                    h('primis', 1),
                                    h('faucibus', 1),
                                    h('luctus', 1),
                                    h('posuere', 1),
                                    h('cubilia', 1),
                                    h('curae,', 1),
                                    h('hendrerit', 1),
                                    h('velit', 1),
                                    h('mauris,', 1),
                                    h('gravida', 1),
                                    h('ornare', 1),
                                    h('ut,', 1),
                                    h('pulvinar', 1),
                                    h('varius,', 1),
                                    h('turpis', 1),
                                    h('nibh,', 1),
                                    h('eros', 1),
                                    h('id', 1),
                                    h('aliquet', 1),
                                    h('quis', 1),
                                    h('lobortis', 1),
                                    h('consectetuer', 1),
                                    h('morbi', 1),
                                    h('vehicula', 1),
                                    h('tortor', 1),
                                    h('tellus,', 1),
                                    h('id,', 1),
                                    h('eu,', 1),
                                    h('quam', 1),
                                    h('feugiat,', 1),
                                    h('posuere,', 1),
                                    h('iaculis', 1),
                                    h('lectus,', 1),
                                    h('tristique', 1),
                                    h('mollis,', 1),
                                    h('nisl,', 1),
                                    h('vulputate', 1),
                                    h('sem', 1),
                                    h('vivamus', 1),
                                    h('placerat', 1),
                                    h('imperdiet', 1),
                                    h('cursus', 1),
                                    h('rutrum', 1),
                                    h('iaculis,', 1),
                                    h('augue,', 1),
                                    h('lacus', 1));
    }
    function lorem(constraints = {}) {
        const {maxCount, mode = 'words', size} = constraints;
        if (maxCount !== undefined && maxCount < 1) {
            throw new Error(`lorem has to produce at least one word/sentence`);
        }
        const wordArbitrary = loremWord();
        if (mode === 'sentences') {
            const sentence =
                (0, array_1$b.array)(wordArbitrary, {minLength: 1, size: 'small'})
                    .map(WordsToLorem_1.wordsToSentenceMapper,
                         (0, WordsToLorem_1.wordsToSentenceUnmapperFor)(wordArbitrary));
            return (0, array_1$b.array)(sentence, {minLength: 1, maxLength: maxCount, size})
                .map(WordsToLorem_1.sentencesToParagraphMapper,
                     WordsToLorem_1.sentencesToParagraphUnmapper);
        } else {
            return (0, array_1$b.array)(wordArbitrary, {minLength: 1, maxLength: maxCount, size})
                .map(WordsToLorem_1.wordsToJoinedStringMapper,
                     (0, WordsToLorem_1.wordsToJoinedStringUnmapperFor)(wordArbitrary));
        }
    }
    lorem$1.lorem = lorem;

    var memo$1 = {};

    Object.defineProperty(memo$1, "__esModule", {value: true});
    memo$1.memo = void 0;
    let contextRemainingDepth = 10;
    function memo(builder) {
        const previous = {};
        return ((maxDepth) => {
            const n = maxDepth !== undefined ? maxDepth : contextRemainingDepth;
            if (!Object.prototype.hasOwnProperty.call(previous, n)) {
                const prev = contextRemainingDepth;
                contextRemainingDepth = n - 1;
                previous[n] = builder(n);
                contextRemainingDepth = prev;
            }
            return previous[n];
        });
    }
    memo$1.memo = memo;

    var mixedCase$1 = {};

    var MixedCaseArbitrary$1 = {};

    var ToggleFlags = {};

    Object.defineProperty(ToggleFlags, "__esModule", {value: true});
    ToggleFlags.applyFlagsOnChars = ToggleFlags.computeFlagsFromChars =
        ToggleFlags.computeTogglePositions = ToggleFlags.computeNextFlags =
            ToggleFlags.countToggledBits = void 0;
    function countToggledBits(n) {
        let count = 0;
        while (n > BigInt(0)) {
            if (n & BigInt(1))
                ++count;
            n >>= BigInt(1);
        }
        return count;
    }
    ToggleFlags.countToggledBits = countToggledBits;
    function computeNextFlags(flags, nextSize) {
        const allowedMask = (BigInt(1) << BigInt(nextSize)) - BigInt(1);
        const preservedFlags = flags & allowedMask;
        let numMissingFlags = countToggledBits(flags - preservedFlags);
        let nFlags = preservedFlags;
        for (let mask = BigInt(1); mask <= allowedMask && numMissingFlags !== 0;
             mask <<= BigInt(1)) {
            if (!(nFlags & mask)) {
                nFlags |= mask;
                --numMissingFlags;
            }
        }
        return nFlags;
    }
    ToggleFlags.computeNextFlags = computeNextFlags;
    function computeTogglePositions(chars, toggleCase) {
        const positions = [];
        for (let idx = chars.length - 1; idx !== -1; --idx) {
            if (toggleCase(chars[idx]) !== chars[idx])
                positions.push(idx);
        }
        return positions;
    }
    ToggleFlags.computeTogglePositions = computeTogglePositions;
    function computeFlagsFromChars(untoggledChars, toggledChars, togglePositions) {
        let flags = BigInt(0);
        for (let idx = 0, mask = BigInt(1); idx !== togglePositions.length;
             ++idx, mask <<= BigInt(1)) {
            if (untoggledChars[togglePositions[idx]] !== toggledChars[togglePositions[idx]]) {
                flags |= mask;
            }
        }
        return flags;
    }
    ToggleFlags.computeFlagsFromChars = computeFlagsFromChars;
    function applyFlagsOnChars(chars, flags, togglePositions, toggleCase) {
        for (let idx = 0, mask = BigInt(1); idx !== togglePositions.length;
             ++idx, mask <<= BigInt(1)) {
            if (flags & mask)
                chars[togglePositions[idx]] = toggleCase(chars[togglePositions[idx]]);
        }
    }
    ToggleFlags.applyFlagsOnChars = applyFlagsOnChars;

    Object.defineProperty(MixedCaseArbitrary$1, "__esModule", {value: true});
    MixedCaseArbitrary$1.MixedCaseArbitrary = void 0;
    const bigUintN_1 = bigUintN$1;
    const Arbitrary_1$5 = Arbitrary$1;
    const Value_1$5 = Value$1;
    const LazyIterableIterator_1$2 = LazyIterableIterator$1;
    const ToggleFlags_1 = ToggleFlags;
    class MixedCaseArbitrary extends Arbitrary_1$5.Arbitrary {
        constructor(stringArb, toggleCase, untoggleAll) {
            super();
            this.stringArb = stringArb;
            this.toggleCase = toggleCase;
            this.untoggleAll = untoggleAll;
        }
        buildContextFor(rawStringValue, flagsValue) {
            return {
                rawString: rawStringValue.value,
                rawStringContext: rawStringValue.context,
                flags: flagsValue.value,
                flagsContext: flagsValue.context,
            };
        }
        generate(mrng, biasFactor) {
            const rawStringValue = this.stringArb.generate(mrng, biasFactor);
            const chars = [...rawStringValue.value];
            const togglePositions =
                (0, ToggleFlags_1.computeTogglePositions)(chars, this.toggleCase);
            const flagsArb = (0, bigUintN_1.bigUintN)(togglePositions.length);
            const flagsValue = flagsArb.generate(mrng, undefined);
            (0, ToggleFlags_1.applyFlagsOnChars)(
                chars, flagsValue.value, togglePositions, this.toggleCase);
            return new Value_1$5.Value(chars.join(''),
                                       this.buildContextFor(rawStringValue, flagsValue));
        }
        canShrinkWithoutContext(value) {
            if (typeof value !== 'string') {
                return false;
            }
            return this.untoggleAll !== undefined
                ? this.stringArb.canShrinkWithoutContext(this.untoggleAll(value))
                : this.stringArb.canShrinkWithoutContext(value);
        }
        shrink(value, context) {
            let contextSafe;
            if (context !== undefined) {
                contextSafe = context;
            } else {
                if (this.untoggleAll !== undefined) {
                    const untoggledValue = this.untoggleAll(value);
                    const valueChars = [...value];
                    const untoggledValueChars = [...untoggledValue];
                    const togglePositions = (0, ToggleFlags_1.computeTogglePositions)(
                        untoggledValueChars, this.toggleCase);
                    contextSafe = {
                        rawString: untoggledValue,
                        rawStringContext: undefined,
                        flags: (0, ToggleFlags_1.computeFlagsFromChars)(
                            untoggledValueChars, valueChars, togglePositions),
                        flagsContext: undefined,
                    };
                } else {
                    contextSafe = {
                        rawString: value,
                        rawStringContext: undefined,
                        flags: BigInt(0),
                        flagsContext: undefined,
                    };
                }
            }
            const rawString = contextSafe.rawString;
            const flags = contextSafe.flags;
            return this.stringArb.shrink(rawString, contextSafe.rawStringContext)
                .map((nRawStringValue) => {
                    const nChars = [...nRawStringValue.value];
                    const nTogglePositions =
                        (0, ToggleFlags_1.computeTogglePositions)(nChars, this.toggleCase);
                    const nFlags =
                        (0, ToggleFlags_1.computeNextFlags)(flags, nTogglePositions.length);
                    (0, ToggleFlags_1.applyFlagsOnChars)(
                        nChars, nFlags, nTogglePositions, this.toggleCase);
                    return new Value_1$5.Value(
                        nChars.join(''),
                        this.buildContextFor(nRawStringValue,
                                             new Value_1$5.Value(nFlags, undefined)));
                })
                .join((0, LazyIterableIterator_1$2.makeLazy)(() => {
                    const chars = [...rawString];
                    const togglePositions =
                        (0, ToggleFlags_1.computeTogglePositions)(chars, this.toggleCase);
                    return (0, bigUintN_1.bigUintN)(togglePositions.length)
                        .shrink(flags, contextSafe.flagsContext)
                        .map((nFlagsValue) => {
                            const nChars = chars.slice();
                            (0, ToggleFlags_1.applyFlagsOnChars)(
                                nChars, nFlagsValue.value, togglePositions, this.toggleCase);
                            return new Value_1$5.Value(
                                nChars.join(''),
                                this.buildContextFor(
                                    new Value_1$5.Value(rawString, contextSafe.rawStringContext),
                                    nFlagsValue));
                        });
                }));
        }
    }
    MixedCaseArbitrary$1.MixedCaseArbitrary = MixedCaseArbitrary;

    Object.defineProperty(mixedCase$1, "__esModule", {value: true});
    mixedCase$1.mixedCase = void 0;
    const MixedCaseArbitrary_1 = MixedCaseArbitrary$1;
    function defaultToggleCase(rawChar) {
        const upper = rawChar.toUpperCase();
        if (upper !== rawChar)
            return upper;
        return rawChar.toLowerCase();
    }
    function mixedCase(stringArb, constraints) {
        if (typeof BigInt === 'undefined') {
            throw new Error(`mixedCase requires BigInt support`);
        }
        const toggleCase = (constraints && constraints.toggleCase) || defaultToggleCase;
        const untoggleAll = constraints && constraints.untoggleAll;
        return new MixedCaseArbitrary_1.MixedCaseArbitrary(stringArb, toggleCase, untoggleAll);
    }
    mixedCase$1.mixedCase = mixedCase;

    var object$1 = {};

    var AnyArbitraryBuilder = {};

    var float32Array$1 = {};

    Object.defineProperty(float32Array$1, "__esModule", {value: true});
    float32Array$1.float32Array = void 0;
    const float_1 = float$1;
    const array_1$a = array$1;
    function toTypedMapper$1(data) {
        return Float32Array.from(data);
    }
    function fromTypedUnmapper$1(value) {
        if (!(value instanceof Float32Array))
            throw new Error('Unexpected type');
        return [...value];
    }
    function float32Array(constraints = {}) {
        return (0, array_1$a.array)((0, float_1.float)(constraints), constraints)
            .map(toTypedMapper$1, fromTypedUnmapper$1);
    }
    float32Array$1.float32Array = float32Array;

    var float64Array$1 = {};

    Object.defineProperty(float64Array$1, "__esModule", {value: true});
    float64Array$1.float64Array = void 0;
    const double_1$2 = double$1;
    const array_1$9 = array$1;
    function toTypedMapper(data) {
        return Float64Array.from(data);
    }
    function fromTypedUnmapper(value) {
        if (!(value instanceof Float64Array))
            throw new Error('Unexpected type');
        return [...value];
    }
    function float64Array(constraints = {}) {
        return (0, array_1$9.array)((0, double_1$2.double)(constraints), constraints)
            .map(toTypedMapper, fromTypedUnmapper);
    }
    float64Array$1.float64Array = float64Array;

    var int16Array$1 = {};

    var TypedIntArrayArbitraryBuilder = {};

    var __rest = (commonjsGlobal && commonjsGlobal.__rest) || function(s, e) {
        var t = {};
        for (var p in s)
            if (Object.prototype.hasOwnProperty.call(s, p) && e.indexOf(p) < 0)
                t[p] = s[p];
        if (s != null && typeof Object.getOwnPropertySymbols === "function")
            for (var i = 0, p = Object.getOwnPropertySymbols(s); i < p.length; i++) {
                if (e.indexOf(p[i]) < 0 && Object.prototype.propertyIsEnumerable.call(s, p[i]))
                    t[p[i]] = s[p[i]];
            }
        return t;
    };
    Object.defineProperty(TypedIntArrayArbitraryBuilder, "__esModule", {value: true});
    TypedIntArrayArbitraryBuilder.typedIntArrayArbitraryArbitraryBuilder = void 0;
    const array_1$8 = array$1;
    function typedIntArrayArbitraryArbitraryBuilder(
        constraints, defaultMin, defaultMax, TypedArrayClass, arbitraryBuilder) {
        const generatorName = TypedArrayClass.name;
        const {min = defaultMin, max = defaultMax} = constraints,
                                       arrayConstraints = __rest(constraints, ["min", "max"]);
        if (min > max) {
            throw new Error(
                `Invalid range passed to ${generatorName}: min must be lower than or equal to max`);
        }
        if (min < defaultMin) {
            throw new Error(`Invalid min value passed to ${
                generatorName}: min must be greater than or equal to ${defaultMin}`);
        }
        if (max > defaultMax) {
            throw new Error(`Invalid max value passed to ${
                generatorName}: max must be lower than or equal to ${defaultMax}`);
        }
        return (0, array_1$8.array)(arbitraryBuilder({min, max}), arrayConstraints)
            .map((data) => TypedArrayClass.from(data), (value) => {
                if (!(value instanceof TypedArrayClass))
                    throw new Error('Invalid type');
                return [...value];
            });
    }
    TypedIntArrayArbitraryBuilder.typedIntArrayArbitraryArbitraryBuilder =
        typedIntArrayArbitraryArbitraryBuilder;

    Object.defineProperty(int16Array$1, "__esModule", {value: true});
    int16Array$1.int16Array = void 0;
    const integer_1$8 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$8 = TypedIntArrayArbitraryBuilder;
    function int16Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$8.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, -32768, 32767, Int16Array, integer_1$8.integer);
    }
    int16Array$1.int16Array = int16Array;

    var int32Array$1 = {};

    Object.defineProperty(int32Array$1, "__esModule", {value: true});
    int32Array$1.int32Array = void 0;
    const integer_1$7 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$7 = TypedIntArrayArbitraryBuilder;
    function int32Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$7.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, -0x80000000, 0x7fffffff, Int32Array, integer_1$7.integer);
    }
    int32Array$1.int32Array = int32Array;

    var int8Array$1 = {};

    Object.defineProperty(int8Array$1, "__esModule", {value: true});
    int8Array$1.int8Array = void 0;
    const integer_1$6 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$6 = TypedIntArrayArbitraryBuilder;
    function int8Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$6.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, -128, 127, Int8Array, integer_1$6.integer);
    }
    int8Array$1.int8Array = int8Array;

    var uint16Array$1 = {};

    Object.defineProperty(uint16Array$1, "__esModule", {value: true});
    uint16Array$1.uint16Array = void 0;
    const integer_1$5 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$5 = TypedIntArrayArbitraryBuilder;
    function uint16Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$5.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, 0, 65535, Uint16Array, integer_1$5.integer);
    }
    uint16Array$1.uint16Array = uint16Array;

    var uint32Array$1 = {};

    Object.defineProperty(uint32Array$1, "__esModule", {value: true});
    uint32Array$1.uint32Array = void 0;
    const integer_1$4 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$4 = TypedIntArrayArbitraryBuilder;
    function uint32Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$4.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, 0, 0xffffffff, Uint32Array, integer_1$4.integer);
    }
    uint32Array$1.uint32Array = uint32Array;

    var uint8Array$1 = {};

    Object.defineProperty(uint8Array$1, "__esModule", {value: true});
    uint8Array$1.uint8Array = void 0;
    const integer_1$3 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$3 = TypedIntArrayArbitraryBuilder;
    function uint8Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$3.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, 0, 255, Uint8Array, integer_1$3.integer);
    }
    uint8Array$1.uint8Array = uint8Array;

    var uint8ClampedArray$1 = {};

    Object.defineProperty(uint8ClampedArray$1, "__esModule", {value: true});
    uint8ClampedArray$1.uint8ClampedArray = void 0;
    const integer_1$2 = integer$1;
    const TypedIntArrayArbitraryBuilder_1$2 = TypedIntArrayArbitraryBuilder;
    function uint8ClampedArray(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$2.typedIntArrayArbitraryArbitraryBuilder)(
            constraints, 0, 255, Uint8ClampedArray, integer_1$2.integer);
    }
    uint8ClampedArray$1.uint8ClampedArray = uint8ClampedArray;

    var sparseArray$1 = {};

    var RestrictedIntegerArbitraryBuilder = {};

    var WithShrinkFromOtherArbitrary$1 = {};

    Object.defineProperty(WithShrinkFromOtherArbitrary$1, "__esModule", {value: true});
    WithShrinkFromOtherArbitrary$1.WithShrinkFromOtherArbitrary = void 0;
    const Arbitrary_1$4 = Arbitrary$1;
    const Value_1$4 = Value$1;
    function isSafeContext(context) {
        return context !== undefined;
    }
    function toGeneratorValue(value) {
        if (value.hasToBeCloned) {
            return new Value_1$4.Value(
                value.value_, {generatorContext: value.context}, () => value.value);
        }
        return new Value_1$4.Value(value.value_, {generatorContext: value.context});
    }
    function toShrinkerValue(value) {
        if (value.hasToBeCloned) {
            return new Value_1$4.Value(
                value.value_, {shrinkerContext: value.context}, () => value.value);
        }
        return new Value_1$4.Value(value.value_, {shrinkerContext: value.context});
    }
    class WithShrinkFromOtherArbitrary extends Arbitrary_1$4.Arbitrary {
        constructor(generatorArbitrary, shrinkerArbitrary) {
            super();
            this.generatorArbitrary = generatorArbitrary;
            this.shrinkerArbitrary = shrinkerArbitrary;
        }
        generate(mrng, biasFactor) {
            return toGeneratorValue(this.generatorArbitrary.generate(mrng, biasFactor));
        }
        canShrinkWithoutContext(value) {
            return this.shrinkerArbitrary.canShrinkWithoutContext(value);
        }
        shrink(value, context) {
            if (!isSafeContext(context)) {
                return this.shrinkerArbitrary.shrink(value, undefined).map(toShrinkerValue);
            }
            if ('generatorContext' in context) {
                return this.generatorArbitrary.shrink(value, context.generatorContext)
                    .map(toGeneratorValue);
            }
            return this.shrinkerArbitrary.shrink(value, context.shrinkerContext)
                .map(toShrinkerValue);
        }
    }
    WithShrinkFromOtherArbitrary$1.WithShrinkFromOtherArbitrary = WithShrinkFromOtherArbitrary;

    Object.defineProperty(RestrictedIntegerArbitraryBuilder, "__esModule", {value: true});
    RestrictedIntegerArbitraryBuilder.restrictedIntegerArbitraryBuilder = void 0;
    const integer_1$1 = integer$1;
    const WithShrinkFromOtherArbitrary_1 = WithShrinkFromOtherArbitrary$1;
    function restrictedIntegerArbitraryBuilder(min, maxGenerated, max) {
        const generatorArbitrary = (0, integer_1$1.integer)({min, max: maxGenerated});
        if (maxGenerated === max) {
            return generatorArbitrary;
        }
        const shrinkerArbitrary = (0, integer_1$1.integer)({min, max});
        return new WithShrinkFromOtherArbitrary_1.WithShrinkFromOtherArbitrary(generatorArbitrary,
                                                                               shrinkerArbitrary);
    }
    RestrictedIntegerArbitraryBuilder.restrictedIntegerArbitraryBuilder =
        restrictedIntegerArbitraryBuilder;

    Object.defineProperty(sparseArray$1, "__esModule", {value: true});
    sparseArray$1.sparseArray = void 0;
    const tuple_1$6 = tuple$1;
    const uniqueArray_1$1 = uniqueArray$1;
    const RestrictedIntegerArbitraryBuilder_1$1 = RestrictedIntegerArbitraryBuilder;
    const MaxLengthFromMinLength_1$3 = MaxLengthFromMinLength;
    function extractMaxIndex(indexesAndValues) {
        let maxIndex = -1;
        for (let index = 0; index !== indexesAndValues.length; ++index) {
            maxIndex = Math.max(maxIndex, indexesAndValues[index][0]);
        }
        return maxIndex;
    }
    function arrayFromItems(length, indexesAndValues) {
        const array = Array(length);
        for (let index = 0; index !== indexesAndValues.length; ++index) {
            const it = indexesAndValues[index];
            if (it[0] < length)
                array[it[0]] = it[1];
        }
        return array;
    }
    function sparseArray(arb, constraints = {}) {
        const {
            size,
            minNumElements = 0,
            maxLength = MaxLengthFromMinLength_1$3.MaxLengthUpperBound,
            maxNumElements = maxLength,
            noTrailingHole,
            depthIdentifier,
        } = constraints;
        const maxGeneratedNumElements =
            (0, MaxLengthFromMinLength_1$3.maxGeneratedLengthFromSizeForArbitrary)(
                size, minNumElements, maxNumElements, constraints.maxNumElements !== undefined);
        const maxGeneratedLength =
            (0, MaxLengthFromMinLength_1$3.maxGeneratedLengthFromSizeForArbitrary)(
                size, maxGeneratedNumElements, maxLength, constraints.maxLength !== undefined);
        if (minNumElements > maxLength) {
            throw new Error(
                `The minimal number of non-hole elements cannot be higher than the maximal length of the array`);
        }
        if (minNumElements > maxNumElements) {
            throw new Error(
                `The minimal number of non-hole elements cannot be higher than the maximal number of non-holes`);
        }
        const resultedMaxNumElements = Math.min(maxNumElements, maxLength);
        const resultedSizeMaxNumElements =
            constraints.maxNumElements !== undefined || size !== undefined ? size : '=';
        const maxGeneratedIndexAuthorized = Math.max(maxGeneratedLength - 1, 0);
        const maxIndexAuthorized = Math.max(maxLength - 1, 0);
        const sparseArrayNoTrailingHole =
            (0, uniqueArray_1$1.uniqueArray)(
                (0, tuple_1$6.tuple)(
                    (0, RestrictedIntegerArbitraryBuilder_1$1.restrictedIntegerArbitraryBuilder)(
                        0, maxGeneratedIndexAuthorized, maxIndexAuthorized),
                    arb),
                {
                    size: resultedSizeMaxNumElements,
                    minLength: minNumElements,
                    maxLength: resultedMaxNumElements,
                    selector: (item) => item[0],
                    depthIdentifier,
                })
                .map(
                    (items) => {
                        const lastIndex = extractMaxIndex(items);
                        return arrayFromItems(lastIndex + 1, items);
                    },
                    (value) => {
                        if (!Array.isArray(value)) {
                            throw new Error('Not supported entry type');
                        }
                        if (noTrailingHole && value.length !== 0 && !(value.length - 1 in value)) {
                            throw new Error('No trailing hole');
                        }
                        return Object.entries(value).map((entry) => [Number(entry[0]), entry[1]]);
                    });
        if (noTrailingHole || maxLength === minNumElements) {
            return sparseArrayNoTrailingHole;
        }
        return (0, tuple_1$6.tuple)(
                   sparseArrayNoTrailingHole,
                   (0, RestrictedIntegerArbitraryBuilder_1$1.restrictedIntegerArbitraryBuilder)(
                       minNumElements, maxGeneratedLength, maxLength))
            .map(
                (data) => {
                    const sparse = data[0];
                    const targetLength = data[1];
                    if (sparse.length >= targetLength) {
                        return sparse;
                    }
                    const longerSparse = sparse.slice();
                    longerSparse.length = targetLength;
                    return longerSparse;
                },
                (value) => {
                    if (!Array.isArray(value)) {
                        throw new Error('Not supported entry type');
                    }
                    return [value, value.length];
                });
    }
    sparseArray$1.sparseArray = sparseArray;

    var ArrayToMap = {};

    Object.defineProperty(ArrayToMap, "__esModule", {value: true});
    ArrayToMap.arrayToMapUnmapper = ArrayToMap.arrayToMapMapper = void 0;
    function arrayToMapMapper(data) {
        return new Map(data);
    }
    ArrayToMap.arrayToMapMapper = arrayToMapMapper;
    function arrayToMapUnmapper(value) {
        if (typeof value !== 'object' || value === null) {
            throw new Error('Incompatible instance received: should be a non-null object');
        }
        if (!('constructor' in value) || value.constructor !== Map) {
            throw new Error('Incompatible instance received: should be of exact type Map');
        }
        return Array.from(value);
    }
    ArrayToMap.arrayToMapUnmapper = arrayToMapUnmapper;

    var ArrayToSet = {};

    Object.defineProperty(ArrayToSet, "__esModule", {value: true});
    ArrayToSet.arrayToSetUnmapper = ArrayToSet.arrayToSetMapper = void 0;
    function arrayToSetMapper(data) {
        return new Set(data);
    }
    ArrayToSet.arrayToSetMapper = arrayToSetMapper;
    function arrayToSetUnmapper(value) {
        if (typeof value !== 'object' || value === null) {
            throw new Error('Incompatible instance received: should be a non-null object');
        }
        if (!('constructor' in value) || value.constructor !== Set) {
            throw new Error('Incompatible instance received: should be of exact type Set');
        }
        return Array.from(value);
    }
    ArrayToSet.arrayToSetUnmapper = arrayToSetUnmapper;

    var ObjectToPrototypeLess = {};

    Object.defineProperty(ObjectToPrototypeLess, "__esModule", {value: true});
    ObjectToPrototypeLess.objectToPrototypeLessUnmapper =
        ObjectToPrototypeLess.objectToPrototypeLessMapper = void 0;
    function objectToPrototypeLessMapper(o) {
        return Object.assign(Object.create(null), o);
    }
    ObjectToPrototypeLess.objectToPrototypeLessMapper = objectToPrototypeLessMapper;
    function objectToPrototypeLessUnmapper(value) {
        if (typeof value !== 'object' || value === null) {
            throw new Error('Incompatible instance received: should be a non-null object');
        }
        if ('__proto__' in value) {
            throw new Error('Incompatible instance received: should not have any __proto__');
        }
        return Object.assign({}, value);
    }
    ObjectToPrototypeLess.objectToPrototypeLessUnmapper = objectToPrototypeLessUnmapper;

    Object.defineProperty(AnyArbitraryBuilder, "__esModule", {value: true});
    AnyArbitraryBuilder.anyArbitraryBuilder = void 0;
    const stringify_1$3 = stringify;
    const array_1$7 = array$1;
    const oneof_1$3 = oneof$1;
    const tuple_1$5 = tuple$1;
    const bigInt_1$2 = bigInt$1;
    const date_1 = date$1;
    const float32Array_1 = float32Array$1;
    const float64Array_1 = float64Array$1;
    const int16Array_1 = int16Array$1;
    const int32Array_1 = int32Array$1;
    const int8Array_1 = int8Array$1;
    const uint16Array_1 = uint16Array$1;
    const uint32Array_1 = uint32Array$1;
    const uint8Array_1 = uint8Array$1;
    const uint8ClampedArray_1 = uint8ClampedArray$1;
    const sparseArray_1 = sparseArray$1;
    const KeyValuePairsToObject_1 = KeyValuePairsToObject;
    const ArrayToMap_1 = ArrayToMap;
    const ArrayToSet_1 = ArrayToSet;
    const ObjectToPrototypeLess_1 = ObjectToPrototypeLess;
    const letrec_1 = letrec$1;
    const uniqueArray_1 = uniqueArray$1;
    const DepthContext_1 = DepthContext;
    function mapOf(ka, va, maxKeys, size, depthIdentifier) {
        return (0, uniqueArray_1.uniqueArray)((0, tuple_1$5.tuple)(ka, va), {
                   maxLength: maxKeys,
                   size,
                   comparator: 'SameValueZero',
                   selector: (t) => t[0],
                   depthIdentifier,
               })
            .map(ArrayToMap_1.arrayToMapMapper, ArrayToMap_1.arrayToMapUnmapper);
    }
    function dictOf(ka, va, maxKeys, size, depthIdentifier) {
        return (0, uniqueArray_1.uniqueArray)((0, tuple_1$5.tuple)(ka, va), {
                   maxLength: maxKeys,
                   size,
                   selector: (t) => t[0],
                   depthIdentifier,
               })
            .map(KeyValuePairsToObject_1.keyValuePairsToObjectMapper,
                 KeyValuePairsToObject_1.keyValuePairsToObjectUnmapper);
    }
    function setOf(va, maxKeys, size, depthIdentifier) {
        return (0, uniqueArray_1.uniqueArray)(
                   va, {maxLength: maxKeys, size, comparator: 'SameValueZero', depthIdentifier})
            .map(ArrayToSet_1.arrayToSetMapper, ArrayToSet_1.arrayToSetUnmapper);
    }
    function prototypeLessOf(objectArb) {
        return objectArb.map(ObjectToPrototypeLess_1.objectToPrototypeLessMapper,
                             ObjectToPrototypeLess_1.objectToPrototypeLessUnmapper);
    }
    function typedArray(constraints) {
        return (0, oneof_1$3.oneof)((0, int8Array_1.int8Array)(constraints),
                                    (0, uint8Array_1.uint8Array)(constraints),
                                    (0, uint8ClampedArray_1.uint8ClampedArray)(constraints),
                                    (0, int16Array_1.int16Array)(constraints),
                                    (0, uint16Array_1.uint16Array)(constraints),
                                    (0, int32Array_1.int32Array)(constraints),
                                    (0, uint32Array_1.uint32Array)(constraints),
                                    (0, float32Array_1.float32Array)(constraints),
                                    (0, float64Array_1.float64Array)(constraints));
    }
    function anyArbitraryBuilder(constraints) {
        const arbitrariesForBase = constraints.values;
        const depthSize = constraints.depthSize;
        const depthIdentifier = (0, DepthContext_1.createDepthIdentifier)();
        const maxDepth = constraints.maxDepth;
        const maxKeys = constraints.maxKeys;
        const size = constraints.size;
        const baseArb =
            (0, oneof_1$3.oneof)(...arbitrariesForBase,
                                 ...(constraints.withBigInt ? [(0, bigInt_1$2.bigInt)()] : []),
                                 ...(constraints.withDate ? [(0, date_1.date)()] : []));
        return (0, letrec_1.letrec)(
                   (tie) => ({
                       anything: (0, oneof_1$3.oneof)(
                           {maxDepth, depthSize, depthIdentifier},
                           baseArb,
                           tie('array'),
                           tie('object'),
                           ...(constraints.withMap ? [tie('map')] : []),
                           ...(constraints.withSet ? [tie('set')] : []),
                           ...(constraints.withObjectString
                                   ? [tie('anything').map((o) => (0, stringify_1$3.stringify)(o))]
                                   : []),
                           ...(constraints.withNullPrototype ? [prototypeLessOf(tie('object'))]
                                                             : []),
                           ...(constraints.withTypedArray ? [typedArray({maxLength: maxKeys, size})]
                                                          : []),
                           ...(constraints.withSparseArray
                                   ? [(0, sparseArray_1.sparseArray)(
                                         tie('anything'),
                                         {maxNumElements: maxKeys, size, depthIdentifier})]
                                   : [])),
                       keys: constraints.withObjectString
                           ? (0, oneof_1$3.oneof)({arbitrary: constraints.key, weight: 10}, {
                                 arbitrary:
                                     tie('anything').map((o) => (0, stringify_1$3.stringify)(o)),
                                 weight: 1
                             })
                           : constraints.key,
                       array: (0, array_1$7.array)(tie('anything'),
                                                   {maxLength: maxKeys, size, depthIdentifier}),
                       set: setOf(tie('anything'), maxKeys, size, depthIdentifier),
                       map: (0, oneof_1$3.oneof)(
                           mapOf(tie('keys'), tie('anything'), maxKeys, size, depthIdentifier),
                           mapOf(tie('anything'), tie('anything'), maxKeys, size, depthIdentifier)),
                       object: dictOf(tie('keys'), tie('anything'), maxKeys, size, depthIdentifier),
                   }))
            .anything;
    }
    AnyArbitraryBuilder.anyArbitraryBuilder = anyArbitraryBuilder;

    var QualifiedObjectConstraints = {};

    var string$1 = {};

    Object.defineProperty(string$1, "__esModule", {value: true});
    string$1.string = void 0;
    const array_1$6 = array$1;
    const char_1 = char$1;
    const CodePointsToString_1$4 = CodePointsToString;
    const SlicesForStringBuilder_1$5 = SlicesForStringBuilder;
    function string(constraints = {}) {
        const charArbitrary = (0, char_1.char)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$5.createSlicesForString)(
            charArbitrary, CodePointsToString_1$4.codePointsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$6.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1$4.codePointsToStringMapper,
                 CodePointsToString_1$4.codePointsToStringUnmapper);
    }
    string$1.string = string;

    var BoxedArbitraryBuilder = {};

    var UnboxedToBoxed = {};

    Object.defineProperty(UnboxedToBoxed, "__esModule", {value: true});
    UnboxedToBoxed.unboxedToBoxedUnmapper = UnboxedToBoxed.unboxedToBoxedMapper = void 0;
    function unboxedToBoxedMapper(value) {
        switch (typeof value) {
            case 'boolean':
                return new Boolean(value);
            case 'number':
                return new Number(value);
            case 'string':
                return new String(value);
            default:
                return value;
        }
    }
    UnboxedToBoxed.unboxedToBoxedMapper = unboxedToBoxedMapper;
    function unboxedToBoxedUnmapper(value) {
        if (typeof value !== 'object' || value === null || !('constructor' in value)) {
            return value;
        }
        return value.constructor === Boolean || value.constructor === Number ||
                value.constructor === String
            ? value.valueOf()
            : value;
    }
    UnboxedToBoxed.unboxedToBoxedUnmapper = unboxedToBoxedUnmapper;

    Object.defineProperty(BoxedArbitraryBuilder, "__esModule", {value: true});
    BoxedArbitraryBuilder.boxedArbitraryBuilder = void 0;
    const UnboxedToBoxed_1 = UnboxedToBoxed;
    function boxedArbitraryBuilder(arb) {
        return arb.map(UnboxedToBoxed_1.unboxedToBoxedMapper,
                       UnboxedToBoxed_1.unboxedToBoxedUnmapper);
    }
    BoxedArbitraryBuilder.boxedArbitraryBuilder = boxedArbitraryBuilder;

    Object.defineProperty(QualifiedObjectConstraints, "__esModule", {value: true});
    QualifiedObjectConstraints.toQualifiedObjectConstraints = void 0;
    const boolean_1$1 = boolean$1;
    const constant_1$3 = constant$1;
    const double_1$1 = double$1;
    const maxSafeInteger_1 = maxSafeInteger$1;
    const oneof_1$2 = oneof$1;
    const string_1$1 = string$1;
    const BoxedArbitraryBuilder_1 = BoxedArbitraryBuilder;
    function defaultValues(constraints) {
        return [
            (0, boolean_1$1.boolean)(),
            (0, maxSafeInteger_1.maxSafeInteger)(),
            (0, double_1$1.double)(),
            (0, string_1$1.string)(constraints),
            (0, oneof_1$2.oneof)((0, string_1$1.string)(constraints),
                                 (0, constant_1$3.constant)(null),
                                 (0, constant_1$3.constant)(undefined)),
        ];
    }
    function boxArbitraries(arbs) {
        return arbs.map((arb) => (0, BoxedArbitraryBuilder_1.boxedArbitraryBuilder)(arb));
    }
    function boxArbitrariesIfNeeded(arbs, boxEnabled) {
        return boxEnabled ? boxArbitraries(arbs).concat(arbs) : arbs;
    }
    function toQualifiedObjectConstraints(settings = {}) {
        function orDefault(optionalValue, defaultValue) {
            return optionalValue !== undefined ? optionalValue : defaultValue;
        }
        const valueConstraints = {size: settings.size};
        return {
            key: orDefault(settings.key, (0, string_1$1.string)(valueConstraints)),
            values:
                boxArbitrariesIfNeeded(orDefault(settings.values, defaultValues(valueConstraints)),
                                       orDefault(settings.withBoxedValues, false)),
            depthSize: settings.depthSize,
            maxDepth: settings.maxDepth,
            maxKeys: settings.maxKeys,
            size: settings.size,
            withSet: orDefault(settings.withSet, false),
            withMap: orDefault(settings.withMap, false),
            withObjectString: orDefault(settings.withObjectString, false),
            withNullPrototype: orDefault(settings.withNullPrototype, false),
            withBigInt: orDefault(settings.withBigInt, false),
            withDate: orDefault(settings.withDate, false),
            withTypedArray: orDefault(settings.withTypedArray, false),
            withSparseArray: orDefault(settings.withSparseArray, false),
        };
    }
    QualifiedObjectConstraints.toQualifiedObjectConstraints = toQualifiedObjectConstraints;

    Object.defineProperty(object$1, "__esModule", {value: true});
    object$1.object = void 0;
    const dictionary_1 = dictionary$1;
    const AnyArbitraryBuilder_1$1 = AnyArbitraryBuilder;
    const QualifiedObjectConstraints_1$1 = QualifiedObjectConstraints;
    function objectInternal(constraints) {
        return (0, dictionary_1.dictionary)(
            constraints.key, (0, AnyArbitraryBuilder_1$1.anyArbitraryBuilder)(constraints), {
                maxKeys: constraints.maxKeys,
                size: constraints.size,
            });
    }
    function object(constraints) {
        return objectInternal(
            (0, QualifiedObjectConstraints_1$1.toQualifiedObjectConstraints)(constraints));
    }
    object$1.object = object;

    var json$1 = {};

    var jsonValue$1 = {};

    var JsonConstraintsBuilder = {};

    Object.defineProperty(JsonConstraintsBuilder, "__esModule", {value: true});
    JsonConstraintsBuilder.jsonConstraintsBuilder = void 0;
    const boolean_1 = boolean$1;
    const constant_1$2 = constant$1;
    const double_1 = double$1;
    function jsonConstraintsBuilder(stringArbitrary, constraints) {
        const {depthSize, maxDepth} = constraints;
        const key = stringArbitrary;
        const values = [
            (0, boolean_1.boolean)(),
            (0, double_1.double)({noDefaultInfinity: true, noNaN: true}),
            stringArbitrary,
            (0, constant_1$2.constant)(null),
        ];
        return {key, values, depthSize, maxDepth};
    }
    JsonConstraintsBuilder.jsonConstraintsBuilder = jsonConstraintsBuilder;

    var anything$1 = {};

    Object.defineProperty(anything$1, "__esModule", {value: true});
    anything$1.anything = void 0;
    const AnyArbitraryBuilder_1 = AnyArbitraryBuilder;
    const QualifiedObjectConstraints_1 = QualifiedObjectConstraints;
    function anything(constraints) {
        return (0, AnyArbitraryBuilder_1.anyArbitraryBuilder)(
            (0, QualifiedObjectConstraints_1.toQualifiedObjectConstraints)(constraints));
    }
    anything$1.anything = anything;

    Object.defineProperty(jsonValue$1, "__esModule", {value: true});
    jsonValue$1.jsonValue = void 0;
    const string_1 = string$1;
    const JsonConstraintsBuilder_1$1 = JsonConstraintsBuilder;
    const anything_1$1 = anything$1;
    function jsonValue(constraints = {}) {
        return (0, anything_1$1.anything)((0, JsonConstraintsBuilder_1$1.jsonConstraintsBuilder)(
            (0, string_1.string)(), constraints));
    }
    jsonValue$1.jsonValue = jsonValue;

    Object.defineProperty(json$1, "__esModule", {value: true});
    json$1.json = void 0;
    const jsonValue_1 = jsonValue$1;
    function json(constraints = {}) {
        const arb = (0, jsonValue_1.jsonValue)(constraints);
        return arb.map(JSON.stringify);
    }
    json$1.json = json;

    var unicodeJsonValue$1 = {};

    var unicodeString$1 = {};

    Object.defineProperty(unicodeString$1, "__esModule", {value: true});
    unicodeString$1.unicodeString = void 0;
    const array_1$5 = array$1;
    const unicode_1 = unicode$1;
    const CodePointsToString_1$3 = CodePointsToString;
    const SlicesForStringBuilder_1$4 = SlicesForStringBuilder;
    function unicodeString(constraints = {}) {
        const charArbitrary = (0, unicode_1.unicode)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$4.createSlicesForString)(
            charArbitrary, CodePointsToString_1$3.codePointsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$5.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1$3.codePointsToStringMapper,
                 CodePointsToString_1$3.codePointsToStringUnmapper);
    }
    unicodeString$1.unicodeString = unicodeString;

    Object.defineProperty(unicodeJsonValue$1, "__esModule", {value: true});
    unicodeJsonValue$1.unicodeJsonValue = void 0;
    const unicodeString_1 = unicodeString$1;
    const JsonConstraintsBuilder_1 = JsonConstraintsBuilder;
    const anything_1 = anything$1;
    function unicodeJsonValue(constraints = {}) {
        return (0, anything_1.anything)((0, JsonConstraintsBuilder_1.jsonConstraintsBuilder)(
            (0, unicodeString_1.unicodeString)(), constraints));
    }
    unicodeJsonValue$1.unicodeJsonValue = unicodeJsonValue;

    var unicodeJson$1 = {};

    Object.defineProperty(unicodeJson$1, "__esModule", {value: true});
    unicodeJson$1.unicodeJson = void 0;
    const unicodeJsonValue_1 = unicodeJsonValue$1;
    function unicodeJson(constraints = {}) {
        const arb = (0, unicodeJsonValue_1.unicodeJsonValue)(constraints);
        return arb.map(JSON.stringify);
    }
    unicodeJson$1.unicodeJson = unicodeJson;

    var record$1 = {};

    var PartialRecordArbitraryBuilder = {};

    var EnumerableKeysExtractor = {};

    Object.defineProperty(EnumerableKeysExtractor, "__esModule", {value: true});
    EnumerableKeysExtractor.extractEnumerableKeys = void 0;
    function extractEnumerableKeys(instance) {
        const keys = Object.keys(instance);
        const symbols = Object.getOwnPropertySymbols(instance);
        for (let index = 0; index !== symbols.length; ++index) {
            const symbol = symbols[index];
            const descriptor = Object.getOwnPropertyDescriptor(instance, symbol);
            if (descriptor && descriptor.enumerable) {
                keys.push(symbol);
            }
        }
        return keys;
    }
    EnumerableKeysExtractor.extractEnumerableKeys = extractEnumerableKeys;

    var ValuesAndSeparateKeysToObject = {};

    Object.defineProperty(ValuesAndSeparateKeysToObject, "__esModule", {value: true});
    ValuesAndSeparateKeysToObject.buildValuesAndSeparateKeysToObjectUnmapper =
        ValuesAndSeparateKeysToObject.buildValuesAndSeparateKeysToObjectMapper = void 0;
    function buildValuesAndSeparateKeysToObjectMapper(keys, noKeyValue) {
        return function valuesAndSeparateKeysToObjectMapper(gs) {
            const obj = {};
            for (let idx = 0; idx !== keys.length; ++idx) {
                const valueWrapper = gs[idx];
                if (valueWrapper !== noKeyValue) {
                    obj[keys[idx]] = valueWrapper;
                }
            }
            return obj;
        };
    }
    ValuesAndSeparateKeysToObject.buildValuesAndSeparateKeysToObjectMapper =
        buildValuesAndSeparateKeysToObjectMapper;
    function buildValuesAndSeparateKeysToObjectUnmapper(keys, noKeyValue) {
        return function valuesAndSeparateKeysToObjectUnmapper(value) {
            if (typeof value !== 'object' || value === null) {
                throw new Error('Incompatible instance received: should be a non-null object');
            }
            if (!('constructor' in value) || value.constructor !== Object) {
                throw new Error('Incompatible instance received: should be of exact type Object');
            }
            let extractedPropertiesCount = 0;
            const extractedValues = [];
            for (let idx = 0; idx !== keys.length; ++idx) {
                const descriptor = Object.getOwnPropertyDescriptor(value, keys[idx]);
                if (descriptor !== undefined) {
                    if (!descriptor.configurable || !descriptor.enumerable ||
                        !descriptor.writable) {
                        throw new Error(
                            'Incompatible instance received: should contain only c/e/w properties');
                    }
                    if (descriptor.get !== undefined || descriptor.set !== undefined) {
                        throw new Error(
                            'Incompatible instance received: should contain only no get/set properties');
                    }
                    ++extractedPropertiesCount;
                    extractedValues.push(descriptor.value);
                } else {
                    extractedValues.push(noKeyValue);
                }
            }
            const namePropertiesCount = Object.getOwnPropertyNames(value).length;
            const symbolPropertiesCount = Object.getOwnPropertySymbols(value).length;
            if (extractedPropertiesCount !== namePropertiesCount + symbolPropertiesCount) {
                throw new Error(
                    'Incompatible instance received: should not contain extra properties');
            }
            return extractedValues;
        };
    }
    ValuesAndSeparateKeysToObject.buildValuesAndSeparateKeysToObjectUnmapper =
        buildValuesAndSeparateKeysToObjectUnmapper;

    Object.defineProperty(PartialRecordArbitraryBuilder, "__esModule", {value: true});
    PartialRecordArbitraryBuilder.buildPartialRecordArbitrary = void 0;
    const option_1$2 = option$1;
    const tuple_1$4 = tuple$1;
    const EnumerableKeysExtractor_1 = EnumerableKeysExtractor;
    const ValuesAndSeparateKeysToObject_1 = ValuesAndSeparateKeysToObject;
    const noKeyValue = Symbol('no-key');
    function buildPartialRecordArbitrary(recordModel, requiredKeys) {
        const keys = (0, EnumerableKeysExtractor_1.extractEnumerableKeys)(recordModel);
        const arbs = [];
        for (let index = 0; index !== keys.length; ++index) {
            const k = keys[index];
            const requiredArbitrary = recordModel[k];
            if (requiredKeys === undefined || requiredKeys.indexOf(k) !== -1)
                arbs.push(requiredArbitrary);
            else
                arbs.push((0, option_1$2.option)(requiredArbitrary, {nil: noKeyValue}));
        }
        return (0, tuple_1$4.tuple)(...arbs).map(
            (0, ValuesAndSeparateKeysToObject_1.buildValuesAndSeparateKeysToObjectMapper)(
                keys, noKeyValue),
            (0, ValuesAndSeparateKeysToObject_1.buildValuesAndSeparateKeysToObjectUnmapper)(
                keys, noKeyValue));
    }
    PartialRecordArbitraryBuilder.buildPartialRecordArbitrary = buildPartialRecordArbitrary;

    Object.defineProperty(record$1, "__esModule", {value: true});
    record$1.record = void 0;
    const PartialRecordArbitraryBuilder_1 = PartialRecordArbitraryBuilder;
    function record(recordModel, constraints) {
        if (constraints == null) {
            return (0, PartialRecordArbitraryBuilder_1.buildPartialRecordArbitrary)(recordModel,
                                                                                    undefined);
        }
        if ('withDeletedKeys' in constraints && 'requiredKeys' in constraints) {
            throw new Error(
                `requiredKeys and withDeletedKeys cannot be used together in fc.record`);
        }
        const requireDeletedKeys =
            ('requiredKeys' in constraints && constraints.requiredKeys !== undefined) ||
            ('withDeletedKeys' in constraints && !!constraints.withDeletedKeys);
        if (!requireDeletedKeys) {
            return (0, PartialRecordArbitraryBuilder_1.buildPartialRecordArbitrary)(recordModel,
                                                                                    undefined);
        }
        const requiredKeys =
            ('requiredKeys' in constraints ? constraints.requiredKeys : undefined) || [];
        for (let idx = 0; idx !== requiredKeys.length; ++idx) {
            const descriptor = Object.getOwnPropertyDescriptor(recordModel, requiredKeys[idx]);
            if (descriptor === undefined) {
                throw new Error(
                    `requiredKeys cannot reference keys that have not been defined in recordModel`);
            }
            if (!descriptor.enumerable) {
                throw new Error(
                    `requiredKeys cannot reference keys that have are enumerable in recordModel`);
            }
        }
        return (0, PartialRecordArbitraryBuilder_1.buildPartialRecordArbitrary)(recordModel,
                                                                                requiredKeys);
    }
    record$1.record = record;

    var infiniteStream$1 = {};

    var StreamArbitrary$1 = {};

    Object.defineProperty(StreamArbitrary$1, "__esModule", {value: true});
    StreamArbitrary$1.StreamArbitrary = void 0;
    const Arbitrary_1$3 = Arbitrary$1;
    const Value_1$3 = Value$1;
    const symbols_1$3 = symbols;
    const Stream_1$3 = Stream$1;
    const stringify_1$2 = stringify;
    function prettyPrint(seenValuesStrings) {
        return `Stream(${seenValuesStrings.join(',')}…)`;
    }
    class StreamArbitrary extends Arbitrary_1$3.Arbitrary {
        constructor(arb) {
            super();
            this.arb = arb;
        }
        generate(mrng, biasFactor) {
            const appliedBiasFactor = biasFactor !== undefined && mrng.nextInt(1, biasFactor) === 1
                ? biasFactor
                : undefined;
            const enrichedProducer = () => {
                const seenValues = [];
                const g = function*(arb, clonedMrng) {
                    while (true) {
                        const value = arb.generate(clonedMrng, appliedBiasFactor).value;
                        seenValues.push(value);
                        yield value;
                    }
                };
                const s = new Stream_1$3.Stream(g(this.arb, mrng.clone()));
                return Object.defineProperties(s, {
                    toString: {value: () => prettyPrint(seenValues.map(stringify_1$2.stringify))},
                    [stringify_1$2.toStringMethod]:
                        {value: () => prettyPrint(seenValues.map(stringify_1$2.stringify))},
                    [stringify_1$2.asyncToStringMethod]: {
                        value: async () => prettyPrint(
                            await Promise.all(seenValues.map(stringify_1$2.asyncStringify)))
                    },
                    [symbols_1$3.cloneMethod]: {value: enrichedProducer, enumerable: true},
                });
            };
            return new Value_1$3.Value(enrichedProducer(), undefined);
        }
        canShrinkWithoutContext(value) {
            return false;
        }
        shrink(_value, _context) {
            return Stream_1$3.Stream.nil();
        }
    }
    StreamArbitrary$1.StreamArbitrary = StreamArbitrary;

    Object.defineProperty(infiniteStream$1, "__esModule", {value: true});
    infiniteStream$1.infiniteStream = void 0;
    const StreamArbitrary_1 = StreamArbitrary$1;
    function infiniteStream(arb) {
        return new StreamArbitrary_1.StreamArbitrary(arb);
    }
    infiniteStream$1.infiniteStream = infiniteStream;

    var asciiString$1 = {};

    Object.defineProperty(asciiString$1, "__esModule", {value: true});
    asciiString$1.asciiString = void 0;
    const array_1$4 = array$1;
    const ascii_1 = ascii$1;
    const CodePointsToString_1$2 = CodePointsToString;
    const SlicesForStringBuilder_1$3 = SlicesForStringBuilder;
    function asciiString(constraints = {}) {
        const charArbitrary = (0, ascii_1.ascii)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$3.createSlicesForString)(
            charArbitrary, CodePointsToString_1$2.codePointsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$4.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1$2.codePointsToStringMapper,
                 CodePointsToString_1$2.codePointsToStringUnmapper);
    }
    asciiString$1.asciiString = asciiString;

    var base64String$1 = {};

    var StringToBase64 = {};

    Object.defineProperty(StringToBase64, "__esModule", {value: true});
    StringToBase64.stringToBase64Unmapper = StringToBase64.stringToBase64Mapper = void 0;
    function stringToBase64Mapper(s) {
        switch (s.length % 4) {
            case 0:
                return s;
            case 3:
                return `${s}=`;
            case 2:
                return `${s}==`;
            default:
                return s.slice(1);
        }
    }
    StringToBase64.stringToBase64Mapper = stringToBase64Mapper;
    function stringToBase64Unmapper(value) {
        if (typeof value !== 'string' || value.length % 4 !== 0) {
            throw new Error('Invalid string received');
        }
        const lastTrailingIndex = value.indexOf('=');
        if (lastTrailingIndex === -1) {
            return value;
        }
        const numTrailings = value.length - lastTrailingIndex;
        if (numTrailings > 2) {
            throw new Error('Cannot unmap the passed value');
        }
        return value.substring(0, lastTrailingIndex);
    }
    StringToBase64.stringToBase64Unmapper = stringToBase64Unmapper;

    Object.defineProperty(base64String$1, "__esModule", {value: true});
    base64String$1.base64String = void 0;
    const array_1$3 = array$1;
    const base64_1 = base64$1;
    const MaxLengthFromMinLength_1$2 = MaxLengthFromMinLength;
    const CodePointsToString_1$1 = CodePointsToString;
    const StringToBase64_1 = StringToBase64;
    const SlicesForStringBuilder_1$2 = SlicesForStringBuilder;
    function base64String(constraints = {}) {
        const {
            minLength: unscaledMinLength = 0,
            maxLength: unscaledMaxLength = MaxLengthFromMinLength_1$2.MaxLengthUpperBound,
            size
        } = constraints;
        const minLength = unscaledMinLength + 3 - ((unscaledMinLength + 3) % 4);
        const maxLength = unscaledMaxLength - (unscaledMaxLength % 4);
        const requestedSize =
            constraints.maxLength === undefined && size === undefined ? '=' : size;
        if (minLength > maxLength)
            throw new Error('Minimal length should be inferior or equal to maximal length');
        if (minLength % 4 !== 0)
            throw new Error('Minimal length of base64 strings must be a multiple of 4');
        if (maxLength % 4 !== 0)
            throw new Error('Maximal length of base64 strings must be a multiple of 4');
        const charArbitrary = (0, base64_1.base64)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$2.createSlicesForString)(
            charArbitrary, CodePointsToString_1$1.codePointsToStringUnmapper);
        const enrichedConstraints = {
            minLength,
            maxLength,
            size: requestedSize,
            experimentalCustomSlices,
        };
        return (0, array_1$3.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1$1.codePointsToStringMapper,
                 CodePointsToString_1$1.codePointsToStringUnmapper)
            .map(StringToBase64_1.stringToBase64Mapper, StringToBase64_1.stringToBase64Unmapper);
    }
    base64String$1.base64String = base64String;

    var fullUnicodeString$1 = {};

    Object.defineProperty(fullUnicodeString$1, "__esModule", {value: true});
    fullUnicodeString$1.fullUnicodeString = void 0;
    const array_1$2 = array$1;
    const fullUnicode_1 = fullUnicode$1;
    const CodePointsToString_1 = CodePointsToString;
    const SlicesForStringBuilder_1$1 = SlicesForStringBuilder;
    function fullUnicodeString(constraints = {}) {
        const charArbitrary = (0, fullUnicode_1.fullUnicode)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1$1.createSlicesForString)(
            charArbitrary, CodePointsToString_1.codePointsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$2.array)(charArbitrary, enrichedConstraints)
            .map(CodePointsToString_1.codePointsToStringMapper,
                 CodePointsToString_1.codePointsToStringUnmapper);
    }
    fullUnicodeString$1.fullUnicodeString = fullUnicodeString;

    var string16bits$1 = {};

    var CharsToString = {};

    Object.defineProperty(CharsToString, "__esModule", {value: true});
    CharsToString.charsToStringUnmapper = CharsToString.charsToStringMapper = void 0;
    function charsToStringMapper(tab) {
        return tab.join('');
    }
    CharsToString.charsToStringMapper = charsToStringMapper;
    function charsToStringUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Cannot unmap the passed value');
        }
        return value.split('');
    }
    CharsToString.charsToStringUnmapper = charsToStringUnmapper;

    Object.defineProperty(string16bits$1, "__esModule", {value: true});
    string16bits$1.string16bits = void 0;
    const array_1$1 = array$1;
    const char16bits_1 = char16bits$1;
    const CharsToString_1 = CharsToString;
    const SlicesForStringBuilder_1 = SlicesForStringBuilder;
    function string16bits(constraints = {}) {
        const charArbitrary = (0, char16bits_1.char16bits)();
        const experimentalCustomSlices = (0, SlicesForStringBuilder_1.createSlicesForString)(
            charArbitrary, CharsToString_1.charsToStringUnmapper);
        const enrichedConstraints =
            Object.assign(Object.assign({}, constraints), {experimentalCustomSlices});
        return (0, array_1$1.array)(charArbitrary, enrichedConstraints)
            .map(CharsToString_1.charsToStringMapper, CharsToString_1.charsToStringUnmapper);
    }
    string16bits$1.string16bits = string16bits;

    var subarray$1 = {};

    var SubarrayArbitrary$1 = {};

    var IsSubarrayOf = {};

    Object.defineProperty(IsSubarrayOf, "__esModule", {value: true});
    IsSubarrayOf.isSubarrayOf = void 0;
    function isSubarrayOf(source, small) {
        const countMap = new Map();
        let countMinusZero = 0;
        for (const sourceEntry of source) {
            if (Object.is(sourceEntry, -0)) {
                ++countMinusZero;
            } else {
                const oldCount = countMap.get(sourceEntry) || 0;
                countMap.set(sourceEntry, oldCount + 1);
            }
        }
        for (let index = 0; index !== small.length; ++index) {
            if (!(index in small)) {
                return false;
            }
            const smallEntry = small[index];
            if (Object.is(smallEntry, -0)) {
                if (countMinusZero === 0)
                    return false;
                --countMinusZero;
            } else {
                const oldCount = countMap.get(smallEntry) || 0;
                if (oldCount === 0)
                    return false;
                countMap.set(smallEntry, oldCount - 1);
            }
        }
        return true;
    }
    IsSubarrayOf.isSubarrayOf = isSubarrayOf;

    Object.defineProperty(SubarrayArbitrary$1, "__esModule", {value: true});
    SubarrayArbitrary$1.SubarrayArbitrary = void 0;
    const Arbitrary_1$2 = Arbitrary$1;
    const Value_1$2 = Value$1;
    const LazyIterableIterator_1$1 = LazyIterableIterator$1;
    const Stream_1$2 = Stream$1;
    const IsSubarrayOf_1 = IsSubarrayOf;
    const IntegerArbitrary_1 = IntegerArbitrary$1;
    class SubarrayArbitrary extends Arbitrary_1$2.Arbitrary {
        constructor(originalArray, isOrdered, minLength, maxLength) {
            super();
            this.originalArray = originalArray;
            this.isOrdered = isOrdered;
            this.minLength = minLength;
            this.maxLength = maxLength;
            if (minLength < 0 || minLength > originalArray.length)
                throw new Error(
                    'fc.*{s|S}ubarrayOf expects the minimal length to be between 0 and the size of the original array');
            if (maxLength < 0 || maxLength > originalArray.length)
                throw new Error(
                    'fc.*{s|S}ubarrayOf expects the maximal length to be between 0 and the size of the original array');
            if (minLength > maxLength)
                throw new Error(
                    'fc.*{s|S}ubarrayOf expects the minimal length to be inferior or equal to the maximal length');
            this.lengthArb = new IntegerArbitrary_1.IntegerArbitrary(minLength, maxLength);
            this.biasedLengthArb = minLength !== maxLength
                ? new IntegerArbitrary_1.IntegerArbitrary(
                      minLength,
                      minLength + Math.floor(Math.log(maxLength - minLength) / Math.log(2)))
                : this.lengthArb;
        }
        generate(mrng, biasFactor) {
            const lengthArb = biasFactor !== undefined && mrng.nextInt(1, biasFactor) === 1
                ? this.biasedLengthArb
                : this.lengthArb;
            const size = lengthArb.generate(mrng, undefined);
            const sizeValue = size.value;
            const remainingElements = this.originalArray.map((_v, idx) => idx);
            const ids = [];
            for (let index = 0; index !== sizeValue; ++index) {
                const selectedIdIndex = mrng.nextInt(0, remainingElements.length - 1);
                ids.push(remainingElements[selectedIdIndex]);
                remainingElements.splice(selectedIdIndex, 1);
            }
            if (this.isOrdered) {
                ids.sort((a, b) => a - b);
            }
            return new Value_1$2.Value(ids.map((i) => this.originalArray[i]), size.context);
        }
        canShrinkWithoutContext(value) {
            if (!Array.isArray(value)) {
                return false;
            }
            if (!this.lengthArb.canShrinkWithoutContext(value.length)) {
                return false;
            }
            return (0, IsSubarrayOf_1.isSubarrayOf)(this.originalArray, value);
        }
        shrink(value, context) {
            if (value.length === 0) {
                return Stream_1$2.Stream.nil();
            }
            return this.lengthArb.shrink(value.length, context)
                .map((newSize) => {
                    return new Value_1$2.Value(value.slice(value.length - newSize.value),
                                               newSize.context);
                })
                .join(value.length > this.minLength
                          ? (0, LazyIterableIterator_1$1.makeLazy)(
                                () => this.shrink(value.slice(1), undefined)
                                          .filter((newValue) =>
                                                      this.minLength <= newValue.value.length + 1)
                                          .map((newValue) => new Value_1$2.Value(
                                                   [value[0]].concat(newValue.value), undefined)))
                          : Stream_1$2.Stream.nil());
        }
    }
    SubarrayArbitrary$1.SubarrayArbitrary = SubarrayArbitrary;

    Object.defineProperty(subarray$1, "__esModule", {value: true});
    subarray$1.subarray = void 0;
    const SubarrayArbitrary_1$1 = SubarrayArbitrary$1;
    function subarray(originalArray, constraints = {}) {
        const {minLength = 0, maxLength = originalArray.length} = constraints;
        return new SubarrayArbitrary_1$1.SubarrayArbitrary(
            originalArray, true, minLength, maxLength);
    }
    subarray$1.subarray = subarray;

    var shuffledSubarray$1 = {};

    Object.defineProperty(shuffledSubarray$1, "__esModule", {value: true});
    shuffledSubarray$1.shuffledSubarray = void 0;
    const SubarrayArbitrary_1 = SubarrayArbitrary$1;
    function shuffledSubarray(originalArray, constraints = {}) {
        const {minLength = 0, maxLength = originalArray.length} = constraints;
        return new SubarrayArbitrary_1.SubarrayArbitrary(
            originalArray, false, minLength, maxLength);
    }
    shuffledSubarray$1.shuffledSubarray = shuffledSubarray;

    var uuid$1 = {};

    var PaddedNumberArbitraryBuilder = {};

    var NumberToPaddedEight = {};

    Object.defineProperty(NumberToPaddedEight, "__esModule", {value: true});
    NumberToPaddedEight.numberToPaddedEightUnmapper =
        NumberToPaddedEight.numberToPaddedEightMapper = void 0;
    function numberToPaddedEightMapper(n) {
        return n.toString(16).padStart(8, '0');
    }
    NumberToPaddedEight.numberToPaddedEightMapper = numberToPaddedEightMapper;
    function numberToPaddedEightUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported type');
        }
        if (value.length !== 8) {
            throw new Error('Unsupported value: invalid length');
        }
        const n = parseInt(value, 16);
        if (value !== numberToPaddedEightMapper(n)) {
            throw new Error('Unsupported value: invalid content');
        }
        return n;
    }
    NumberToPaddedEight.numberToPaddedEightUnmapper = numberToPaddedEightUnmapper;

    Object.defineProperty(PaddedNumberArbitraryBuilder, "__esModule", {value: true});
    PaddedNumberArbitraryBuilder.buildPaddedNumberArbitrary = void 0;
    const integer_1 = integer$1;
    const NumberToPaddedEight_1 = NumberToPaddedEight;
    function buildPaddedNumberArbitrary(min, max) {
        return (0, integer_1.integer)({min, max})
            .map(NumberToPaddedEight_1.numberToPaddedEightMapper,
                 NumberToPaddedEight_1.numberToPaddedEightUnmapper);
    }
    PaddedNumberArbitraryBuilder.buildPaddedNumberArbitrary = buildPaddedNumberArbitrary;

    var PaddedEightsToUuid = {};

    Object.defineProperty(PaddedEightsToUuid, "__esModule", {value: true});
    PaddedEightsToUuid.paddedEightsToUuidUnmapper = PaddedEightsToUuid.paddedEightsToUuidMapper =
        void 0;
    function paddedEightsToUuidMapper(t) {
        return `${t[0]}-${t[1].substring(4)}-${t[1].substring(0, 4)}-${t[2].substring(0, 4)}-${
            t[2].substring(4)}${t[3]}`;
    }
    PaddedEightsToUuid.paddedEightsToUuidMapper = paddedEightsToUuidMapper;
    const UuidRegex = /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/;
    function paddedEightsToUuidUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported type');
        }
        const m = UuidRegex.exec(value);
        if (m === null) {
            throw new Error('Unsupported type');
        }
        return [m[1], m[3] + m[2], m[4] + m[5].substring(0, 4), m[5].substring(4)];
    }
    PaddedEightsToUuid.paddedEightsToUuidUnmapper = paddedEightsToUuidUnmapper;

    Object.defineProperty(uuid$1, "__esModule", {value: true});
    uuid$1.uuid = void 0;
    const tuple_1$3 = tuple$1;
    const PaddedNumberArbitraryBuilder_1$1 = PaddedNumberArbitraryBuilder;
    const PaddedEightsToUuid_1$1 = PaddedEightsToUuid;
    function uuid() {
        const padded =
            (0, PaddedNumberArbitraryBuilder_1$1.buildPaddedNumberArbitrary)(0, 0xffffffff);
        const secondPadded = (0, PaddedNumberArbitraryBuilder_1$1.buildPaddedNumberArbitrary)(
            0x10000000, 0x5fffffff);
        const thirdPadded = (0, PaddedNumberArbitraryBuilder_1$1.buildPaddedNumberArbitrary)(
            0x80000000, 0xbfffffff);
        return (0, tuple_1$3.tuple)(padded, secondPadded, thirdPadded, padded)
            .map(PaddedEightsToUuid_1$1.paddedEightsToUuidMapper,
                 PaddedEightsToUuid_1$1.paddedEightsToUuidUnmapper);
    }
    uuid$1.uuid = uuid;

    var uuidV$1 = {};

    Object.defineProperty(uuidV$1, "__esModule", {value: true});
    uuidV$1.uuidV = void 0;
    const tuple_1$2 = tuple$1;
    const PaddedNumberArbitraryBuilder_1 = PaddedNumberArbitraryBuilder;
    const PaddedEightsToUuid_1 = PaddedEightsToUuid;
    function uuidV(versionNumber) {
        const padded =
            (0, PaddedNumberArbitraryBuilder_1.buildPaddedNumberArbitrary)(0, 0xffffffff);
        const offsetSecond = versionNumber * 0x10000000;
        const secondPadded = (0, PaddedNumberArbitraryBuilder_1.buildPaddedNumberArbitrary)(
            offsetSecond, offsetSecond + 0x0fffffff);
        const thirdPadded =
            (0, PaddedNumberArbitraryBuilder_1.buildPaddedNumberArbitrary)(0x80000000, 0xbfffffff);
        return (0, tuple_1$2.tuple)(padded, secondPadded, thirdPadded, padded)
            .map(PaddedEightsToUuid_1.paddedEightsToUuidMapper,
                 PaddedEightsToUuid_1.paddedEightsToUuidUnmapper);
    }
    uuidV$1.uuidV = uuidV;

    var webAuthority$1 = {};

    Object.defineProperty(webAuthority$1, "__esModule", {value: true});
    webAuthority$1.webAuthority = void 0;
    const CharacterRangeArbitraryBuilder_1$2 = CharacterRangeArbitraryBuilder;
    const constant_1$1 = constant$1;
    const domain_1 = domain$1;
    const ipV4_1 = ipV4$1;
    const ipV4Extended_1 = ipV4Extended$1;
    const ipV6_1 = ipV6$1;
    const nat_1 = nat$1;
    const oneof_1$1 = oneof$1;
    const option_1$1 = option$1;
    const stringOf_1$2 = stringOf$1;
    const tuple_1$1 = tuple$1;
    function hostUserInfo(size) {
        const others =
            ['-', '.', '_', '~', '!', '$', '&', "'", '(', ')', '*', '+', ',', ';', '=', ':'];
        return (0, stringOf_1$2.stringOf)(
            (0, CharacterRangeArbitraryBuilder_1$2.buildAlphaNumericPercentArbitrary)(others),
            {size});
    }
    function userHostPortMapper([u, h, p]) {
        return (u === null ? '' : `${u}@`) + h + (p === null ? '' : `:${p}`);
    }
    function userHostPortUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Unsupported');
        }
        const atPosition = value.indexOf('@');
        const user = atPosition !== -1 ? value.substring(0, atPosition) : null;
        const portRegex = /:(\d+)$/;
        const m = portRegex.exec(value);
        const port = m !== null ? Number(m[1]) : null;
        const host = m !== null ? value.substring(atPosition + 1, value.length - m[1].length - 1)
                                : value.substring(atPosition + 1);
        return [user, host, port];
    }
    function bracketedMapper(s) {
        return `[${s}]`;
    }
    function bracketedUnmapper(value) {
        if (typeof value !== 'string' || value[0] !== '[' || value[value.length - 1] !== ']') {
            throw new Error('Unsupported');
        }
        return value.substring(1, value.length - 1);
    }
    function webAuthority(constraints) {
        const c = constraints || {};
        const size = c.size;
        const hostnameArbs =
            [(0, domain_1.domain)({size})]
                .concat(c.withIPv4 === true ? [(0, ipV4_1.ipV4)()] : [])
                .concat(c.withIPv6 === true
                            ? [(0, ipV6_1.ipV6)().map(bracketedMapper, bracketedUnmapper)]
                            : [])
                .concat(c.withIPv4Extended === true ? [(0, ipV4Extended_1.ipV4Extended)()] : []);
        return (0, tuple_1$1.tuple)(
                   c.withUserInfo === true ? (0, option_1$1.option)(hostUserInfo(size))
                                           : (0, constant_1$1.constant)(null),
                   (0, oneof_1$1.oneof)(...hostnameArbs),
                   c.withPort === true ? (0, option_1$1.option)((0, nat_1.nat)(65535))
                                       : (0, constant_1$1.constant)(null))
            .map(userHostPortMapper, userHostPortUnmapper);
    }
    webAuthority$1.webAuthority = webAuthority;

    var webFragments$1 = {};

    var UriQueryOrFragmentArbitraryBuilder = {};

    Object.defineProperty(UriQueryOrFragmentArbitraryBuilder, "__esModule", {value: true});
    UriQueryOrFragmentArbitraryBuilder.buildUriQueryOrFragmentArbitrary = void 0;
    const CharacterRangeArbitraryBuilder_1$1 = CharacterRangeArbitraryBuilder;
    const stringOf_1$1 = stringOf$1;
    function buildUriQueryOrFragmentArbitrary(size) {
        const others = [
            '-',
            '.',
            '_',
            '~',
            '!',
            '$',
            '&',
            "'",
            '(',
            ')',
            '*',
            '+',
            ',',
            ';',
            '=',
            ':',
            '@',
            '/',
            '?'
        ];
        return (0, stringOf_1$1.stringOf)(
            (0, CharacterRangeArbitraryBuilder_1$1.buildAlphaNumericPercentArbitrary)(others),
            {size});
    }
    UriQueryOrFragmentArbitraryBuilder.buildUriQueryOrFragmentArbitrary =
        buildUriQueryOrFragmentArbitrary;

    Object.defineProperty(webFragments$1, "__esModule", {value: true});
    webFragments$1.webFragments = void 0;
    const UriQueryOrFragmentArbitraryBuilder_1$1 = UriQueryOrFragmentArbitraryBuilder;
    function webFragments(constraints = {}) {
        return (0, UriQueryOrFragmentArbitraryBuilder_1$1.buildUriQueryOrFragmentArbitrary)(
            constraints.size);
    }
    webFragments$1.webFragments = webFragments;

    var webQueryParameters$1 = {};

    Object.defineProperty(webQueryParameters$1, "__esModule", {value: true});
    webQueryParameters$1.webQueryParameters = void 0;
    const UriQueryOrFragmentArbitraryBuilder_1 = UriQueryOrFragmentArbitraryBuilder;
    function webQueryParameters(constraints = {}) {
        return (0, UriQueryOrFragmentArbitraryBuilder_1.buildUriQueryOrFragmentArbitrary)(
            constraints.size);
    }
    webQueryParameters$1.webQueryParameters = webQueryParameters;

    var webSegment$1 = {};

    Object.defineProperty(webSegment$1, "__esModule", {value: true});
    webSegment$1.webSegment = void 0;
    const CharacterRangeArbitraryBuilder_1 = CharacterRangeArbitraryBuilder;
    const stringOf_1 = stringOf$1;
    function webSegment(constraints = {}) {
        const others =
            ['-', '.', '_', '~', '!', '$', '&', "'", '(', ')', '*', '+', ',', ';', '=', ':', '@'];
        return (0, stringOf_1.stringOf)(
            (0, CharacterRangeArbitraryBuilder_1.buildAlphaNumericPercentArbitrary)(others),
            {size: constraints.size});
    }
    webSegment$1.webSegment = webSegment;

    var webUrl$1 = {};

    var PartsToUrl = {};

    Object.defineProperty(PartsToUrl, "__esModule", {value: true});
    PartsToUrl.partsToUrlUnmapper = PartsToUrl.partsToUrlMapper = void 0;
    function partsToUrlMapper(data) {
        const [scheme, authority, path] = data;
        const query = data[3] === null ? '' : `?${data[3]}`;
        const fragments = data[4] === null ? '' : `#${data[4]}`;
        return `${scheme}://${authority}${path}${query}${fragments}`;
    }
    PartsToUrl.partsToUrlMapper = partsToUrlMapper;
    const UrlSplitRegex =
        /^([[A-Za-z][A-Za-z0-9+.-]*):\/\/([^/?#]*)([^?#]*)(\?[A-Za-z0-9\-._~!$&'()*+,;=:@/?%]*)?(#[A-Za-z0-9\-._~!$&'()*+,;=:@/?%]*)?$/;
    function partsToUrlUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Incompatible value received: type');
        }
        const m = UrlSplitRegex.exec(value);
        if (m === null) {
            throw new Error('Incompatible value received');
        }
        const scheme = m[1];
        const authority = m[2];
        const path = m[3];
        const query = m[4];
        const fragments = m[5];
        return [
            scheme,
            authority,
            path,
            query !== undefined ? query.substring(1) : null,
            fragments !== undefined ? fragments.substring(1) : null,
        ];
    }
    PartsToUrl.partsToUrlUnmapper = partsToUrlUnmapper;

    var UriPathArbitraryBuilder = {};

    var SegmentsToPath = {};

    Object.defineProperty(SegmentsToPath, "__esModule", {value: true});
    SegmentsToPath.segmentsToPathUnmapper = SegmentsToPath.segmentsToPathMapper = void 0;
    function segmentsToPathMapper(segments) {
        return segments.map((v) => `/${v}`).join('');
    }
    SegmentsToPath.segmentsToPathMapper = segmentsToPathMapper;
    function segmentsToPathUnmapper(value) {
        if (typeof value !== 'string') {
            throw new Error('Incompatible value received: type');
        }
        if (value.length !== 0 && value[0] !== '/') {
            throw new Error('Incompatible value received: start');
        }
        return value.split('/').splice(1);
    }
    SegmentsToPath.segmentsToPathUnmapper = segmentsToPathUnmapper;

    Object.defineProperty(UriPathArbitraryBuilder, "__esModule", {value: true});
    UriPathArbitraryBuilder.buildUriPathArbitrary = void 0;
    const webSegment_1 = webSegment$1;
    const array_1 = array$1;
    const SegmentsToPath_1 = SegmentsToPath;
    function sqrtSize(size) {
        switch (size) {
            case 'xsmall':
                return ['xsmall', 'xsmall'];
            case 'small':
                return ['small', 'xsmall'];
            case 'medium':
                return ['small', 'small'];
            case 'large':
                return ['medium', 'small'];
            case 'xlarge':
                return ['medium', 'medium'];
        }
    }
    function buildUriPathArbitrary(resolvedSize) {
        const [segmentSize, numSegmentSize] = sqrtSize(resolvedSize);
        return (0, array_1.array)((0, webSegment_1.webSegment)({size: segmentSize}),
                                  {size: numSegmentSize})
            .map(SegmentsToPath_1.segmentsToPathMapper, SegmentsToPath_1.segmentsToPathUnmapper);
    }
    UriPathArbitraryBuilder.buildUriPathArbitrary = buildUriPathArbitrary;

    Object.defineProperty(webUrl$1, "__esModule", {value: true});
    webUrl$1.webUrl = void 0;
    const constantFrom_1 = constantFrom$1;
    const constant_1 = constant$1;
    const option_1 = option$1;
    const tuple_1 = tuple$1;
    const webQueryParameters_1 = webQueryParameters$1;
    const webFragments_1 = webFragments$1;
    const webAuthority_1 = webAuthority$1;
    const PartsToUrl_1 = PartsToUrl;
    const MaxLengthFromMinLength_1$1 = MaxLengthFromMinLength;
    const UriPathArbitraryBuilder_1 = UriPathArbitraryBuilder;
    function webUrl(constraints) {
        const c = constraints || {};
        const resolvedSize = (0, MaxLengthFromMinLength_1$1.resolveSize)(c.size);
        const resolvedAuthoritySettingsSize =
            c.authoritySettings !== undefined && c.authoritySettings.size !== undefined
            ? (0, MaxLengthFromMinLength_1$1.relativeSizeToSize)(c.authoritySettings.size,
                                                                 resolvedSize)
            : resolvedSize;
        const resolvedAuthoritySettings = Object.assign(Object.assign({}, c.authoritySettings),
                                                        {size: resolvedAuthoritySettingsSize});
        const validSchemes = c.validSchemes || ['http', 'https'];
        const schemeArb = (0, constantFrom_1.constantFrom)(...validSchemes);
        const authorityArb = (0, webAuthority_1.webAuthority)(resolvedAuthoritySettings);
        const pathArb = (0, UriPathArbitraryBuilder_1.buildUriPathArbitrary)(resolvedSize);
        return (0, tuple_1.tuple)(
                   schemeArb,
                   authorityArb,
                   pathArb,
                   c.withQueryParameters === true
                       ? (0, option_1.option)(
                             (0, webQueryParameters_1.webQueryParameters)({size: resolvedSize}))
                       : (0, constant_1.constant)(null),
                   c.withFragments === true ? (0, option_1.option)((0, webFragments_1.webFragments)(
                                                  {size: resolvedSize}))
                                            : (0, constant_1.constant)(null))
            .map(PartsToUrl_1.partsToUrlMapper, PartsToUrl_1.partsToUrlUnmapper);
    }
    webUrl$1.webUrl = webUrl;

    var commands$1 = {};

    var CommandsArbitrary$1 = {};

    var CommandsIterable$1 = {};

    Object.defineProperty(CommandsIterable$1, "__esModule", {value: true});
    CommandsIterable$1.CommandsIterable = void 0;
    const symbols_1$2 = symbols;
    class CommandsIterable {
        constructor(commands, metadataForReplay) {
            this.commands = commands;
            this.metadataForReplay = metadataForReplay;
        }
        [Symbol.iterator]() {
            return this.commands[Symbol.iterator]();
        }
        [symbols_1$2.cloneMethod]() {
            return new CommandsIterable(this.commands.map((c) => c.clone()),
                                        this.metadataForReplay);
        }
        toString() {
            const serializedCommands =
                this.commands.filter((c) => c.hasRan).map((c) => c.toString()).join(',');
            const metadata = this.metadataForReplay();
            return metadata.length !== 0 ? `${serializedCommands} /*${metadata}*/`
                                         : serializedCommands;
        }
    }
    CommandsIterable$1.CommandsIterable = CommandsIterable;

    var CommandWrapper$1 = {};

    Object.defineProperty(CommandWrapper$1, "__esModule", {value: true});
    CommandWrapper$1.CommandWrapper = void 0;
    const stringify_1$1 = stringify;
    const symbols_1$1 = symbols;
    class CommandWrapper {
        constructor(cmd) {
            this.cmd = cmd;
            this.hasRan = false;
            if ((0, stringify_1$1.hasToStringMethod)(cmd)) {
                const method = cmd[stringify_1$1.toStringMethod];
                this[stringify_1$1.toStringMethod] = function toStringMethod() {
                    return method.call(cmd);
                };
            }
            if ((0, stringify_1$1.hasAsyncToStringMethod)(cmd)) {
                const method = cmd[stringify_1$1.asyncToStringMethod];
                this[stringify_1$1.asyncToStringMethod] = function asyncToStringMethod() {
                    return method.call(cmd);
                };
            }
        }
        check(m) {
            return this.cmd.check(m);
        }
        run(m, r) {
            this.hasRan = true;
            return this.cmd.run(m, r);
        }
        clone() {
            if ((0, symbols_1$1.hasCloneMethod)(this.cmd))
                return new CommandWrapper(this.cmd[symbols_1$1.cloneMethod]());
            return new CommandWrapper(this.cmd);
        }
        toString() {
            return this.cmd.toString();
        }
    }
    CommandWrapper$1.CommandWrapper = CommandWrapper;

    var ReplayPath$1 = {};

    Object.defineProperty(ReplayPath$1, "__esModule", {value: true});
    ReplayPath$1.ReplayPath = void 0;
    class ReplayPath {
        static parse(replayPathStr) {
            const [serializedCount, serializedChanges] = replayPathStr.split(':');
            const counts = this.parseCounts(serializedCount);
            const changes = this.parseChanges(serializedChanges);
            return this.parseOccurences(counts, changes);
        }
        static stringify(replayPath) {
            const occurences = this.countOccurences(replayPath);
            const serializedCount = this.stringifyCounts(occurences);
            const serializedChanges = this.stringifyChanges(occurences);
            return `${serializedCount}:${serializedChanges}`;
        }
        static intToB64(n) {
            if (n < 26)
                return String.fromCharCode(n + 65);
            if (n < 52)
                return String.fromCharCode(n + 97 - 26);
            if (n < 62)
                return String.fromCharCode(n + 48 - 52);
            return String.fromCharCode(n === 62 ? 43 : 47);
        }
        static b64ToInt(c) {
            if (c >= 'a')
                return c.charCodeAt(0) - 97 + 26;
            if (c >= 'A')
                return c.charCodeAt(0) - 65;
            if (c >= '0')
                return c.charCodeAt(0) - 48 + 52;
            return c === '+' ? 62 : 63;
        }
        static countOccurences(replayPath) {
            return replayPath.reduce((counts, cur) => {
                if (counts.length === 0 || counts[counts.length - 1].count === 64 ||
                    counts[counts.length - 1].value !== cur)
                    counts.push({value: cur, count: 1});
                else
                    counts[counts.length - 1].count += 1;
                return counts;
            }, []);
        }
        static parseOccurences(counts, changes) {
            const replayPath = [];
            for (let idx = 0; idx !== counts.length; ++idx) {
                const count = counts[idx];
                const value = changes[idx];
                for (let num = 0; num !== count; ++num)
                    replayPath.push(value);
            }
            return replayPath;
        }
        static stringifyChanges(occurences) {
            let serializedChanges = '';
            for (let idx = 0; idx < occurences.length; idx += 6) {
                const changesInt =
                    occurences.slice(idx, idx + 6)
                        .reduceRight((prev, cur) => prev * 2 + (cur.value ? 1 : 0), 0);
                serializedChanges += this.intToB64(changesInt);
            }
            return serializedChanges;
        }
        static parseChanges(serializedChanges) {
            const changesInt = serializedChanges.split('').map((c) => this.b64ToInt(c));
            const changes = [];
            for (let idx = 0; idx !== changesInt.length; ++idx) {
                let current = changesInt[idx];
                for (let n = 0; n !== 6; ++n, current >>= 1) {
                    changes.push(current % 2 === 1);
                }
            }
            return changes;
        }
        static stringifyCounts(occurences) {
            return occurences.map(({count}) => this.intToB64(count - 1)).join('');
        }
        static parseCounts(serializedCount) {
            return serializedCount.split('').map((c) => this.b64ToInt(c) + 1);
        }
    }
    ReplayPath$1.ReplayPath = ReplayPath;

    Object.defineProperty(CommandsArbitrary$1, "__esModule", {value: true});
    CommandsArbitrary$1.CommandsArbitrary = void 0;
    const Arbitrary_1$1 = Arbitrary$1;
    const Value_1$1 = Value$1;
    const CommandsIterable_1 = CommandsIterable$1;
    const CommandWrapper_1 = CommandWrapper$1;
    const ReplayPath_1 = ReplayPath$1;
    const LazyIterableIterator_1 = LazyIterableIterator$1;
    const Stream_1$1 = Stream$1;
    const oneof_1 = oneof$1;
    const RestrictedIntegerArbitraryBuilder_1 = RestrictedIntegerArbitraryBuilder;
    class CommandsArbitrary extends Arbitrary_1$1.Arbitrary {
        constructor(
            commandArbs, maxGeneratedCommands, maxCommands, sourceReplayPath, disableReplayLog) {
            super();
            this.sourceReplayPath = sourceReplayPath;
            this.disableReplayLog = disableReplayLog;
            this.oneCommandArb = (0, oneof_1.oneof)(...commandArbs)
                                     .map((c) => new CommandWrapper_1.CommandWrapper(c));
            this.lengthArb =
                (0, RestrictedIntegerArbitraryBuilder_1.restrictedIntegerArbitraryBuilder)(
                    0, maxGeneratedCommands, maxCommands);
            this.replayPath = [];
            this.replayPathPosition = 0;
        }
        metadataForReplay() {
            return this.disableReplayLog
                ? ''
                : `replayPath=${
                      JSON.stringify(ReplayPath_1.ReplayPath.stringify(this.replayPath))}`;
        }
        buildValueFor(items, shrunkOnce) {
            const commands = items.map((item) => item.value_);
            const context = {shrunkOnce, items};
            return new Value_1$1.Value(
                new CommandsIterable_1.CommandsIterable(commands, () => this.metadataForReplay()),
                context);
        }
        generate(mrng) {
            const size = this.lengthArb.generate(mrng, undefined);
            const sizeValue = size.value;
            const items = Array(sizeValue);
            for (let idx = 0; idx !== sizeValue; ++idx) {
                const item = this.oneCommandArb.generate(mrng, undefined);
                items[idx] = item;
            }
            this.replayPathPosition = 0;
            return this.buildValueFor(items, false);
        }
        canShrinkWithoutContext(value) {
            return false;
        }
        filterOnExecution(itemsRaw) {
            const items = [];
            for (const c of itemsRaw) {
                if (c.value_.hasRan) {
                    this.replayPath.push(true);
                    items.push(c);
                } else
                    this.replayPath.push(false);
            }
            return items;
        }
        filterOnReplay(itemsRaw) {
            return itemsRaw.filter((c, idx) => {
                const state = this.replayPath[this.replayPathPosition + idx];
                if (state === undefined)
                    throw new Error(`Too short replayPath`);
                if (!state && c.value_.hasRan)
                    throw new Error(`Mismatch between replayPath and real execution`);
                return state;
            });
        }
        filterForShrinkImpl(itemsRaw) {
            if (this.replayPathPosition === 0) {
                this.replayPath = this.sourceReplayPath !== null
                    ? ReplayPath_1.ReplayPath.parse(this.sourceReplayPath)
                    : [];
            }
            const items = this.replayPathPosition < this.replayPath.length
                ? this.filterOnReplay(itemsRaw)
                : this.filterOnExecution(itemsRaw);
            this.replayPathPosition += itemsRaw.length;
            return items;
        }
        shrink(_value, context) {
            if (context === undefined) {
                return Stream_1$1.Stream.nil();
            }
            const safeContext = context;
            const shrunkOnce = safeContext.shrunkOnce;
            const itemsRaw = safeContext.items;
            const items = this.filterForShrinkImpl(itemsRaw);
            if (items.length === 0) {
                return Stream_1$1.Stream.nil();
            }
            const rootShrink = shrunkOnce ? Stream_1$1.Stream.nil()
                                          : new Stream_1$1.Stream([[]][Symbol.iterator]());
            const nextShrinks = [];
            for (let numToKeep = 0; numToKeep !== items.length; ++numToKeep) {
                nextShrinks.push((0, LazyIterableIterator_1.makeLazy)(() => {
                    const fixedStart = items.slice(0, numToKeep);
                    return this.lengthArb.shrink(items.length - 1 - numToKeep, undefined)
                        .map((l) => fixedStart.concat(items.slice(items.length - (l.value + 1))));
                }));
            }
            for (let itemAt = 0; itemAt !== items.length; ++itemAt) {
                nextShrinks.push((0, LazyIterableIterator_1.makeLazy)(
                    () => this.oneCommandArb.shrink(items[itemAt].value_, items[itemAt].context)
                              .map((v) => items.slice(0, itemAt).concat([v],
                                                                        items.slice(itemAt + 1)))));
            }
            return rootShrink.join(...nextShrinks).map((shrinkables) => {
                return this.buildValueFor(
                    shrinkables.map((c) => new Value_1$1.Value(c.value_.clone(), c.context)), true);
            });
        }
    }
    CommandsArbitrary$1.CommandsArbitrary = CommandsArbitrary;

    Object.defineProperty(commands$1, "__esModule", {value: true});
    commands$1.commands = void 0;
    const CommandsArbitrary_1 = CommandsArbitrary$1;
    const MaxLengthFromMinLength_1 = MaxLengthFromMinLength;
    function commands(commandArbs, constraints = {}) {
        const {
            size,
            maxCommands = MaxLengthFromMinLength_1.MaxLengthUpperBound,
            disableReplayLog = false,
            replayPath = null
        } = constraints;
        const specifiedMaxCommands = constraints.maxCommands !== undefined;
        const maxGeneratedCommands =
            (0, MaxLengthFromMinLength_1.maxGeneratedLengthFromSizeForArbitrary)(
                size, 0, maxCommands, specifiedMaxCommands);
        return new CommandsArbitrary_1.CommandsArbitrary(
            commandArbs, maxGeneratedCommands, maxCommands, replayPath, disableReplayLog);
    }
    commands$1.commands = commands;

    var ModelRunner = {};

    var ScheduledCommand$1 = {};

    Object.defineProperty(ScheduledCommand$1, "__esModule", {value: true});
    ScheduledCommand$1.scheduleCommands = ScheduledCommand$1.ScheduledCommand = void 0;
    class ScheduledCommand {
        constructor(s, cmd) {
            this.s = s;
            this.cmd = cmd;
        }
        async check(m) {
            let error = null;
            let checkPassed = false;
            const status = await this.s
                               .scheduleSequence([
                                   {
                                       label: `check@${this.cmd.toString()}`,
                                       builder: async () => {
                                           try {
                                               checkPassed =
                                                   await Promise.resolve(this.cmd.check(m));
                                           } catch (err) {
                                               error = err;
                                               throw err;
                                           }
                                       },
                                   },
                               ])
                               .task;
            if (status.faulty) {
                throw error;
            }
            return checkPassed;
        }
        async run(m, r) {
            let error = null;
            const status = await this.s
                               .scheduleSequence([
                                   {
                                       label: `run@${this.cmd.toString()}`,
                                       builder: async () => {
                                           try {
                                               await this.cmd.run(m, r);
                                           } catch (err) {
                                               error = err;
                                               throw err;
                                           }
                                       },
                                   },
                               ])
                               .task;
            if (status.faulty) {
                throw error;
            }
        }
    }
    ScheduledCommand$1.ScheduledCommand = ScheduledCommand;
    const scheduleCommands = function*(s, cmds) {
        for (const cmd of cmds) {
            yield new ScheduledCommand(s, cmd);
        }
    };
    ScheduledCommand$1.scheduleCommands = scheduleCommands;

    Object.defineProperty(ModelRunner, "__esModule", {value: true});
    ModelRunner.scheduledModelRun = ModelRunner.asyncModelRun = ModelRunner.modelRun = void 0;
    const ScheduledCommand_1 = ScheduledCommand$1;
    const genericModelRun = (s, cmds, initialValue, runCmd, then) => {
        return s.then((o) => {
            const {model, real} = o;
            let state = initialValue;
            for (const c of cmds) {
                state = then(state, () => {
                    return runCmd(c, model, real);
                });
            }
            return state;
        });
    };
    const internalModelRun = (s, cmds) => {
        const then = (_p, c) => c();
        const setupProducer = {
            then: (fun) => {
                fun(s());
                return undefined;
            },
        };
        const runSync = (cmd, m, r) => {
            if (cmd.check(m))
                cmd.run(m, r);
            return undefined;
        };
        return genericModelRun(setupProducer, cmds, undefined, runSync, then);
    };
    const isAsyncSetup = (s) => {
        return typeof s.then === 'function';
    };
    const internalAsyncModelRun = async (s, cmds, defaultPromise = Promise.resolve()) => {
        const then = (p, c) => p.then(c);
        const setupProducer = {
            then: (fun) => {
                const out = s();
                if (isAsyncSetup(out))
                    return out.then(fun);
                else
                    return fun(out);
            },
        };
        const runAsync = async (cmd, m, r) => {
            if (await cmd.check(m))
                await cmd.run(m, r);
        };
        return await genericModelRun(setupProducer, cmds, defaultPromise, runAsync, then);
    };
    function modelRun(s, cmds) {
        internalModelRun(s, cmds);
    }
    ModelRunner.modelRun = modelRun;
    async function asyncModelRun(s, cmds) {
        await internalAsyncModelRun(s, cmds);
    }
    ModelRunner.asyncModelRun = asyncModelRun;
    async function scheduledModelRun(scheduler, s, cmds) {
        const scheduledCommands = (0, ScheduledCommand_1.scheduleCommands)(scheduler, cmds);
        const out = internalAsyncModelRun(
            s, scheduledCommands, scheduler.schedule(Promise.resolve(), 'startModel'));
        await scheduler.waitAll();
        await out;
    }
    ModelRunner.scheduledModelRun = scheduledModelRun;

    var scheduler$1 = {};

    var BuildSchedulerFor = {};

    var SchedulerImplem$1 = {};

    Object.defineProperty(SchedulerImplem$1, "__esModule", {value: true});
    SchedulerImplem$1.SchedulerImplem = void 0;
    const TextEscaper_1 = TextEscaper;
    const symbols_1 = symbols;
    const stringify_1 = stringify;
    class SchedulerImplem {
        constructor(act, taskSelector) {
            this.act = act;
            this.taskSelector = taskSelector;
            this.lastTaskId = 0;
            this.sourceTaskSelector = taskSelector.clone();
            this.scheduledTasks = [];
            this.triggeredTasks = [];
            this.scheduledWatchers = [];
        }
        static buildLog(reportItem) {
            return `[task\${${reportItem.taskId}}] ${
                reportItem.label.length !== 0 ? `${reportItem.schedulingType}::${reportItem.label}`
                                              : reportItem.schedulingType} ${reportItem.status}${
                reportItem.outputValue !== undefined
                    ? ` with value ${
                          (0, TextEscaper_1.escapeForTemplateString)(reportItem.outputValue)}`
                    : ''}`;
        }
        log(schedulingType, taskId, label, metadata, status, data) {
            this.triggeredTasks.push({
                status,
                schedulingType,
                taskId,
                label,
                metadata,
                outputValue: data !== undefined ? (0, stringify_1.stringify)(data) : undefined,
            });
        }
        scheduleInternal(schedulingType, label, task, metadata, thenTaskToBeAwaited) {
            let trigger = null;
            const taskId = ++this.lastTaskId;
            const scheduledPromise = new Promise((resolve, reject) => {
                trigger = () => {
                    (thenTaskToBeAwaited ? task.then(() => thenTaskToBeAwaited()) : task)
                        .then(
                            (data) => {
                                this.log(schedulingType, taskId, label, metadata, 'resolved', data);
                                return resolve(data);
                            },
                            (err) => {
                                this.log(schedulingType, taskId, label, metadata, 'rejected', err);
                                return reject(err);
                            });
                };
            });
            this.scheduledTasks.push({
                original: task,
                scheduled: scheduledPromise,
                trigger: trigger,
                schedulingType,
                taskId,
                label,
                metadata,
            });
            if (this.scheduledWatchers.length !== 0) {
                this.scheduledWatchers[0]();
            }
            return scheduledPromise;
        }
        schedule(task, label, metadata) {
            return this.scheduleInternal('promise', label || '', task, metadata);
        }
        scheduleFunction(asyncFunction) {
            return (...args) => this.scheduleInternal(
                       'function',
                       `${asyncFunction.name}(${args.map(stringify_1.stringify).join(',')})`,
                       asyncFunction(...args),
                       undefined);
        }
        scheduleSequence(sequenceBuilders) {
            const status = {done: false, faulty: false};
            const dummyResolvedPromise = {then: (f) => f()};
            let resolveSequenceTask = () => {};
            const sequenceTask = new Promise((resolve) => (resolveSequenceTask = resolve));
            sequenceBuilders
                .reduce(
                    (previouslyScheduled, item) => {
                        const [builder, label, metadata] = typeof item === 'function'
                            ? [item, item.name, undefined]
                            : [item.builder, item.label, item.metadata];
                        return previouslyScheduled.then(() => {
                            const scheduled = this.scheduleInternal(
                                'sequence', label, dummyResolvedPromise, metadata, () => builder());
                            scheduled.catch(() => {
                                status.faulty = true;
                                resolveSequenceTask();
                            });
                            return scheduled;
                        });
                    },
                    dummyResolvedPromise)
                .then(() => {
                    status.done = true;
                    resolveSequenceTask();
                }, () => {});
            return Object.assign(status, {
                task: Promise.resolve(sequenceTask).then(() => {
                    return {done: status.done, faulty: status.faulty};
                }),
            });
        }
        count() {
            return this.scheduledTasks.length;
        }
        async internalWaitOne() {
            if (this.scheduledTasks.length === 0) {
                throw new Error('No task scheduled');
            }
            const taskIndex = this.taskSelector.nextTaskIndex(this.scheduledTasks);
            const [scheduledTask] = this.scheduledTasks.splice(taskIndex, 1);
            scheduledTask.trigger();
            try {
                await scheduledTask.scheduled;
            } catch (_err) {
            }
        }
        async waitOne() {
            await this.act(async () => await this.internalWaitOne());
        }
        async waitAll() {
            while (this.scheduledTasks.length > 0) {
                await this.waitOne();
            }
        }
        async waitFor(unscheduledTask) {
            let taskResolved = false;
            let awaiterPromise = null;
            const awaiter = async () => {
                while (!taskResolved && this.scheduledTasks.length > 0) {
                    await this.waitOne();
                }
                awaiterPromise = null;
            };
            const handleNotified = () => {
                if (awaiterPromise !== null) {
                    return;
                }
                awaiterPromise = Promise.resolve().then(awaiter);
            };
            const clearAndReplaceWatcher = () => {
                const handleNotifiedIndex = this.scheduledWatchers.indexOf(handleNotified);
                if (handleNotifiedIndex !== -1) {
                    this.scheduledWatchers.splice(handleNotifiedIndex, 1);
                }
                if (handleNotifiedIndex === 0 && this.scheduledWatchers.length !== 0) {
                    this.scheduledWatchers[0]();
                }
            };
            const rewrappedTask = unscheduledTask.then(
                (ret) => {
                    taskResolved = true;
                    if (awaiterPromise === null) {
                        clearAndReplaceWatcher();
                        return ret;
                    }
                    return awaiterPromise.then(() => {
                        clearAndReplaceWatcher();
                        return ret;
                    });
                },
                (err) => {
                    taskResolved = true;
                    if (awaiterPromise === null) {
                        clearAndReplaceWatcher();
                        throw err;
                    }
                    return awaiterPromise.then(() => {
                        clearAndReplaceWatcher();
                        throw err;
                    });
                });
            if (this.scheduledTasks.length > 0 && this.scheduledWatchers.length === 0) {
                handleNotified();
            }
            this.scheduledWatchers.push(handleNotified);
            return rewrappedTask;
        }
        report() {
            return [
                ...this.triggeredTasks,
                ...this.scheduledTasks.map((t) => ({
                                               status: 'pending',
                                               schedulingType: t.schedulingType,
                                               taskId: t.taskId,
                                               label: t.label,
                                               metadata: t.metadata,
                                           })),
            ];
        }
        toString() {
            return (
                'schedulerFor()`\n' +
                this.report().map(SchedulerImplem.buildLog).map((log) => `-> ${log}`).join('\n') +
                '`');
        }
        [symbols_1.cloneMethod]() {
            return new SchedulerImplem(this.act, this.sourceTaskSelector);
        }
    }
    SchedulerImplem$1.SchedulerImplem = SchedulerImplem;

    Object.defineProperty(BuildSchedulerFor, "__esModule", {value: true});
    BuildSchedulerFor.buildSchedulerFor = void 0;
    const SchedulerImplem_1$1 = SchedulerImplem$1;
    function buildNextTaskIndex$1(ordering) {
        let numTasks = 0;
        return {
            clone: () => buildNextTaskIndex$1(ordering),
            nextTaskIndex: (scheduledTasks) => {
                if (ordering.length <= numTasks) {
                    throw new Error(
                        `Invalid schedulerFor defined: too many tasks have been scheduled`);
                }
                const taskIndex = scheduledTasks.findIndex((t) => t.taskId === ordering[numTasks]);
                if (taskIndex === -1) {
                    throw new Error(`Invalid schedulerFor defined: unable to find next task`);
                }
                ++numTasks;
                return taskIndex;
            },
        };
    }
    function buildSchedulerFor(act, ordering) {
        return new SchedulerImplem_1$1.SchedulerImplem(act, buildNextTaskIndex$1(ordering));
    }
    BuildSchedulerFor.buildSchedulerFor = buildSchedulerFor;

    var SchedulerArbitrary$1 = {};

    Object.defineProperty(SchedulerArbitrary$1, "__esModule", {value: true});
    SchedulerArbitrary$1.SchedulerArbitrary = void 0;
    const Arbitrary_1 = Arbitrary$1;
    const Value_1 = Value$1;
    const Stream_1 = Stream$1;
    const SchedulerImplem_1 = SchedulerImplem$1;
    function buildNextTaskIndex(mrng) {
        const clonedMrng = mrng.clone();
        return {
            clone: () => buildNextTaskIndex(clonedMrng),
            nextTaskIndex: (scheduledTasks) => {
                return mrng.nextInt(0, scheduledTasks.length - 1);
            },
        };
    }
    class SchedulerArbitrary extends Arbitrary_1.Arbitrary {
        constructor(act) {
            super();
            this.act = act;
        }
        generate(mrng, _biasFactor) {
            return new Value_1.Value(
                new SchedulerImplem_1.SchedulerImplem(this.act, buildNextTaskIndex(mrng.clone())),
                undefined);
        }
        canShrinkWithoutContext(value) {
            return false;
        }
        shrink(_value, _context) {
            return Stream_1.Stream.nil();
        }
    }
    SchedulerArbitrary$1.SchedulerArbitrary = SchedulerArbitrary;

    Object.defineProperty(scheduler$1, "__esModule", {value: true});
    scheduler$1.schedulerFor = scheduler$1.scheduler = void 0;
    const BuildSchedulerFor_1 = BuildSchedulerFor;
    const SchedulerArbitrary_1 = SchedulerArbitrary$1;
    function scheduler(constraints) {
        const {act = (f) => f()} = constraints || {};
        return new SchedulerArbitrary_1.SchedulerArbitrary(act);
    }
    scheduler$1.scheduler = scheduler;
    function schedulerFor(customOrderingOrConstraints, constraintsOrUndefined) {
        const {act = (f) => f()} = Array.isArray(customOrderingOrConstraints)
            ? constraintsOrUndefined || {}
            : customOrderingOrConstraints || {};
        if (Array.isArray(customOrderingOrConstraints)) {
            return (0, BuildSchedulerFor_1.buildSchedulerFor)(act, customOrderingOrConstraints);
        }
        return function(_strs, ...ordering) {
            return (0, BuildSchedulerFor_1.buildSchedulerFor)(act, ordering);
        };
    }
    scheduler$1.schedulerFor = schedulerFor;

    var bigInt64Array$1 = {};

    Object.defineProperty(bigInt64Array$1, "__esModule", {value: true});
    bigInt64Array$1.bigInt64Array = void 0;
    const bigInt_1$1 = bigInt$1;
    const TypedIntArrayArbitraryBuilder_1$1 = TypedIntArrayArbitraryBuilder;
    function bigInt64Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1$1.typedIntArrayArbitraryArbitraryBuilder)(
            constraints,
            BigInt('-9223372036854775808'),
            BigInt('9223372036854775807'),
            BigInt64Array,
            bigInt_1$1.bigInt);
    }
    bigInt64Array$1.bigInt64Array = bigInt64Array;

    var bigUint64Array$1 = {};

    Object.defineProperty(bigUint64Array$1, "__esModule", {value: true});
    bigUint64Array$1.bigUint64Array = void 0;
    const bigInt_1 = bigInt$1;
    const TypedIntArrayArbitraryBuilder_1 = TypedIntArrayArbitraryBuilder;
    function bigUint64Array(constraints = {}) {
        return (0, TypedIntArrayArbitraryBuilder_1.typedIntArrayArbitraryArbitraryBuilder)(
            constraints,
            BigInt(0),
            BigInt('18446744073709551615'),
            BigUint64Array,
            bigInt_1.bigInt);
    }
    bigUint64Array$1.bigUint64Array = bigUint64Array;

    (function(exports) {
        Object.defineProperty(exports, "__esModule", {value: true});
        exports.sparseArray = exports.array = exports.subarray = exports.shuffledSubarray =
            exports.clone = exports.oneof = exports.option = exports.mapToConstant =
                exports.constantFrom = exports.constant = exports.lorem = exports.base64String =
                    exports.hexaString = exports.fullUnicodeString = exports.unicodeString =
                        exports.stringOf = exports.string16bits = exports.asciiString =
                            exports.string = exports.mixedCase = exports.base64 = exports.hexa =
                                exports.fullUnicode = exports.unicode = exports.char16bits =
                                    exports.ascii = exports.char = exports.bigUint = exports
                                                                                         .bigInt =
                                        exports.bigUintN = exports.bigIntN = exports.maxSafeNat =
                                            exports.maxSafeInteger = exports.nat = exports.integer =
                                                exports.double = exports.float = exports.falsy =
                                                    exports.boolean = exports.asyncProperty =
                                                        exports.property =
                                                            exports.PreconditionFailure = exports
                                                                                              .pre =
                                                                exports.assert = exports.check =
                                                                    exports.statistics =
                                                                        exports.sample =
                                                                            exports.__commitHash =
                                                                                exports.__version =
                                                                                    exports.__type =
                                                                                        void 0;
        exports.cloneMethod = exports.Value = exports.Arbitrary = exports.schedulerFor =
            exports.scheduler = exports.commands = exports.scheduledModelRun = exports.modelRun =
                exports
                    .asyncModelRun = exports
                                         .bigUint64Array = exports
                                                               .bigInt64Array = exports
                                                                                    .float64Array =
                    exports.float32Array = exports
                                               .uint32Array = exports
                                                                  .int32Array = exports
                                                                                    .uint16Array =
                        exports.int16Array = exports.uint8ClampedArray = exports.uint8Array =
                            exports.int8Array = exports.uuidV = exports.uuid = exports
                                                                                   .emailAddress =
                                exports.webUrl = exports.webQueryParameters = exports.webFragments =
                                    exports.webSegment = exports.webAuthority = exports.domain =
                                        exports.ipV6 = exports
                                                           .ipV4Extended = exports
                                                                               .ipV4 = exports
                                                                                           .date =
                                            exports.context = exports.func = exports.compareFunc =
                                                exports.compareBooleanFunc = exports.memo =
                                                    exports.letrec = exports.unicodeJsonValue =
                                                        exports.unicodeJson = exports.jsonValue =
                                                            exports.json = exports.object =
                                                                exports.anything = exports
                                                                                       .dictionary =
                                                                    exports.record = exports.tuple =
                                                                        exports.uniqueArray =
                                                                            exports.infiniteStream =
                                                                                void 0;
        exports.createDepthIdentifier = exports.stream = exports.Stream = exports.Random =
            exports.ExecutionStatus = exports.resetConfigureGlobal = exports.readConfigureGlobal =
                exports.configureGlobal = exports.VerbosityLevel = exports.hash =
                    exports.asyncDefaultReportMessage = exports.defaultReportMessage =
                        exports.asyncStringify = exports.stringify = exports.getDepthContextFor =
                            exports.hasAsyncToStringMethod = exports.asyncToStringMethod =
                                exports.hasToStringMethod = exports.toStringMethod =
                                    exports.hasCloneMethod = exports.cloneIfNeeded = void 0;
        const Pre_1 = Pre;
        Object.defineProperty(exports, "pre", {
            enumerable: true,
            get: function() {
                return Pre_1.pre;
            }
        });
        const AsyncProperty_1 = AsyncProperty$1;
        Object.defineProperty(exports, "asyncProperty", {
            enumerable: true,
            get: function() {
                return AsyncProperty_1.asyncProperty;
            }
        });
        const Property_1 = Property$1;
        Object.defineProperty(exports, "property", {
            enumerable: true,
            get: function() {
                return Property_1.property;
            }
        });
        const Runner_1 = Runner;
        Object.defineProperty(exports, "assert", {
            enumerable: true,
            get: function() {
                return Runner_1.assert;
            }
        });
        Object.defineProperty(exports, "check", {
            enumerable: true,
            get: function() {
                return Runner_1.check;
            }
        });
        const Sampler_1 = Sampler;
        Object.defineProperty(exports, "sample", {
            enumerable: true,
            get: function() {
                return Sampler_1.sample;
            }
        });
        Object.defineProperty(exports, "statistics", {
            enumerable: true,
            get: function() {
                return Sampler_1.statistics;
            }
        });
        const array_1 = array$1;
        Object.defineProperty(exports, "array", {
            enumerable: true,
            get: function() {
                return array_1.array;
            }
        });
        const bigInt_1 = bigInt$1;
        Object.defineProperty(exports, "bigInt", {
            enumerable: true,
            get: function() {
                return bigInt_1.bigInt;
            }
        });
        const bigIntN_1 = bigIntN$1;
        Object.defineProperty(exports, "bigIntN", {
            enumerable: true,
            get: function() {
                return bigIntN_1.bigIntN;
            }
        });
        const bigUint_1 = bigUint$1;
        Object.defineProperty(exports, "bigUint", {
            enumerable: true,
            get: function() {
                return bigUint_1.bigUint;
            }
        });
        const bigUintN_1 = bigUintN$1;
        Object.defineProperty(exports, "bigUintN", {
            enumerable: true,
            get: function() {
                return bigUintN_1.bigUintN;
            }
        });
        const boolean_1 = boolean$1;
        Object.defineProperty(exports, "boolean", {
            enumerable: true,
            get: function() {
                return boolean_1.boolean;
            }
        });
        const falsy_1 = falsy$1;
        Object.defineProperty(exports, "falsy", {
            enumerable: true,
            get: function() {
                return falsy_1.falsy;
            }
        });
        const ascii_1 = ascii$1;
        Object.defineProperty(exports, "ascii", {
            enumerable: true,
            get: function() {
                return ascii_1.ascii;
            }
        });
        const base64_1 = base64$1;
        Object.defineProperty(exports, "base64", {
            enumerable: true,
            get: function() {
                return base64_1.base64;
            }
        });
        const char_1 = char$1;
        Object.defineProperty(exports, "char", {
            enumerable: true,
            get: function() {
                return char_1.char;
            }
        });
        const char16bits_1 = char16bits$1;
        Object.defineProperty(exports, "char16bits", {
            enumerable: true,
            get: function() {
                return char16bits_1.char16bits;
            }
        });
        const fullUnicode_1 = fullUnicode$1;
        Object.defineProperty(exports, "fullUnicode", {
            enumerable: true,
            get: function() {
                return fullUnicode_1.fullUnicode;
            }
        });
        const hexa_1 = hexa$1;
        Object.defineProperty(exports, "hexa", {
            enumerable: true,
            get: function() {
                return hexa_1.hexa;
            }
        });
        const unicode_1 = unicode$1;
        Object.defineProperty(exports, "unicode", {
            enumerable: true,
            get: function() {
                return unicode_1.unicode;
            }
        });
        const constant_1 = constant$1;
        Object.defineProperty(exports, "constant", {
            enumerable: true,
            get: function() {
                return constant_1.constant;
            }
        });
        const constantFrom_1 = constantFrom$1;
        Object.defineProperty(exports, "constantFrom", {
            enumerable: true,
            get: function() {
                return constantFrom_1.constantFrom;
            }
        });
        const context_1 = context$1;
        Object.defineProperty(exports, "context", {
            enumerable: true,
            get: function() {
                return context_1.context;
            }
        });
        const date_1 = date$1;
        Object.defineProperty(exports, "date", {
            enumerable: true,
            get: function() {
                return date_1.date;
            }
        });
        const clone_1 = clone$1;
        Object.defineProperty(exports, "clone", {
            enumerable: true,
            get: function() {
                return clone_1.clone;
            }
        });
        const dictionary_1 = dictionary$1;
        Object.defineProperty(exports, "dictionary", {
            enumerable: true,
            get: function() {
                return dictionary_1.dictionary;
            }
        });
        const emailAddress_1 = emailAddress$1;
        Object.defineProperty(exports, "emailAddress", {
            enumerable: true,
            get: function() {
                return emailAddress_1.emailAddress;
            }
        });
        const double_1 = double$1;
        Object.defineProperty(exports, "double", {
            enumerable: true,
            get: function() {
                return double_1.double;
            }
        });
        const float_1 = float$1;
        Object.defineProperty(exports, "float", {
            enumerable: true,
            get: function() {
                return float_1.float;
            }
        });
        const compareBooleanFunc_1 = compareBooleanFunc$1;
        Object.defineProperty(exports, "compareBooleanFunc", {
            enumerable: true,
            get: function() {
                return compareBooleanFunc_1.compareBooleanFunc;
            }
        });
        const compareFunc_1 = compareFunc$1;
        Object.defineProperty(exports, "compareFunc", {
            enumerable: true,
            get: function() {
                return compareFunc_1.compareFunc;
            }
        });
        const func_1 = func$1;
        Object.defineProperty(exports, "func", {
            enumerable: true,
            get: function() {
                return func_1.func;
            }
        });
        const domain_1 = domain$1;
        Object.defineProperty(exports, "domain", {
            enumerable: true,
            get: function() {
                return domain_1.domain;
            }
        });
        const integer_1 = integer$1;
        Object.defineProperty(exports, "integer", {
            enumerable: true,
            get: function() {
                return integer_1.integer;
            }
        });
        const maxSafeInteger_1 = maxSafeInteger$1;
        Object.defineProperty(exports, "maxSafeInteger", {
            enumerable: true,
            get: function() {
                return maxSafeInteger_1.maxSafeInteger;
            }
        });
        const maxSafeNat_1 = maxSafeNat$1;
        Object.defineProperty(exports, "maxSafeNat", {
            enumerable: true,
            get: function() {
                return maxSafeNat_1.maxSafeNat;
            }
        });
        const nat_1 = nat$1;
        Object.defineProperty(exports, "nat", {
            enumerable: true,
            get: function() {
                return nat_1.nat;
            }
        });
        const ipV4_1 = ipV4$1;
        Object.defineProperty(exports, "ipV4", {
            enumerable: true,
            get: function() {
                return ipV4_1.ipV4;
            }
        });
        const ipV4Extended_1 = ipV4Extended$1;
        Object.defineProperty(exports, "ipV4Extended", {
            enumerable: true,
            get: function() {
                return ipV4Extended_1.ipV4Extended;
            }
        });
        const ipV6_1 = ipV6$1;
        Object.defineProperty(exports, "ipV6", {
            enumerable: true,
            get: function() {
                return ipV6_1.ipV6;
            }
        });
        const letrec_1 = letrec$1;
        Object.defineProperty(exports, "letrec", {
            enumerable: true,
            get: function() {
                return letrec_1.letrec;
            }
        });
        const lorem_1 = lorem$1;
        Object.defineProperty(exports, "lorem", {
            enumerable: true,
            get: function() {
                return lorem_1.lorem;
            }
        });
        const mapToConstant_1 = mapToConstant$1;
        Object.defineProperty(exports, "mapToConstant", {
            enumerable: true,
            get: function() {
                return mapToConstant_1.mapToConstant;
            }
        });
        const memo_1 = memo$1;
        Object.defineProperty(exports, "memo", {
            enumerable: true,
            get: function() {
                return memo_1.memo;
            }
        });
        const mixedCase_1 = mixedCase$1;
        Object.defineProperty(exports, "mixedCase", {
            enumerable: true,
            get: function() {
                return mixedCase_1.mixedCase;
            }
        });
        const object_1 = object$1;
        Object.defineProperty(exports, "object", {
            enumerable: true,
            get: function() {
                return object_1.object;
            }
        });
        const json_1 = json$1;
        Object.defineProperty(exports, "json", {
            enumerable: true,
            get: function() {
                return json_1.json;
            }
        });
        const anything_1 = anything$1;
        Object.defineProperty(exports, "anything", {
            enumerable: true,
            get: function() {
                return anything_1.anything;
            }
        });
        const unicodeJsonValue_1 = unicodeJsonValue$1;
        Object.defineProperty(exports, "unicodeJsonValue", {
            enumerable: true,
            get: function() {
                return unicodeJsonValue_1.unicodeJsonValue;
            }
        });
        const jsonValue_1 = jsonValue$1;
        Object.defineProperty(exports, "jsonValue", {
            enumerable: true,
            get: function() {
                return jsonValue_1.jsonValue;
            }
        });
        const unicodeJson_1 = unicodeJson$1;
        Object.defineProperty(exports, "unicodeJson", {
            enumerable: true,
            get: function() {
                return unicodeJson_1.unicodeJson;
            }
        });
        const oneof_1 = oneof$1;
        Object.defineProperty(exports, "oneof", {
            enumerable: true,
            get: function() {
                return oneof_1.oneof;
            }
        });
        const option_1 = option$1;
        Object.defineProperty(exports, "option", {
            enumerable: true,
            get: function() {
                return option_1.option;
            }
        });
        const record_1 = record$1;
        Object.defineProperty(exports, "record", {
            enumerable: true,
            get: function() {
                return record_1.record;
            }
        });
        const uniqueArray_1 = uniqueArray$1;
        Object.defineProperty(exports, "uniqueArray", {
            enumerable: true,
            get: function() {
                return uniqueArray_1.uniqueArray;
            }
        });
        const infiniteStream_1 = infiniteStream$1;
        Object.defineProperty(exports, "infiniteStream", {
            enumerable: true,
            get: function() {
                return infiniteStream_1.infiniteStream;
            }
        });
        const asciiString_1 = asciiString$1;
        Object.defineProperty(exports, "asciiString", {
            enumerable: true,
            get: function() {
                return asciiString_1.asciiString;
            }
        });
        const base64String_1 = base64String$1;
        Object.defineProperty(exports, "base64String", {
            enumerable: true,
            get: function() {
                return base64String_1.base64String;
            }
        });
        const fullUnicodeString_1 = fullUnicodeString$1;
        Object.defineProperty(exports, "fullUnicodeString", {
            enumerable: true,
            get: function() {
                return fullUnicodeString_1.fullUnicodeString;
            }
        });
        const hexaString_1 = hexaString$1;
        Object.defineProperty(exports, "hexaString", {
            enumerable: true,
            get: function() {
                return hexaString_1.hexaString;
            }
        });
        const string_1 = string$1;
        Object.defineProperty(exports, "string", {
            enumerable: true,
            get: function() {
                return string_1.string;
            }
        });
        const string16bits_1 = string16bits$1;
        Object.defineProperty(exports, "string16bits", {
            enumerable: true,
            get: function() {
                return string16bits_1.string16bits;
            }
        });
        const stringOf_1 = stringOf$1;
        Object.defineProperty(exports, "stringOf", {
            enumerable: true,
            get: function() {
                return stringOf_1.stringOf;
            }
        });
        const unicodeString_1 = unicodeString$1;
        Object.defineProperty(exports, "unicodeString", {
            enumerable: true,
            get: function() {
                return unicodeString_1.unicodeString;
            }
        });
        const subarray_1 = subarray$1;
        Object.defineProperty(exports, "subarray", {
            enumerable: true,
            get: function() {
                return subarray_1.subarray;
            }
        });
        const shuffledSubarray_1 = shuffledSubarray$1;
        Object.defineProperty(exports, "shuffledSubarray", {
            enumerable: true,
            get: function() {
                return shuffledSubarray_1.shuffledSubarray;
            }
        });
        const tuple_1 = tuple$1;
        Object.defineProperty(exports, "tuple", {
            enumerable: true,
            get: function() {
                return tuple_1.tuple;
            }
        });
        const uuid_1 = uuid$1;
        Object.defineProperty(exports, "uuid", {
            enumerable: true,
            get: function() {
                return uuid_1.uuid;
            }
        });
        const uuidV_1 = uuidV$1;
        Object.defineProperty(exports, "uuidV", {
            enumerable: true,
            get: function() {
                return uuidV_1.uuidV;
            }
        });
        const webAuthority_1 = webAuthority$1;
        Object.defineProperty(exports, "webAuthority", {
            enumerable: true,
            get: function() {
                return webAuthority_1.webAuthority;
            }
        });
        const webFragments_1 = webFragments$1;
        Object.defineProperty(exports, "webFragments", {
            enumerable: true,
            get: function() {
                return webFragments_1.webFragments;
            }
        });
        const webQueryParameters_1 = webQueryParameters$1;
        Object.defineProperty(exports, "webQueryParameters", {
            enumerable: true,
            get: function() {
                return webQueryParameters_1.webQueryParameters;
            }
        });
        const webSegment_1 = webSegment$1;
        Object.defineProperty(exports, "webSegment", {
            enumerable: true,
            get: function() {
                return webSegment_1.webSegment;
            }
        });
        const webUrl_1 = webUrl$1;
        Object.defineProperty(exports, "webUrl", {
            enumerable: true,
            get: function() {
                return webUrl_1.webUrl;
            }
        });
        const commands_1 = commands$1;
        Object.defineProperty(exports, "commands", {
            enumerable: true,
            get: function() {
                return commands_1.commands;
            }
        });
        const ModelRunner_1 = ModelRunner;
        Object.defineProperty(exports, "asyncModelRun", {
            enumerable: true,
            get: function() {
                return ModelRunner_1.asyncModelRun;
            }
        });
        Object.defineProperty(exports, "modelRun", {
            enumerable: true,
            get: function() {
                return ModelRunner_1.modelRun;
            }
        });
        Object.defineProperty(exports, "scheduledModelRun", {
            enumerable: true,
            get: function() {
                return ModelRunner_1.scheduledModelRun;
            }
        });
        const Random_1 = Random$1;
        Object.defineProperty(exports, "Random", {
            enumerable: true,
            get: function() {
                return Random_1.Random;
            }
        });
        const GlobalParameters_1 = GlobalParameters;
        Object.defineProperty(exports, "configureGlobal", {
            enumerable: true,
            get: function() {
                return GlobalParameters_1.configureGlobal;
            }
        });
        Object.defineProperty(exports, "readConfigureGlobal", {
            enumerable: true,
            get: function() {
                return GlobalParameters_1.readConfigureGlobal;
            }
        });
        Object.defineProperty(exports, "resetConfigureGlobal", {
            enumerable: true,
            get: function() {
                return GlobalParameters_1.resetConfigureGlobal;
            }
        });
        const VerbosityLevel_1 = VerbosityLevel;
        Object.defineProperty(exports, "VerbosityLevel", {
            enumerable: true,
            get: function() {
                return VerbosityLevel_1.VerbosityLevel;
            }
        });
        const ExecutionStatus_1 = ExecutionStatus;
        Object.defineProperty(exports, "ExecutionStatus", {
            enumerable: true,
            get: function() {
                return ExecutionStatus_1.ExecutionStatus;
            }
        });
        const symbols_1 = symbols;
        Object.defineProperty(exports, "cloneMethod", {
            enumerable: true,
            get: function() {
                return symbols_1.cloneMethod;
            }
        });
        Object.defineProperty(exports, "cloneIfNeeded", {
            enumerable: true,
            get: function() {
                return symbols_1.cloneIfNeeded;
            }
        });
        Object.defineProperty(exports, "hasCloneMethod", {
            enumerable: true,
            get: function() {
                return symbols_1.hasCloneMethod;
            }
        });
        const Stream_1 = Stream$1;
        Object.defineProperty(exports, "Stream", {
            enumerable: true,
            get: function() {
                return Stream_1.Stream;
            }
        });
        Object.defineProperty(exports, "stream", {
            enumerable: true,
            get: function() {
                return Stream_1.stream;
            }
        });
        const hash_1 = hash$1;
        Object.defineProperty(exports, "hash", {
            enumerable: true,
            get: function() {
                return hash_1.hash;
            }
        });
        const stringify_1 = stringify;
        Object.defineProperty(exports, "stringify", {
            enumerable: true,
            get: function() {
                return stringify_1.stringify;
            }
        });
        Object.defineProperty(exports, "asyncStringify", {
            enumerable: true,
            get: function() {
                return stringify_1.asyncStringify;
            }
        });
        Object.defineProperty(exports, "toStringMethod", {
            enumerable: true,
            get: function() {
                return stringify_1.toStringMethod;
            }
        });
        Object.defineProperty(exports, "hasToStringMethod", {
            enumerable: true,
            get: function() {
                return stringify_1.hasToStringMethod;
            }
        });
        Object.defineProperty(exports, "asyncToStringMethod", {
            enumerable: true,
            get: function() {
                return stringify_1.asyncToStringMethod;
            }
        });
        Object.defineProperty(exports, "hasAsyncToStringMethod", {
            enumerable: true,
            get: function() {
                return stringify_1.hasAsyncToStringMethod;
            }
        });
        const scheduler_1 = scheduler$1;
        Object.defineProperty(exports, "scheduler", {
            enumerable: true,
            get: function() {
                return scheduler_1.scheduler;
            }
        });
        Object.defineProperty(exports, "schedulerFor", {
            enumerable: true,
            get: function() {
                return scheduler_1.schedulerFor;
            }
        });
        const RunDetailsFormatter_1 = RunDetailsFormatter;
        Object.defineProperty(exports, "defaultReportMessage", {
            enumerable: true,
            get: function() {
                return RunDetailsFormatter_1.defaultReportMessage;
            }
        });
        Object.defineProperty(exports, "asyncDefaultReportMessage", {
            enumerable: true,
            get: function() {
                return RunDetailsFormatter_1.asyncDefaultReportMessage;
            }
        });
        const PreconditionFailure_1 = PreconditionFailure$1;
        Object.defineProperty(exports, "PreconditionFailure", {
            enumerable: true,
            get: function() {
                return PreconditionFailure_1.PreconditionFailure;
            }
        });
        const int8Array_1 = int8Array$1;
        Object.defineProperty(exports, "int8Array", {
            enumerable: true,
            get: function() {
                return int8Array_1.int8Array;
            }
        });
        const int16Array_1 = int16Array$1;
        Object.defineProperty(exports, "int16Array", {
            enumerable: true,
            get: function() {
                return int16Array_1.int16Array;
            }
        });
        const int32Array_1 = int32Array$1;
        Object.defineProperty(exports, "int32Array", {
            enumerable: true,
            get: function() {
                return int32Array_1.int32Array;
            }
        });
        const uint8Array_1 = uint8Array$1;
        Object.defineProperty(exports, "uint8Array", {
            enumerable: true,
            get: function() {
                return uint8Array_1.uint8Array;
            }
        });
        const uint8ClampedArray_1 = uint8ClampedArray$1;
        Object.defineProperty(exports, "uint8ClampedArray", {
            enumerable: true,
            get: function() {
                return uint8ClampedArray_1.uint8ClampedArray;
            }
        });
        const uint16Array_1 = uint16Array$1;
        Object.defineProperty(exports, "uint16Array", {
            enumerable: true,
            get: function() {
                return uint16Array_1.uint16Array;
            }
        });
        const uint32Array_1 = uint32Array$1;
        Object.defineProperty(exports, "uint32Array", {
            enumerable: true,
            get: function() {
                return uint32Array_1.uint32Array;
            }
        });
        const float32Array_1 = float32Array$1;
        Object.defineProperty(exports, "float32Array", {
            enumerable: true,
            get: function() {
                return float32Array_1.float32Array;
            }
        });
        const float64Array_1 = float64Array$1;
        Object.defineProperty(exports, "float64Array", {
            enumerable: true,
            get: function() {
                return float64Array_1.float64Array;
            }
        });
        const sparseArray_1 = sparseArray$1;
        Object.defineProperty(exports, "sparseArray", {
            enumerable: true,
            get: function() {
                return sparseArray_1.sparseArray;
            }
        });
        const Arbitrary_1 = Arbitrary$1;
        Object.defineProperty(exports, "Arbitrary", {
            enumerable: true,
            get: function() {
                return Arbitrary_1.Arbitrary;
            }
        });
        const Value_1 = Value$1;
        Object.defineProperty(exports, "Value", {
            enumerable: true,
            get: function() {
                return Value_1.Value;
            }
        });
        const DepthContext_1 = DepthContext;
        Object.defineProperty(exports, "createDepthIdentifier", {
            enumerable: true,
            get: function() {
                return DepthContext_1.createDepthIdentifier;
            }
        });
        Object.defineProperty(exports, "getDepthContextFor", {
            enumerable: true,
            get: function() {
                return DepthContext_1.getDepthContextFor;
            }
        });
        const bigInt64Array_1 = bigInt64Array$1;
        Object.defineProperty(exports, "bigInt64Array", {
            enumerable: true,
            get: function() {
                return bigInt64Array_1.bigInt64Array;
            }
        });
        const bigUint64Array_1 = bigUint64Array$1;
        Object.defineProperty(exports, "bigUint64Array", {
            enumerable: true,
            get: function() {
                return bigUint64Array_1.bigUint64Array;
            }
        });
        const __type = 'commonjs';
        exports.__type = __type;
        const __version = '3.1.0';
        exports.__version = __version;
        const __commitHash = '3749924de8932f7cb3ddf945ce499835bb0513ac';
        exports.__commitHash = __commitHash;
    }(fastCheckDefault$1));

    var fastCheckDefault = /*@__PURE__*/ getDefaultExportFromCjs(fastCheckDefault$1);

    return fastCheckDefault;
})();
