const {now: internalNow} = internalModule("performance");

export const performance = Object.freeze({
    now() {
        return internalNow();
    },
});
