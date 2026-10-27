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
  end back to vanilla, which sidesteps it). Phase 2 chain is written but has not run in-game yet.
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
2. **Chain** (first version in, belts only, 2026-10-26): drag the full distance, build once, mod
   splits into N equal legal pieces and sends them one after another.
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

## How the current hook works

- Pre-hooks on `CheckValidPlacement` of `AFGConveyorBeltHologram`, `AFGRailroadTrackHologram`,
  `AFGPipelineHologram` via `SUBSCRIBE_UOBJECT_METHOD` (patches the native class vtable through
  its CDO; Blueprint subclasses share that vtable, so all tiers are covered).
- The build gun calls `ValidatePlacementAndCost` → `CheckValidPlacement` every update, after
  the spline has been routed, so by then `mSplineData` / `mSplineComponent` are current.
- If `mSplineComponent->GetSplineLength() > limit`: call the game's own
  `FSplineUtils::SplitSplineAtDistance(spline, limit - 2cm, first, second)` (declared in
  `Hologram/HologramHelpers.h`, used by the game for belt welding / blueprint splitting; it
  preserves tangents), set `mSplineData = first`, call `UpdateSplineComponent()`. Vanilla
  validation then passes.
- End support after a trim (belts, pipes): wall/ceiling mount children for the far end are
  disabled. A line trace on the build gun channel (`ECC_GameTraceChannel5`; `TC_BuildGun` is
  declared but not defined by the header stubs, so it does not link) goes straight down from the cut end as far as the
  tallest variation of the pole's buildable class (`AFGBuildablePole::GetPoleVariations`). Floor
  found (normal.Z >= cos 40 deg): `AFGPoleHologram::SetPoleHeight` picks the closest fixed height,
  the pole is moved onto the floor under the end and turned by the change in end direction, and
  the last spline point is nudged vertically onto the pole top (re-trimmed once if that pushed
  the length over). No floor, or the closest pole height misses by more than 60 cm: the pole is
  `SetDisabled(true)`, the same switch vanilla uses when a belt end snaps to a machine
  (`mIsDisabled` is CustomSerialization, so the server sees it too), and the end is left free.
  Supports we disabled are tracked in a static set and restored only when the drag comes back
  within the limit; vanilla's own disabling (snapping) is never touched.
- Skipped when: not local hologram; build step != `SHBS_PlacePoleOrSnapEnding`; far end snapped
  to a connection (`IsConnectionSnapped(true)`), so aiming at a machine input keeps vanilla's
  "too long" rather than leaving a belt floating short of it.
- Access to protected/private members comes from `Config/AccessTransformers.ini` Friend entries
  for the hook class `FSegmentedSplinesHooks`. These only apply after a C++ build. If the
  header generator ignores a change, add/remove a `;` comment line to bump the mtime.
- Hooks are registered in `StartupModule` behind `#if !WITH_EDITOR` (docs: hooks can crash the editor).

Alternative if the cut flickers or runs too early: move the shared helper to a post-hook on
`PostHologramPlacement(const FHitResult&, bool)` (public virtual on all three classes). The belt
header has `mAutomaticallyGenerateSpline` with comment "Whether or not to automatically generate
spline in PostHologramPlacement", which is why PostHologramPlacement is the other candidate.

## Unverified assumptions (check these first)

1. Hypertube recipes use `AFGPipelineHologram` (or a subclass). Both buildables derive from
   `AFGBuildablePipeBase`, but confirm in the editor. If not, add a fourth hook.
2. `FSplinePointData` is **not** in the game headers; it is defined in the CSS-modified engine's
   `Components/SplineComponent.h` (every game header that uses it includes only that + FactoryGame.h).
   Fields used: `Location`, `ArriveTangent`, `LeaveTangent` (confirmed from FGBlueprintFunctionLibrary.cpp usage).
3. End support (see "How the current hook works"). In-game on a vanilla server the first version
   (pole shifted blindly) built fine on flat ground and went red over empty air. The current
   version seats or disables the pole; untested as of writing: (a) whether vanilla's belt
   validation accepts a disabled pole with nothing snapped (vanilla only ever pairs "disabled"
   with "snapped"), (b) that the pole's yaw convention survives the delta-yaw carry-over, (c)
   pipes. If (a) fails, fall back to a support search (trace sideways for a wall mount, walk the
   cut back to the previous pillar).
4. `IsLocalHologram()` is protected on `AFGHologram`; `AFGHologram` is in the Friend list for that.
5. Rail limit `mMaxLength` is set in Blueprint defaults (no C++ default); belts/pipes default 5600.1 cm.
6. Everything about the vanilla `.cpp` flow is inferred from headers + comments; the SML repo only
   ships stub `.cpp` files for FactoryGame. Verify with logging in-game.

## Test plan (also in README)

1. Singleplayer: Mk1 belt, curve mode, drag far past 56 m → valid hologram ending ~56 m along the
   curve, pole under the new end, builds. Repeat for pipe and rail.
2. Aim a long belt at a machine input → vanilla red "too long" (snap case excluded by design).
3. **Real proof**: local Linux dedicated server (Steam app 1690800) with **no SML**, join from the
   modded client, repeat 1. If the server shows the belt, client-only is proven.
4. `LogSegmentedSplines` category; set Verbose in Engine.ini for per-cut lines (already set in the
   Proton prefix's `Saved/Config/Windows/Engine.ini` on the Linux PC). Chain steps log at Log level.
5. Chain: Mk1 belt dragged 3-4x the limit across a gap; expect N pieces built one after another,
   joints with a pole where there is floor and free otherwise, the far end as vanilla would place it.

## Phase 2 (chaining) as implemented for belts

In-game result that motivated it: on a vanilla server, a trimmed belt whose cut end hung in the
air built fine with the pole disabled (free end), so sending one legal piece after another works.

- Belts no longer trim. `IsChainCandidate`: local, step PlacePoleOrSnapEnding or AdjustPole,
  length > `mMaxSplineLength`, ceil(L / (limit - margin)) <= `kMaxChainPieces` (32).
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
  `UFGBuildGunStateBuild::PrimaryFire()`: vanilla treats it as the player's final click, assigns
  the construction ID, serialises (the hook stands aside), sends and spawns a fresh hologram. If
  the build gun still holds the same hologram afterwards, nothing went out and the chain aborts.
- Abort (log Warning, leave the partial run) on: timeout, hologram gone or recipe changed, previous
  belt gone, piece not constructible (disqualifier names are logged).
- Not yet: chaining for pipes and tracks (they still trim, phase 1), a preview of the joint
  supports along the full hologram (the user asked for it; the full curve itself is shown by
  vanilla), handling of vanilla's own construction failure RPC (timeout covers it).

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
