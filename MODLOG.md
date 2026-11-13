# CraftNet Development Log

## Minecraft 26.3 assets, inventory, and HUD

- Packed the original 64×32 `humanoid` and `humanoid_leggings` armor layers into a 256×128 atlas. Held and dropped items share the inventory icon atlas, and the UI uses the compiled Minecraft bitmap glyph atlas.
- Cropped the 176×166 player inventory and 176×222 six-row container panels from their original 256×256 PNGs. The hotbar is 182×22 and heart and hunger sprites are 9×9. Item icons occupy 16×16 cells with nearest-neighbor sampling, original orientation, and transparency. Bed icons combine the top faces of the head and foot. The PSP loads the atlases as RGBA4444.
- Imported assets from a local Java Edition client JAR. Stone and grass-side textures are 16×16, while `water_still` is a 16×512 animation strip. Atlas generation extracts one static frame with nearest-neighbor sampling and handles indexed PNG transparency. Source and generated assets are stored under `assets/imported`, which Git ignores.
- Packed the ten 16×16 `destroy_stage_0` through `destroy_stage_9` sprites at Y=448 in the HUD atlas. The renderer overlays them on the faces of the block being mined.
- Verification status. In-game and visual checks were skipped for these changes.

## 2026-11-04 — Block models, controls, and server browser

- Added the CCB3 binary format for block-state models, multipart rules, rotations, UVs, culling masks, shared faces, and collision and selection shapes from local Java Edition data. Two atlas pages held 1,244 textures. The tables occupied 4,015,356 bytes.
- Added alpha testing on PSP so transparent texels do not write depth and hide terrain. Translucent surfaces draw in a separate pass, and face masks use actual texture transparency. Fixed liquid height and player submersion checks. Added water faces for waterlogged models and step-up behavior at shorelines.
- Mapped camera movement to Triangle, Cross, Square, and Circle. At this stage, R handled attacks and mining, L placed blocks, D-pad Down dropped items, Up opened chat, Right jumped or swam up, and Left crouched. Moved chat to the bottom edge and reused the PSP on-screen keyboard for chat, addresses, and player names. Keyboard input consumes gameplay input while active.
- Added a server browser and PSP Wi-Fi dialog at startup. Added Add, Edit, Remove, Refresh, and Player actions, sequential status requests, and `servers.dat` persistence with a backup. Cached server names and descriptions, including MOTDs.
- Added Allman formatting through `.clang-format` and updated the build and packaging scripts for additional atlas pages.
- Verification status. Gameplay and tests were skipped. FPS, memory use, and leaks on physical PSP hardware were unmeasured.

## 2026-11-04 — UV lock, stepping, and lobby transfers

- Fixed UV transforms for cropped faces using `uvlock`. Added step-up for ledges up to 0.6 blocks and auto-jump with clearance checks.
- Kept the connection UI active during server reconfiguration and respawn. Assigned separate geometry revisions to new worlds.
- Added Transfer handling, bounded proxy cookie storage, an empty Known Packs response, and disconnect messages.
- Verification status. Gameplay and tests were skipped.

## 2026-11-04 — Dedicated PSP renderer

- Added a native PSPGL VBO path with a shared index buffer, removing per-frame conversion of separate `DrawMesh` arrays. Spread section builds across frames, baked lighting once per mesh, and grouped faces by material and direction.
- Set the initial PSP geometry cache to 32 sections and 6 MiB, with a 48-block render distance, frustum culling, and back-facing group culling. Used RGBA4444 atlases and set the CPU and bus clocks to 333 and 166 MHz.
- Optimized uniform sections, palette decoding, and local physics. Kept the previous mesh until its replacement completed and released partial buffers when a build was canceled.
- Verification status. Gameplay and tests were skipped. FPS was unmeasured.

## 2026-11-04 — Disappearing chunks

- Expanded section storage to 768 arrays of 16-bit block states within 6 MiB, with up to 2,048 metadata records. Protected nearby columns across their full height.
- Prioritized geometry work by visibility and camera direction. Retried failed builds and kept existing Windows meshes until replacements succeeded.
- Set a 32 MiB Windows geometry budget. The PSP budget remained 6 MiB with a per-section limit of 49,152 faces.
- Verification status. Gameplay and tests were skipped.

## PSP mesh rebuilds when turning the camera

- Stopped removing completed meshes when the camera turned. Allowed more than 32 candidates in the nearby section list and kept the 32 mesh slots independent of view direction. At this stage, builds outside the visible area were canceled.
- Reduced the PSP render distance to 24 blocks, set fog to 16–24 blocks, and set the far camera plane and player culling distance to 24 blocks.
- Limited each build slice to 256 blocks, checking a target budget of 1 ms every 32 blocks. Checked the full mesh budget before the first VBO allocation and rejected distant sections before checking their neighbors. The geometry budget remained 6 MiB and raw world storage capacity stayed the same.
- Verification status. Gameplay and tests were skipped. FPS was unmeasured.

## Spatial cache and one VBO per PSP section

- Removed view-dependent eviction and build cancellation. Prioritized the 96 nearest sections within 12 MiB by distance and queued nearby off-screen sections after visible ones.
- Allocated one VBO per section. Wrote to a cached mapping in slices of up to 512 faces with a 1 ms target. Increased draw groups to 10,922 faces within the 16-bit GE count and reduced the vertex size from 24 to 20 bytes with 16-bit UVs.
- Grouped opaque draws by atlas page, sorted translucent draws by distance, and removed the per-frame mesh slot lookup.
- Skipped rebuilds for unchanged sections. Marked neighbors dirty only when a block change affected a shared boundary. Kept the render distance at 24 blocks.
- Verification status. Gameplay and tests were skipped. Device FPS and memory use were unmeasured.

## Fullscreen PSP Wi-Fi and keyboard dialogs

- Rendered system dialogs with a fullscreen viewport and scissor rectangle, flushed pending PSPGL work, and saved and restored GE state. Displayed a dedicated Wi-Fi modal and opened it after UI asset loading, before the main loop.
- Handled both FINISHED and NONE dialog states, retried `ShutdownStart` while the system was busy, and waited for button release after a dialog closed.
- Added yellow focus indicators, D-pad navigation and repeat, Cross activation, form navigation, Save and Cancel, and a delete confirmation. Preserved Triangle for Add, Square for Edit, Select for Refresh, and Start for Player.
- Verification status. Gameplay and tests were skipped.

## Invisible world on PSP

- Fixed section meshes being discarded when `glUnmapBufferARB` returned `GL_FALSE`. Binding a different PSPGL buffer with `glBindBufferARB` automatically unmapped the previous one, invalidating the pointer saved across frames.
- Staged each slice in a CPU array of up to 40 KiB and uploaded it with `glBufferSubDataARB`. Validated VBO allocation, uploads, and slice bounds before accepting a mesh. Retained one VBO per section, spatial caching, the existing UVs and render distance, and the PSP dialog fixes.
- Verification status. Gameplay and tests were skipped.

## Movement synchronization and custom server worlds

- Added `player_input`, sprint commands, and `horizontalCollision`. Applied server-side `movement_speed` modifiers and preserved teleport velocity through physics resets. Sent the accepted position alongside the teleport confirmation.
- Parsed the `dimension_type` registry with bounded NBT traversal. Accepted custom world names when supported dimension bounds were available.
- Added basic rendering, `custom_name` labels, and interaction for common NPCs. Logged disconnect reasons and malformed packet IDs and sizes in `last_error.txt`.
- Verification status. Gameplay and tests were skipped. Mineblaze compatibility was unverified.

## Player models, profiles, name tags, and skin caching

- Imported local 64×64 Steve and Alex PNGs with transparent outer layers and pixel-aligned body parts. The skeleton rotates each part around pivots relative to the midpoint between the feet. Texture origins are (0, 0) for the head, (16, 16) for the torso, (40, 16) for the right arm, and (0, 16) for the right leg. The left limbs use the lower half of the skin. The slim model uses three-pixel-wide arms.
- Packed 763 unscaled 8×8 bitmap glyphs into a 256×256 RGBA atlas and measured glyph widths from opaque pixels. Kept the imported resources under `assets/imported`.
- Added `player_info`, UUID, profile, and skin associations, scoreboard teams, named and RGB colors, bold text, overhead names, head rotation, and swing events. Animated six textured body parts and their outer layers procedurally.
- Downloaded public skins one at a time with PNG validation and a 32-image cache. Moved PSP DNS lookup to a worker thread and converted legacy 64×32 skins to 64×64. Updated the platform builds, CMake, and packaging.
- Verification status. Gameplay and tests were skipped. Device behavior was unverified.

## Movement corrections and flickering name tags

- Synchronized fixed-step physics with packet output. Each simulation step sends its own `client_tick_end`. The client processes corrections before simulation and acknowledges them without emitting an extra tick or resetting the physics timer phase.
- Replaced binary-search contact resolution with exact AABB bounds. Applied server `set_entity_motion` velocity, aligned PSP digital input and jumps with horizontal axis order, and waited for supporting terrain before marking the player ready.
- Flushed rlgl around the name-tag pass, drew backgrounds, shadows, and letters separately, and reversed the draw order of overlapping labels so nearby glyphs stayed in front.
- Verification status. Gameplay and tests were skipped. Behavior on Mineblaze was unverified.
