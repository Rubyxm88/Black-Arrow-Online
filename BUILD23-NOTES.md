# Build 23 notes (scale-swap branch)

Status 2026-10-23: the code is tested, but nothing is packaged or pushed. Remove this file before release.

## What is on this branch (on top of build 22, next-22 e6417d380a)

- **47ff4a7564:** switching resolution scale in a match keeps shaders ready.
  - When the game needs a shader that isn't loaded, it reads `<cache>/<hash>` from disk before compiling (`shader_fill_from_disk`). This also fixes the build 22 problem where shaders on disk went unused unless they were in `shader_cache_list`.
  - New setting `perf.shader_precache_scales` is a bitmask where bit 0 = 1x. The default 6 means 2x and 3x. The scale in use is added and saved.
  - The precache builds every scale in the list, the one in use first. If the player switches to an unbuilt scale, it moves that scale to the front.
  - The shader cache window re-runs the auto precache when the scale changes and lists the scales.
- **14887b917f:** any scale other than the one the session started at builds at a third of the speed. After each shader it sleeps twice the build time, at most 200 ms.

## Test results (shader-coverage/sstest, Agora)

| Case | Result |
| --- | --- |
| 2x to 3x mid-match, 40 places | 0 new shaders, all loaded from disk |
| Unbuilt 4x, full-speed build | fell from 11 to 2 fps for about 3 min |
| Unbuilt 5x, paced build | 6.7 fps while building, 9.8 when done (about 9 min) |
| Back to 2x, 30 places | 30 fps, 0 new shaders |

- Disk use is about 127 MB per scale, so the default 2x + 3x is about 255 MB.
- Test exe build: `tools/x138/docker-build-ss-cover.sh`.
  - It applies `cover-hooks.patch` and `scale-hook.patch` (XEMU_TEST_SCALE_FILE). Both are test only and never go in a release.
- Test script: `shader-coverage/sstest.py` (boot, walk N, swap N). Source `sstest/env.sh` first.
  - If the Create Match "Next" button isn't detected, press down once and run with SS_ON_NEXT=1.
  - Start the console from a foreground command. A background command shuts it down when it ends.

## Shader list for build 23

- `shader-coverage/candidate-v41.bin` has 3260 states, including the shipped 1949. It ships as `black-arrow-online/package/shader-list.bin`.
- The precache check passed with 3251 unique shaders and 0 failed at each scale: 3x in 13 s, 2x in 76 s, 1x in 41 s.

## Open decisions

1. **xemu v0.8.139** (aa54c34cae, vsh register carryover): merge or skip.
   - It merges cleanly (`git merge-tree` exit 0).
   - It needs a new hash in `.github/shader-state.txt`, or the release workflow fails.
   - If merged, run a short coverage pass on 1-2 maps afterwards.
2. **Coverage not done yet:**
   - Step D grenade/effects passes on about 30 maps already scanned. Only KillhouseFixed is done.
   - The Fixed variants: Kes, Prison, WinterLodge.
   - Smoke grenades.
   - Other players' bodies and flashes, which needs a second player.

## Next steps

1. Decide on v0.8.139.
2. Package build 23 (Windows and Mac) with the new shader list.
3. Check for xemu releases again before the push.
4. The user runs the push.
