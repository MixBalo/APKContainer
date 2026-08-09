/*
 * opensl_bridge.c — companion TU for opensl_bridge.mm
 *
 * Status: REAL (common path). All implementation lives in opensl_bridge.mm
 *         (Objective-C++ / ARC). This .c file is intentionally empty so the
 *         build picks up the .mm version without duplicate-symbol conflicts.
 *
 * Why a separate .c file exists at all:
 *   - Historical: the original skeleton was pure C. Phase 3 (Task P3-2)
 *     rewrote the implementation in Objective-C++ so it can use AVAudioEngine.
 *   - XcodeGen's `sources: [path: Native]` glob compiles BOTH .c and .mm
 *     in the same target. Keeping the .c as an empty TU means we don't have
 *     to touch project.yml (no excludes); the .mm is the sole source of the
 *     opensl_bridge_* symbols.
 *
 * If you remove this file, no functionality is lost — the build will still
 * work. It's kept only to minimize the diff vs. the Phase 2 skeleton.
 *
 * See opensl_bridge.mm for the real implementation, opensl_bridge.h for the
 * C ABI, and docs/ARCHITECTURE.md §5 for design notes.
 */

/* Intentionally no symbols. See header comment. */
