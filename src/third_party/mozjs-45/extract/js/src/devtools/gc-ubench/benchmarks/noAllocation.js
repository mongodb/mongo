window.tests.set('noAllocation', {
    description: "Do not generate any garbage.",
    load: (N) => {},
    unload: () => {},
    makeGarbage: (N) => {}
});
