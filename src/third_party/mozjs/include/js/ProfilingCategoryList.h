/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef baseprofiler_ProfilingCategoryList_h
#define baseprofiler_ProfilingCategoryList_h

// Profiler sub-categories are applied to each sampled stack to describe the
// type of workload that the CPU is busy with. Only one sub-category can be
// assigned so be mindful that these are non-overlapping. The active category is
// set by pushing a label to the profiling stack, or by the unwinder in cases
// such as JITs. A profile sample in arbitrary C++/Rust will typically be
// categorized based on the top of the label stack.
//
// The list of available color names for categories is:
//    transparent
//    blue
//    green
//    grey
//    lightblue
//    magenta
//    orange
//    purple
//    yellow

// clang-format off

#define MOZ_PROFILING_CATEGORY_LIST(BEGIN_CATEGORY, SUBCATEGORY, END_CATEGORY) \
  BEGIN_CATEGORY(IDLE, "Idle", "transparent") \
    SUBCATEGORY(IDLE, IDLE, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(OTHER, "Other", "grey") \
    SUBCATEGORY(OTHER, OTHER, "Other") \
    SUBCATEGORY(OTHER, OTHER_PreferenceRead, "Preference Read") \
    SUBCATEGORY(OTHER, OTHER_Profiling, "Profiling") \
  END_CATEGORY \
  BEGIN_CATEGORY(TEST, "Test", "darkgray") \
    SUBCATEGORY(TEST, TEST, "Test") \
  END_CATEGORY \
  BEGIN_CATEGORY(LAYOUT, "Layout", "purple") \
    SUBCATEGORY(LAYOUT, LAYOUT, "Other") \
    SUBCATEGORY(LAYOUT, LAYOUT_FrameConstruction, "Frame construction") \
    SUBCATEGORY(LAYOUT, LAYOUT_Reflow, "Reflow") \
    SUBCATEGORY(LAYOUT, LAYOUT_CSSParsing, "CSS parsing") \
    SUBCATEGORY(LAYOUT, LAYOUT_SelectorQuery, "Selector query") \
    SUBCATEGORY(LAYOUT, LAYOUT_StyleComputation, "Style computation") \
  END_CATEGORY \
  BEGIN_CATEGORY(JS, "JavaScript", "yellow") \
    SUBCATEGORY(JS, JS, "Other") \
    SUBCATEGORY(JS, JS_Parsing, "Parsing") \
    SUBCATEGORY(JS, JS_BaselineCompilation, "JIT Compile (baseline)") \
    SUBCATEGORY(JS, JS_IonCompilation, "JIT Compile (ion)") \
    SUBCATEGORY(JS, JS_Interpreter, "Interpreter") \
    SUBCATEGORY(JS, JS_BaselineInterpret, "JIT (baseline-interpreter)") \
    SUBCATEGORY(JS, JS_Baseline, "JIT (baseline)") \
    SUBCATEGORY(JS, JS_IonMonkey, "JIT (ion)") \
  END_CATEGORY \
  BEGIN_CATEGORY(GCCC, "GC / CC", "orange") \
    SUBCATEGORY(GCCC, GCCC, "Other") \
    SUBCATEGORY(GCCC, GCCC_MinorGC, "Minor GC") \
    SUBCATEGORY(GCCC, GCCC_MajorGC, "Major GC (Other)") \
    SUBCATEGORY(GCCC, GCCC_MajorGC_Mark, "Major GC (Mark)") \
    SUBCATEGORY(GCCC, GCCC_MajorGC_Sweep, "Major GC (Sweep)") \
    SUBCATEGORY(GCCC, GCCC_MajorGC_Compact, "Major GC (Compact)") \
    SUBCATEGORY(GCCC, GCCC_UnmarkGray, "Unmark Gray") \
    SUBCATEGORY(GCCC, GCCC_Barrier, "Barrier") \
    SUBCATEGORY(GCCC, GCCC_FreeSnowWhite, "CC (Free Snow White)") \
    SUBCATEGORY(GCCC, GCCC_BuildGraph, "CC (Build Graph)") \
    SUBCATEGORY(GCCC, GCCC_ScanRoots, "CC (Scan Roots)") \
    SUBCATEGORY(GCCC, GCCC_CollectWhite, "CC (Collect White)") \
    SUBCATEGORY(GCCC, GCCC_Finalize, "CC (Finalize)") \
  END_CATEGORY \
  BEGIN_CATEGORY(NETWORK, "Network", "lightblue") \
    SUBCATEGORY(NETWORK, NETWORK, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(GRAPHICS, "Graphics", "green") \
    SUBCATEGORY(GRAPHICS, GRAPHICS, "Other") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_DisplayListBuilding, "DisplayList building") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_DisplayListMerging, "DisplayList merging") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_LayerBuilding, "Layer building") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_TileAllocation, "Tile allocation") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_WRDisplayList, "WebRender display list") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_Rasterization, "Rasterization") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_FlushingAsyncPaints, "Flushing async paints") \
    SUBCATEGORY(GRAPHICS, GRAPHICS_ImageDecoding, "Image decoding") \
  END_CATEGORY \
  BEGIN_CATEGORY(DOM, "DOM", "blue") \
    SUBCATEGORY(DOM, DOM, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_ANDROID, "Android", "yellow") \
    SUBCATEGORY(JAVA_ANDROID, JAVA_ANDROID, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_ANDROIDX, "AndroidX", "orange") \
    SUBCATEGORY(JAVA_ANDROIDX, JAVA_ANDROIDX, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_LANGUAGE, "Java", "blue") \
    SUBCATEGORY(JAVA_LANGUAGE, JAVA_LANGUAGE, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_MOZILLA, "Mozilla", "green") \
    SUBCATEGORY(JAVA_MOZILLA, JAVA_MOZILLA, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_KOTLIN, "Kotlin", "purple") \
    SUBCATEGORY(JAVA_KOTLIN, JAVA_KOTLIN, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(JAVA_BLOCKED, "Blocked", "lightblue") \
    SUBCATEGORY(JAVA_BLOCKED, JAVA_BLOCKED, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(IPC, "IPC", "lightgreen") \
    SUBCATEGORY(IPC, IPC, "Other") \
  END_CATEGORY \
  BEGIN_CATEGORY(MEDIA, "Media", "orange") \
    SUBCATEGORY(MEDIA, MEDIA, "Other") \
    SUBCATEGORY(MEDIA, MEDIA_CUBEB, "Cubeb") \
    SUBCATEGORY(MEDIA, MEDIA_PLAYBACK, "Playback") \
    SUBCATEGORY(MEDIA, MEDIA_RT, "Real-time rendering") \
  END_CATEGORY \
  BEGIN_CATEGORY(PROFILER, "Profiler", "lightred") \
    SUBCATEGORY(PROFILER, PROFILER, "Other") \
  END_CATEGORY

// clang-format on

#endif  // baseprofiler_ProfilingCategoryList_h
