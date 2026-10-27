# CLAUDE.md — Segmented Splines handoff

This file is the context handoff from the Mac session that drafted this mod to the Claude
session on the Linux (CachyOS) PC that will compile, test and continue it. Read it fully once.

## Status

- **First build passed (2026-10-26, Linux PC).** `FactoryEditor Linux Development` against engine
  5.6.1-css-83 compiled and linked `libUnrealEditor-SegmentedSplines.so` with no errors or
  warnings on the first pass; the `AccessTransformers.ini` Friend entries took effect. Debug
  symbols show all three hook lambdas and `TruncateIfTooLong` present.
- **Win64 Shipping package passed (2026-10-26).** MSVC 14.38 under wine compiled and linked
  `FactoryGameSteam-SegmentedSplines-Win64-Shipping.dll` with no warnings; the package was
  copied into the Proton game's `FactoryGame/Mods/SegmentedSplines/` next to SML 3.12.0.
  Command-line equivalent of Alpakit Dev (run from the starter root with `UE_WINE_MSVC` exported):
  `RunUAT.sh -ScriptsForProject=<uproject> PackagePlugin -project=<uproject> -clientconfig=Shipping -serverconfig=Shipping -utf8output -DLCName=SegmentedSplines -build -platform=Win64 -nocompileeditor -installed -CopyToGameDirectory_Windows=<game>`
  (~10 min). Satisfactory Mod Manager 3.1.0 is installed and knows the Steam install.
- **In-game on a vanilla server (2026-10-26):** phase 1 trimming builds; a trimmed belt with its
  pole disabled (free end in the air) builds; wall-support to wall-support spans work. Seen once:
  aiming the far end at a bare pillar made vanilla's wall mount and our seated floor pole both
  show up and the hologram went red (cause not diagnosed; the chain's final piece now hands that
  end back to vanilla, which sidesteps it).
- **Phase 2 chain works on a vanilla server (2026-10-27, ninth run):** three chains built
  end to end (14179 cm -> 3 pieces, 8000 cm -> 2, 14593 cm -> 3), each piece arriving in
  ~140 ms, joints seated on a pole where there was floor and left free otherwise, the far end
  placed by vanilla (free or snapped). Found in that log and fixed since: the 8000 cm belt cut
  in half had piece 2 flipped back over piece 1 (`MakeContinuous` no longer reverses, see Phase
  2 section); confirmed fixed in the tenth run. Tenth run on the ground: piece 1 built with its
  pole seated on the floor, but the link to the remote server stalled for ~45 s right then
  (vanilla logged "Hit limit of 96 saved moves", missed acks, server Playing -> Offline ->
  Playing) and the 10 s wait gave up before the belt arrived. The wait is now 60 s with a
  "still waiting" line at 5 s, and the server's rejection RPC ends the chain at once instead.
  Eleventh run (ground again): the whole chain built, piece 1 after 16.7 s, piece 2 in 40 ms.
- **Open: server stalls on ground runs (2026-10-27).** Both ground runs stalled; none of the
  17 air chains did. In both, the client stopped getting move acks within ~1 s of the belt
  START being placed (press -> step 1 AdjustStartingPole, release -> step 2: a fresh floor
  pole; every air run started snapped to something and went straight to step 2), i.e. before
  any construction message. 30-50 s later the server came back and the piece arrived. The mod
  sends nothing during a drag (per-frame hooks are local: RestoreSupports, TooLong removal,
  last-hit capture), so the client log cannot say why; one earlier stall (01:56, first session)
  had no build going at all, and the two onsets are 9 min 39 s apart, so a periodic server
  job (autosave?) is as likely as the ground start. The server's own log at the onset times
  (16:49:48 and 16:59:27 UTC) is the next thing to read; two client-side experiments separate
  the variables: a short vanilla belt on the ground from a fresh pole, and a long ground chain
  started from an existing belt end (no step 1).
- **Chain generalised to pipes/hypertubes and tracks (2026-10-27, not yet run in-game).** The
  user found hypertubes and rails "not working the same way" (they only had phase-1 trimming) and
  a hypertube from a wall hole to a far Y-junction stayed red "too long" (phase 1 stood down on a
  snapped far end). The chain code is now written against a per-type adapter (`FSplineKind`:
  belt, pipe, track) and hooked on all three hologram classes; phase-1 trimming is gone. The
  hypertube does use `AFGPipelineHologram` (its limit is 100 m; earlier logs show `[pipeline]`
  trims at that limit). Twelfth run (hypertube, wall support to wall support) crashed the game
  inside `AFGPipelineHologram::ValidatePipeline` because the chain had emptied
  `mSnappedPassthroughs`; fixed by nulling its slots instead (see Phase 2 section). Pipe and
  track chains are still unproven; the track path has the most guesses.
- The user plays Satisfactory on this PC through Steam/Proton. The modding toolchain is the
  native Linux one (see README "Getting it building"). The Mac cannot build the custom engine.
- This repo is the single source of truth. Commit and push from here. It is cloned into the
  SatisfactoryModLoader starter project at `Mods/SegmentedSplines`.
- Linux PC layout: starter project `~/Projects/SatisfactoryModLoader` (master), engine
  `~/UnrealEngineCSS` registered as `UE_5.6.1-CSS`, MSVC via msvc-wine at `~/msvc`
  (`UE_WINE_MSVC`), Wwise SDK cached by `~/tools/wwise-cli`. Build the editor with
  `~/UnrealEngineCSS/Engine/Build/BatchFiles/Linux/Build.sh FactoryEditor Linux Development -project=<starter>/FactoryGame.uproject`
  (about 7 min incremental); a Makefile exists at the starter root (`make FactoryEditor`).
  The Wwise authoring project was never created; the editor's soundbank notice is expected.

## What the mod is for (user's words, condensed)

Building long, gently curved splines (belts, hypertubes, rails, pipes) is painful because of
the per-piece length limit. The user wants a *vanilla+* experience that also works when joining
a **vanilla (unmodded) dedicated server**, so raising the limit (Flex Splines, "Longer
Belt-Lift-Pipe-Track-Wire") is out: those need the mod on the server.

Phases, in order:
1. **Build what fits** (this draft). Drag past the limit; instead of red "too long", the hologram
   is cut at the limit *along the curve routed toward the cursor*, so the curve you aimed for is
   kept. Build, then continue from the end.
2. **Chain** (belts proven 2026-10-27; pipes/hypertubes and tracks written, untested): drag the
   full distance, build once, mod splits into N equal legal pieces and sends them one after
   another. Phase 1 is retired: the chain covers its case (one piece under the limit is just
   vanilla), and a snapped far end is handled by the final piece instead of being refused.
3. **Bonus**: optionally auto-place a chosen blueprint at each joint (mainly for rail junctions).

## The one rule that makes client-only work

From `FGBuildGun.h`: "Only the server is allowed to build stuff so all the actions are executed
on the server but may be simulated on the client." The client owns a local hologram; on build,
`UFGBuildGunStateBuild::Server_ConstructHologram(FNetConstructionID, FConstructHologramMessage)`
sends the serialised hologram (spline point data, snapped connections, ...) to the server. The
server deserialises into its own hologram, re-runs validation (`CheckValidPlacement`: length,
incline, curvature, clearance, cost) and constructs, or replies `Client_OnBuildableFailedConstruction`.

Therefore: **the mod may change the local hologram however it likes, but every construction
message it causes must be one a vanilla server would accept.** Never try to bypass a limit;
only ever send legal segments. Phase 2 must send pieces one at a time and wait for each to
exist before snapping the next to it (snapped connections are actor references).

SML facts (verified in docs):
- SML 3.8+: a client with SML can join a server with no SML. (Reverse is impossible, irrelevant.)
- SML 3.9+: a client refuses to join a server missing any mod marked `RequiredOnRemote: true`.
  This mod's `.uplugin` sets it **false**. Do not remove that.
- Host→client and client→host mod checks are governed by `RequiredOnRemote` / `RemoteVersionRange`.

## How the hooks work

- One set of hooks per hologram class (`AFGConveyorBeltHologram`, `AFGPipelineHologram`,
  `AFGRailroadTrackHologram`), installed by a local macro in `RegisterHooks` with
  `SUBSCRIBE_UOBJECT_METHOD` (patches the native class vtable through its CDO; Blueprint
  subclasses share that vtable, so all tiers are covered): `CheckValidPlacement` before and
  after, `TrySnapToActor`, `SetHologramLocationAndRotation`, `DoMultiStepPlacement` after.
  All handlers take `AFGSplineHologram*` and look up the type with `FindKind`.
- The build gun calls `ValidatePlacementAndCost` → `CheckValidPlacement` every update, after
  the spline has been routed, so by then `mSplineData` / `mSplineComponent` are current.
- Per-type knowledge is an `ESplineKind` switch inside `FSegmentedSplinesHooks` (`Kind*`
  functions), not a class hierarchy, so the Friend access covers all of it: length limit
  (`mMaxSplineLength` for belts/pipes, `mMaxLength` for tracks), the TooLong disqualifier
  (`UFGCDConveyorTooLong` / `UFGCDPipeTooLong` / `UFGCDTrackTooLong`), end supports (belt:
  pole + wall + ceiling mount per end; pipe: pole + wall mount; track: none), how a snapped
  start is recorded, how the far-end snap is cleared, the built actor class and its two
  connectors (`AFGBuildableConveyorBelt::GetConnection0/1`, `AFGBuildablePipeBase::
  GetConnection0/1`, `AFGBuildableRailroadTrack::GetConnection(0/1)`).
- End support at a joint: wall/ceiling mounts are disabled. A line trace on the build gun
  channel (`ECC_GameTraceChannel5`; `TC_BuildGun` is declared but not defined by the header
  stubs, so it does not link) goes straight down from the end as far as the tallest variation
  of the pole's buildable class (`AFGBuildablePole::GetPoleVariations`). Floor found (normal.Z
  >= cos 40 deg): `AFGPoleHologram::SetPoleHeight` picks the closest fixed height, the pole is
  moved onto the floor under the end facing along the curve, and the last spline point is
  nudged vertically onto the pole top (re-trimmed once if that pushed the length over). No
  floor, or the closest pole height misses by more than 60 cm: the pole is `SetDisabled(true)`,
  the same switch vanilla uses when a belt end snaps to a machine (`mIsDisabled` is
  CustomSerialization, so the server sees it too), and the end is left free. Supports we
  disabled are tracked in a static set and restored only when the drag comes back within the
  limit; vanilla's own disabling (snapping) is never touched.
- Access to protected/private members comes from `Config/AccessTransformers.ini` Friend entries
  for the hook class `FSegmentedSplinesHooks`. These only apply after a C++ build. If the
  header generator ignores a change, add/remove a `;` comment line to bump the mtime.
- Hooks are registered in `StartupModule` behind `#if !WITH_EDITOR` (docs: hooks can crash the editor).

## Unverified assumptions (check these first)

1. Hypertube recipes use `AFGPipelineHologram`: confirmed in-game (the pipeline hook logged
   trims at the hypertube's 10000 cm limit; fluid pipes are 5600).
2. `FSplinePointData` is **not** in the game headers; it is defined in the CSS-modified engine's
   `Components/SplineComponent.h` (every game header that uses it includes only that + FactoryGame.h).
   Fields used: `Location`, `ArriveTangent`, `LeaveTangent` (confirmed from FGBlueprintFunctionLibrary.cpp usage).
3. End support (see "How the hooks work"). Proven for belts on a vanilla server: a disabled
   pole with nothing snapped builds (free end in the air), and a seated pole at a joint builds.
   Pipes: the same code path, untested. Tracks: no supports; a free track end is legal in vanilla.
4. `IsLocalHologram()` is protected on `AFGHologram`; `AFGHologram` is in the Friend list for that.
5. Rail limit `mMaxLength` is set in Blueprint defaults (no C++ default); belts/pipes default 5600.1 cm.
6. Everything about the vanilla `.cpp` flow is inferred from headers + comments; the SML repo only
   ships stub `.cpp` files for FactoryGame. Verify with logging in-game.

## Test plan (also in README)

1. Belt dragged 2-4x the limit (done, works): the full hologram shows valid, one click builds
   every piece. Repeat for a hypertube, a fluid pipe and a track (untested).
2. Far end snapped to a machine input / junction / wall hole (belts done, pipes untested): the
   final piece should snap exactly as the full hologram did.
3. **Real proof**: local Linux dedicated server (Steam app 1690800) with **no SML**, join from the
   modded client, repeat 1. If the server shows the belt, client-only is proven.
4. `LogSegmentedSplines` category; set Verbose in Engine.ini for per-cut lines (already set in the
   Proton prefix's `Saved/Config/Windows/Engine.ini` on the Linux PC). Chain steps log at Log level.
5. Chain: Mk1 belt dragged 3-4x the limit across a gap; expect N pieces built one after another,
   joints with a pole where there is floor and free otherwise, the far end as vanilla would place it.

## Phase 2 (chaining) as implemented

In-game result that motivated it: on a vanilla server, a trimmed belt whose cut end hung in the
air built fine with the pole disabled (free end), so sending one legal piece after another works.

- Proven for belts (eleven runs); written the same way for pipes/hypertubes and tracks through
  the `FSplineKind` adapter, not yet run. `IsChainCandidate`: local, step PlacePoleOrSnapEnding
  or AdjustPole, length > limit, ceil(L / (limit - margin)) <= `kMaxChainPieces` (32).
- Type differences the driver has to care about (all in the `Kind*` functions):
  - Start snap of a later piece. Belt: `mSnappedConnectionComponents[0]`, clear
    `mSnappedWallPassthrough[0]`, `SetupSnappedConnectionDirections()`. Pipe:
    `mSnappedConnectionComponents[0]`, `mSnappedPassthroughs.Reset()`. Track: vanilla's own
    `TryFindAndSnapToOverlappingConnection(0, connectorLocation)` (private; Friend) is tried
    first, and if it did not land on the previous track's connector the fields are set by hand
    (`mSnappedConnectionComponents[0]`, `mSnappedStartLocation`, `mSnappedStartTangent` =
    connector normal, `mSnappedRailroadTrack` = null). Which one happened is logged.
  - Far-end clear. Belt: connection + wall passthrough [1]. Pipe: connection [1] and every
    `mSnappedPassthroughs` entry further than 6 m from the spline start is set to null (the
    array does not say which end a wall hole belongs to). NEVER shrink that array: vanilla's
    `ValidatePipeline` indexes it without a size check and the twelfth run crashed the game
    (`Array index out of bounds: 0 into an array of size 0`) when the chain had `RemoveAll`'d
    its null slots. Track: connection [1], `mUseCustomEndRotation` = false.
  - Track `mFloorData` is one floor sample per spline point (local space); whenever
    `mSplineData` is replaced it is resized to match (floor under each point by trace, else the
    point itself). Guess: vanilla indexes it per point.
  - Final build step before the step inputs: belts and pipes `SHBS_AdjustPole` (proven for
    free and snapped belt ends); tracks the step the player's own final click was accepted
    from (`FChainPlan::FinalStep`, logged by the `[diag] track DoMultiStepPlacement` line).
  - Belts store their spline in flow direction and the server may reverse the points; pipes and
    tracks have no flow, but the joint match accepts either connector regardless.
- After-hook on `CheckValidPlacement` removes `UFGCDConveyorTooLong` from `mConstructDisqualifiers`
  for a chain candidate, so the full hologram shows valid (everything else vanilla found stands).
- Pre-hooks on `TrySnapToActor` / `SetHologramLocationAndRotation` record the last cursor hit per
  belt hologram (`sLastHit`).
- Trigger: after-hook on `AFGConveyorBeltHologram::DoMultiStepPlacement`. A true result is the
  build gun accepting the click as the final step; it serialises and sends right after, so the
  hologram is reshaped into piece 0 in between (`StartChain` -> `PlanChain`: cut the curve into
  N equal pieces with `SplitSplineAtDistance`, reusing the hologram's spline component for each
  remainder. The splitter's SECOND half does not start at the split point (sixth run: piece 2's
  first point was a piece length from the joint); `MakeContinuous` reverses it if it ends there,
  else translates it onto the split point, and logs which. Every piece's end points are logged; piece 0 keeps the player's start, end snap cleared, joint via `SeatOrDisableEndPole`,
  re-validated). Fallback trigger plus diagnostics: `IFGConstructionMessageInterface::
  SerializeConstructMessage` hooked through the interface type (`SUBSCRIBE_METHOD_VIRTUAL` with
  the belt CDO cast to the interface). Hooking it as `AFGHologram::SerializeConstructMessage`
  installs but hands the handler the interface subobject (SML logs "This adjustment: 0x2b0"),
  which is why the second attempt was silent in-game. `sDriverBusy` makes the placement hooks
  stand aside while the driver touches a hologram. At build time only the length is checked,
  not the build step. Both triggers log a `[diag]` line at Log level on every call.
  Dead end: `UFGBuildGunStateBuild::InternalConstructHologram` is never called on the client
  (it is the server half); a hook there sees nothing.
- `TickChain` (core ticker) waits for the previous piece's belt to replicate: it scans
  `AFGBuildableConveyorBelt` actors for the one whose replicated `GetNetConstructionID()` has the
  NetPlayerID/Client_ID the client sent (captured in the serializer hook: the client serialises
  twice per build, an empty-ID pass for its pending copy, then the real ID), falling back to
  connection0/1 within 25 cm of the piece's ends; 10 s timeout, which then lists nearby belts
  with their IDs and end distances. Only belts that did not exist when the piece was sent
  qualify (`KnownBelts` snapshot): client IDs restart every launch, so a belt from an earlier
  session carries the same NetPlayerID/Client_ID (fourth run matched the previous session's
  piece after 24 ms and snapped piece 2 onto it: "hologram jumped back, invalid shape"). The
  far end must be within 100 cm of the expected joint at EITHER connector: a belt stores its
  spline in flow direction (connection0 = input, connection1 = output) and the server reverses
  the points when the player drags against the flow (fifth run: the built belt arrived in
  150 ms with c0 at the far end and c1 at the start, and was ignored for 10 s). The connector at
  the joint becomes piece i's start snap and its location piece i's first point;
  `SetupSnappedConnectionDirections()` then sets the piece's flow from that snap. Pieces are stored in world space and applied in the hologram's current transform
  (`ApplyPiece`), so vanilla moving the hologram during final-end placement cannot skew them.
  On abort, a hologram the driver configured is discarded with `ResetHologram()` (protected;
  `UFGBuildGunStateBuild` is in the Friend list for it). Third run: trigger and piece 1 worked,
  the end-position match never found the built belt (reason still unknown, logged now). Then `SendNextPiece` takes the fresh hologram vanilla spawned after the last
  build (`GetHologram()`), puts it in the plan's frame, snaps its start to the previous belt's
  `GetConnection1()`, disables its start supports by hand, sets the build step, and:
  - intermediate piece: clears the end snap, `SeatOrDisableEndPole` at the joint;
  - final piece: `TrySnapToActor(EndHit)` then `SetHologramLocationAndRotation(EndHit)` so vanilla
    recreates the end the player aimed at (pole, wall/ceiling mount, or snapped machine), then the
    piece's curve is put back under it;
  then `ValidatePlacementAndCost`, `mBuildStep = SHBS_AdjustPole` and
  `UFGBuildGunStateBuild::InternalExecuteDuBuildStepInput` (protected; Friend entry), the
  function the player's click reaches: vanilla runs DoMultiStepPlacement, assigns the
  construction ID, serialises (the hook stands aside), sends and spawns a fresh hologram. At
  the pole-adjust step the FIRST step input is swallowed (DoMultiStepPlacement answers "more
  steps" and stays on step 3, press or release alike: eighth and ninth runs) and the SECOND
  finishes the belt; a player's click is a press plus a release, which is why their click
  always built it. The driver sends a release (`true`), then a press (`false`) if the hologram
  is still there.
  `PrimaryFire()` called from code does nothing (seventh run: the configured hologram stayed,
  and the player's own click then built it, step 3, server accepted). If the build gun still
  holds the same hologram afterwards, nothing went out: the chain stops but leaves the
  hologram, which is valid, for the player to click.
- Splitter convention (confirmed seventh run): `SplitSplineAtDistance`'s second half has its
  first point at the local origin, i.e. it is relative to the split point; `MakeContinuous`
  translates it onto the split point and never flips it. (It used to flip the half when its
  END was at the split point; a straight belt cut exactly in half satisfies that by
  coincidence, and the ninth run's 8000 cm belt got piece 2 built backwards over piece 1.)
  Pieces of a 2-point curve are 2-point curves.
- Seating the joint pole can nudge the joint vertically (up to 60 cm); the next piece's start is
  then moved onto the built belt's real output connector (ninth run: 44.7 cm), its remaining
  points unchanged. Joint tolerance for recognising the built belt is 100 cm, so this fits.
- Abort (log Warning, leave the partial run) on: timeout (60 s, `kPieceTimeout`; the built belt
  is matched by ID so a late arrival still counts), the server's
  `Client_OnBuildableFailedConstruction` RPC for the pending ID (hooked on its `_Implementation`
  through the build state CDO), hologram gone or recipe changed, previous belt gone, piece not
  constructible (disqualifier names are logged).
- Not yet: a preview of the joint supports along the full hologram (the user asked for it; the
  full curve itself is shown by vanilla).

Everything above about vanilla's internals (what InternalConstructHologram checks, whether
SetHologramLocationAndRotation on a fresh hologram in PlacePoleOrSnapEnding needs more start state
than the snapped connection, FNetConstructionID semantics) is inferred from headers. First in-game
run of the chain is the test; `LogSegmentedSplines` Log-level lines narrate every step.

## Phase 3 note (blueprint at joints)

Blueprint placement (`AFGBlueprintHologram`) also goes through a construction message, so a
client-only mod can auto-place one at each joint. Open question: whether the dedicated server
needs the blueprint file itself (`Server_SetDesiredBlueprint(blueprintName)` is by name). Test
before designing.

## Where to look (SML repo = satisfactorymodding/SatisfactoryModLoader, master)

- `Source/FactoryGame/Public/Hologram/FGSplineHologram.h` — base: `mSplineData`, `mSplineComponent`, build steps
- `.../Hologram/FGConveyorBeltHologram.h`, `FGRailroadTrackHologram.h`, `FGPipelineHologram.h`
- `.../Hologram/HologramHelpers.h` — `FSplineUtils` incl. `SplitSplineAtDistance`, `CalcTangentRatioAtSplit`
- `.../Hologram/FGHologram.h` — `Construct`, pending construction, disqualifiers, `IsLocalHologram`
- `.../Equipment/FGBuildGun.h`, `FGBuildGunBuild.h` — authority model, `Server_ConstructHologram`
- `.../FGConstructionMessageInterface.h` — `FConstructHologramMessage`
- `Mods/SML/Source/SML/Public/Patching/NativeHookManager.h` — hook macros
- `Mods/Alpakit/Templates/CPPAndBlueprintBlank/` — the template this skeleton was derived from
- `Mods/GameFeatures/ExampleMod/ExampleMod.uplugin` — reference uplugin (SML ^3.12.0, GameVersion >=491125)

Docs (docs.ficsit.app/satisfactory-modding/latest/...):
- `Development/Linux/LinuxSetup.html` — Linux toolchain (Debian/Fedora package names; Arch equivalents in README)
- `Development/Cpp/hooking.html` — native hooking
- `Development/ModLoader/AccessTransformers.html`
- `Development/BeginnersGuide/ReleaseMod.html` — uplugin fields, Alpakit Release, SMR upload
- `Development/UpdatingFromSml37.html`, `UpdatingFromSml38.html` — client-only SML / RequiredOnRemote history
- `Development/Satisfactory/Multiplayer.html`, `Development/Satisfactory/BuildableHolograms.html`

## Conventions

- Mod reference `SegmentedSplines` is permanent once released on SMR; don't rename.
- Keep `RequiredOnRemote: false`. Keep `Config/Alpakit.ini` targets to `Windows` only (the Proton
  game is the Windows build; Linux docs note Wine is needed to build the Win64 target).
- `Resources/Icon128.png` is still missing; needed before an SMR upload.
- Build artifacts (`Binaries/`, `Intermediate/`, `Saved/`) are gitignored.
