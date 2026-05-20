/**
 * Tests resuming a single primary-driven index build multiple times in a row -- once in a
 * randomly-chosen position (beginning, middle, or end) of each phase (scan, load, drain). The
 * chosen positions are logged so that any failure can be reproduced via `--shellSeed`.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

Random.setRandomSeed();

const allPositions = Object.values(PdibPosition);
const pickPosition = () => allPositions[Random.randInt(allPositions.length)];

const positions = {
    [PdibPhase.SCAN]: pickPosition(),
    [PdibPhase.LOAD]: pickPosition(),
    [PdibPhase.DRAIN]: pickPosition(),
};
jsTest.log.info("Randomly-chosen positions for runMultiPhase", {positions});

const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
PrimaryDrivenResumableIndexBuildTest.runMultiPhase(rst, {positions});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
