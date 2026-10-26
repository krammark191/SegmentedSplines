# CLAUDE.md — Segmented Splines handoff

This file is the context handoff from the Mac session that drafted this mod to the Claude
session on the Linux (CachyOS) PC that will compile, test and continue it. Read it fully once.

## Status

- Everything in this repo is an **uncompiled draft**. No file has ever been through UBT.
  Expect the first build to surface small errors (includes, access, signatures). Fix them here.
- The user plays Satisfactory on this PC through Steam/Proton. The modding toolchain is the
  native Linux one (see README "Getting it building"). The Mac cannot build the custom engine.
- This repo is the single source of truth. Commit and push from here. It is cloned into the
  SatisfactoryModLoader starter project at `Mods/SegmentedSplines`.

## What the mod is for (user's words, condensed)

Building long, gently curved splines (belts, hypertubes, rails, pipes) is painful because of
the per-piece length limit. The user wants a *vanilla+* experience that also works when joining
a **vanilla (unmodded) dedicated server**, so raising the limit (Flex Splines, "Longer
Belt-Lift-Pipe-Track-Wire") is out: those need the mod on the server.

Phases, in order:
1. **Build what fits** (this draft). Drag past the limit; instead of red "too long", the hologram
   is cut at the limit *along the curve routed toward the cursor*, so the curve you aimed for is
   kept. Build, then continue from the end.
2. **Chain**: drag the full distance, build once, mod splits into N equal legal pieces and sends
   them one after another.
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
  preserves tangents), set `mSplineData = first`, call `UpdateSplineComponent()`, and shift the
  end-pole child hologram by the same delta as the belt end moved. Vanilla validation then passes.
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
3. End pole handling is a plain world-offset shift. On sloped ground it will float/sink and it is
   not re-aimed along the new end tangent. Proper fix: line-trace down from the new end, run the
   pole hologram's own `SetHologramLocationAndRotation` with that hit, then re-anchor the belt
   end to the pole's snap connection (`AFGConveyorPoleHologram::GetSnapConnection()`).
   Belts also have wall/ceiling pole variants (`mChildWallPoleHologram`, `mChildCeilingPoleHologram`)
   that are currently ignored.
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
4. `LogSegmentedSplines` category; set Verbose in Engine.ini for per-cut lines.

## Phase 2 design sketch (chaining)

- Let the local hologram show the full curve (suppress the cut while a "chain mode" toggle is on,
  but still mark it valid only if each prospective piece would be valid).
- On construct: split the full spline with `SplitSplineAtDistance` repeatedly into
  `ceil(L / limit)` equal arc-length pieces, each < limit.
- Send piece 1 through the normal build path. Wait for the server's constructed buildable to
  replicate (`Client_OnBuildableConstructed` on the build state, or poll for the new actor), snap
  piece 2's start to its end connection (`UFGFactoryConnectionComponent` for belts,
  `UFGRailroadTrackConnectionComponent` for rails, `UFGPipeConnectionComponentBase` for pipes),
  send, repeat. A few hundred ms per piece on a remote server.
- Must handle mid-chain rejection (clearance, running out of belts: cost is checked per piece)
  by stopping cleanly and leaving the partial run.
- `AFGHologram::SetupPendingConstructionHologram` exists ("client-sided representation of a
  hologram that the server will construct soon") but pieces cannot snap to a pending actor, so
  waiting is required.

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
