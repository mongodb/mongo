// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

//! Pre-initializes a WASM component with Wizer.
//!
//! Like `wasmtime wizer -W exceptions=y -S cli=y <in> -o <out>`, except that it initializes the
//! component by calling the engine's `initialize-engine` export, and that the WASI randomness and
//! clocks initialization can observe are deterministic, so the same input always produces the same
//! snapshot. Anything random the snapshot captures is therefore a per-build constant; the guest
//! must re-seed it after restore.
//!
//! Usage: wizer_driver <input.wasm> <output.wasm>

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use wasmtime::component::{Component, Instance, Linker, ResourceTable, Val};
use wasmtime::{Config, Engine, Result, Store, error::Context as _};
use wasmtime_wasi::random::Deterministic;
use wasmtime_wasi::{
    HostMonotonicClock, HostWallClock, WasiCtx, WasiCtxBuilder, WasiCtxView, WasiView,
};
use wasmtime_wizer::{WasmtimeWizerComponent, Wizer};

// Arbitrary but fixed.
const RANDOM_SEED: u64 = 0x6d6f_6e67_6f64_6200;
const WALL_CLOCK: Duration = Duration::from_secs(1_700_000_000);

/// A fixed, well-mixed byte sequence (splitmix64 output) for WASI randomness to cycle through.
fn deterministic_bytes(seed: u64) -> Vec<u8> {
    let mut state = seed;
    (0..512)
        .flat_map(|_| {
            state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
            let mut z = state;
            z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
            (z ^ (z >> 31)).to_le_bytes()
        })
        .collect()
}

struct FixedWallClock;

impl HostWallClock for FixedWallClock {
    fn resolution(&self) -> Duration {
        Duration::from_nanos(1)
    }
    fn now(&self) -> Duration {
        WALL_CLOCK
    }
}

/// Advances by a fixed step on every read, so code that waits for time to pass still terminates.
#[derive(Default)]
struct SteppingMonotonicClock(AtomicU64);

impl HostMonotonicClock for SteppingMonotonicClock {
    fn resolution(&self) -> u64 {
        1
    }
    fn now(&self) -> u64 {
        const STEP_NANOS: u64 = 1_000;
        self.0.fetch_add(STEP_NANOS, Ordering::Relaxed)
    }
}

struct Host {
    wasi: WasiCtx,
    table: ResourceTable,
}

impl WasiView for Host {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView {
            ctx: &mut self.wasi,
            table: &mut self.table,
        }
    }
}

async fn initialize_engine(store: &mut Store<Host>, instance: &Instance) -> Result<()> {
    const INTERFACE: &str = "mongo:mozjs/mozjs";
    let (_, interface) = instance
        .get_export(&mut *store, None, INTERFACE)
        .with_context(|| format!("component does not export {INTERFACE}"))?;
    let (_, func) = instance
        .get_export(&mut *store, Some(&interface), "initialize-engine")
        .with_context(|| format!("{INTERFACE} does not export initialize-engine"))?;
    let func = instance
        .get_func(&mut *store, func)
        .with_context(|| "initialize-engine is not a function")?;
    let options = Val::Record(vec![
        ("heap-size-mb".to_string(), Val::U32(0)),
        ("javascript-protection".to_string(), Val::Bool(false)),
    ]);
    let mut result = [Val::Bool(false)];
    func.call_async(&mut *store, &[options], &mut result)
        .await?;
    match &result[0] {
        Val::Result(Ok(_)) => Ok(()),
        other => wasmtime::bail!("initialize-engine failed: {other:?}"),
    }
}

async fn run(input: &str, output: &str) -> Result<()> {
    let wasm = std::fs::read(input).with_context(|| format!("failed to read {input}"))?;

    let mut config = Config::new();
    config.wasm_exceptions(true);
    let engine = Engine::new(&config)?;

    // No stdio, environment, arguments, or filesystem; only deterministic clocks and randomness.
    let wasi = WasiCtxBuilder::new()
        .secure_random(Deterministic::new(deterministic_bytes(RANDOM_SEED)))
        .insecure_random(Deterministic::new(deterministic_bytes(RANDOM_SEED + 1)))
        .insecure_random_seed(u128::from(RANDOM_SEED + 2))
        .wall_clock(FixedWallClock)
        .monotonic_clock(SteppingMonotonicClock::default())
        .build();
    let mut store = Store::new(
        &engine,
        Host {
            wasi,
            table: ResourceTable::new(),
        },
    );
    let mut linker = Linker::<Host>::new(&engine);
    wasmtime_wasi::p2::add_to_linker_async(&mut linker)?;

    let wizer = Wizer::new();
    let (cx, instrumented) = wizer.instrument_component(&wasm)?;
    let component = Component::new(&engine, &instrumented)?;
    let instance = linker.instantiate_async(&mut store, &component).await?;
    initialize_engine(&mut store, &instance).await?;
    let snapshot = wizer
        .snapshot_component(
            cx,
            &mut WasmtimeWizerComponent {
                store: &mut store,
                instance,
            },
        )
        .await?;

    std::fs::write(output, snapshot).with_context(|| format!("failed to write {output}"))?;
    Ok(())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        wasmtime::bail!("usage: {} <input.wasm> <output.wasm>", args[0]);
    }
    tokio::runtime::Builder::new_current_thread()
        .build()?
        .block_on(run(&args[1], &args[2]))
}
