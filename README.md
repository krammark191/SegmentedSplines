# Segmented Splines (draft skeleton)

A client-side-only Satisfactory mod. Drag a belt, pipe, hypertube or rail hologram past its
length limit and, instead of a red "too long" hologram, you get the longest legal piece cut from
the curve you were aiming at. Build it, then start the next piece from its end.

Phase 2 (planned) chains the pieces automatically; phase 3 drops a chosen blueprint at each joint.

## Why it works on vanilla servers

Only the server constructs. The client serialises its local hologram into a construction message
and the server re-validates it. This mod only ever changes the *local* hologram so that what the
client sends is already a legal segment, so a server with no SML and no mods accepts it.

* `RequiredOnRemote: false` in the `.uplugin` is essential. SML 3.9+ clients refuse to join a
  server that lacks a mod marked required, which would lock you out of vanilla servers.
* A client with SML can join a server without SML (SML 3.8+). The reverse is not possible, but
  that direction does not matter here.

## Layout

```
SegmentedSplines/
  SegmentedSplines.uplugin           mod metadata (RequiredOnRemote=false, SML ^3.12)
  Config/AccessTransformers.ini      friend access to hologram internals (needs C++ rebuild)
  Config/Alpakit.ini                 package the Windows client target only
  Config/PluginSettings.ini          standard template file
  Resources/                         put a 128x128 Icon128.png here before uploading to SMR
  Source/SegmentedSplines/
    SegmentedSplines.Build.cs
    Public/SegmentedSplines.h        Unreal module
    Private/SegmentedSplines.cpp     registers hooks (not in editor)
    Public/SegmentedSplinesHooks.h
    Private/SegmentedSplinesHooks.cpp  the actual truncation logic
```

## How the hook works

* `AFGConveyorBeltHologram`, `AFGRailroadTrackHologram` and `AFGPipelineHologram` all override
  `CheckValidPlacement`, which the build gun runs every update after the spline has been routed.
* We install a **pre** hook on each with SML's `SUBSCRIBE_UOBJECT_METHOD`. The hook measures the
  routed spline; if it is over the class's limit it calls the game's own
  `FSplineUtils::SplitSplineAtDistance` at (limit - 2 cm), keeps the first half as the hologram's
  spline data, rebuilds the spline component, and shifts the end pole by the same offset.
* Vanilla's `CheckValidPlacement` then runs on the trimmed spline and passes.
* The hook does nothing when the far end is snapped to a connection, so aiming at a machine
  input still gives vanilla's "too long" instead of a belt that stops short of it.

Limits as of this draft: belts and pipes 5600.1 cm (from the headers); rails read `mMaxLength`
from the Blueprint defaults at runtime.

## Getting it building on your CachyOS PC

The Mac cannot build this: the custom Coffee Stain engine is Windows/Linux only. Your Linux PC can.
Follow the official guide and treat the notes below as the Arch translation of it:
https://docs.ficsit.app/satisfactory-modding/latest/Development/Linux/LinuxSetup.html

1. Packages. The guide lists Debian/Fedora names; on Arch:
   ```bash
   sudo pacman -S --needed git msitools tar zstd dos2unix wine
   ```
   Wine is needed for the Wwise authoring tool and to build the Win64 Alpakit target (which is
   the one your Proton game runs).
2. MSVC via msvc-wine (`ue-patches` branch), then set `UE_WINE_MSVC` and re-login, per the guide.
3. Engine access: link GitHub to Epic and to the ficsit linker, then download the
   `UnrealEngine-CSS-Editor-Linux.tar.zst.*` parts, extract, and register with
   `UnrealVersionSelector -register -unattended`. Budget 30+ GB.
4. Wwise 2025.1.6.9117 via wwise-cli, integrate, create the Wwise project, generate soundbanks.
5. Clone the Starter Project (`satisfactorymodding/SatisfactoryModLoader`). Open it in Rider
   (install the "Unreal Project Model Mods Fix" plugin) or VS Code, build `FactoryEditor` for
   Linux Development, open `FactoryGame.uproject`.
6. In the editor, Alpakit Dev -> Create Mod -> template **"C++ & Blueprint Library (Advanced)"**,
   mod reference `SegmentedSplines`. Close the editor, then copy this folder's `Config/`,
   `Source/` and `.uplugin` over the generated `Mods/SegmentedSplines/` (overwrite). Keep the
   wizard's generated `Resources/Icon128.png` if it made one.
7. Rebuild the editor target (Access Transformers only apply after a C++ build; if the generated
   accessors do not appear, add or remove a `;` comment line in `AccessTransformers.ini`).
8. Alpakit: set the game path to the Proton install, typically
   `~/.local/share/Steam/steamapps/common/Satisfactory`, target **Windows**, and use Alpakit Dev
   to copy the packaged mod straight into `FactoryGame/Mods/` there. Install SML into that game
   install once with the native Linux Satisfactory Mod Manager.

## Test plan

1. Singleplayer: drag a Mk1 belt far past 56 m in curve mode. Expect: hologram turns valid, ends
   at ~56 m along the curve, end pole sits under the new end. Build. Repeat for a pipe and a rail.
2. Aim a long belt at a machine input: expect vanilla's red "too long" (snap case is excluded).
3. The real proof: run the Linux dedicated server (Steam app 1690800) locally **without** SML,
   join from the modded client, repeat test 1. If the belt appears for the server too, the
   client-only design is confirmed.
4. Watch the log for `LogSegmentedSplines` lines (set it to Verbose in `Engine.ini` to see the
   per-cut messages).

## Things to verify in the editor that I could not verify from headers alone

* That the hypertube recipe's hologram class is `AFGPipelineHologram` (or a subclass of it).
* That `CheckValidPlacement` is indeed called after routing every update (if the cut flickers,
  move the hook to a post-hook on `PostHologramPlacement` instead; the helper is shared).
* The exact end-pole behaviour on sloped ground (see KNOWN LIMITATION in the .cpp).

## Phase 2 sketch: chaining

Keep the same truncation helper, but on construct: compute the full long spline locally, split
it with `SplitSplineAtDistance` into equal arc-length pieces each under the limit, send piece 1
through the normal build path, wait for `Client_OnBuildableConstructed` / the new buildable to
replicate, snap piece 2's start to its end connection, send, repeat. Needs a small client-side
queue and a clean abort if the server rejects a piece (clearance, cost). Each piece is still a
plain vanilla construction message, so the vanilla-server property is preserved.
