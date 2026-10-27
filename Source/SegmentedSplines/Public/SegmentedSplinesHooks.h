#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class AFGHologram;
class AFGSplineHologram;
class AFGPoleHologram;
class AFGConveyorBeltHologram;
class AFGRailroadTrackHologram;
class AFGPipelineHologram;

/**
 * Phase 1 of Segmented Splines: "build what fits".
 *
 * Vanilla lets you drag a spline hologram past its length limit, keeps routing the curve
 * toward your cursor, then refuses to build with "too long". We hook each spline hologram's
 * CheckValidPlacement (which the build gun runs every update, right after the spline has been
 * routed) and, if the routed spline is over the limit, cut it at the limit while keeping the
 * exact curve that was routed toward the far destination. Vanilla validation then passes and
 * the construction message the client sends to the server only ever contains a legal segment,
 * so an unmodded server accepts it.
 *
 * The cut end then needs a support. Vanilla placed the end pole under the cursor, which is
 * usually nowhere near the cut. We line-trace straight down from the cut end: if there is a
 * floor within the pole's reach, the pole is seated on it at the right height and the spline
 * end is nudged onto the pole top; otherwise the pole is disabled, which is the same switch
 * vanilla flips when a belt end snaps to a machine, so the end is simply left free (a belt
 * whose pole was dismantled is in the same state).
 *
 * Everything here runs on the local player's hologram only. Nothing is replicated by this mod.
 *
 * The class name is referenced from Config/AccessTransformers.ini (Friend=...), which is what
 * lets it touch protected/private hologram members. Keep the two in sync if you rename it.
 */
class FSegmentedSplinesHooks
{
public:
	/** Install all native hooks. Call once from the module's StartupModule, never in the editor. */
	static void RegisterHooks();

private:
	/** How far below the hard limit we cut, so float error can never push the result over it. [cm] */
	static constexpr float kSafetyMargin = 2.0f;

	/** Trace starts this far above the cut end so it cannot begin inside the floor. [cm] */
	static constexpr float kTraceStartLift = 20.0f;

	/**
	 * Pole heights come in fixed steps. If the closest step leaves the pole top within this
	 * distance of the spline end, the end is nudged onto the pole; further than this the pole
	 * cannot reach and is disabled instead. [cm]
	 */
	static constexpr float kMaxPoleSeatNudge = 60.0f;

	/** Used when the pole's buildable class does not expose its height variations. [cm] */
	static constexpr float kFallbackMaxPoleHeight = 800.0f;

	/** Floors steeper than this (normal.Z below the value) are not accepted. cos(40 deg). */
	static constexpr float kMinFloorNormalZ = 0.766f;

	/** The supports vanilla may have placed under the far end of a spline. */
	struct FEndSupports
	{
		/** The height-adjustable ground pole for the far end (belts, pipes). May be null (rails). */
		AFGPoleHologram* Pole = nullptr;
		/** Wall / ceiling mounts for the far end. Disabled whenever we trim. */
		TArray< AFGHologram*, TInlineAllocator< 2 > > Mounts;
	};

	/** Shared implementation. Returns true if the hologram was truncated. */
	static bool TruncateIfTooLong( AFGSplineHologram* hologram, float maxLength, const FEndSupports& supports, const TCHAR* debugName );

	/**
	 * Seat the end pole on the floor below the cut end, or disable it if there is none in reach.
	 * May nudge the last spline point vertically and re-trim; returns the final spline length.
	 */
	static float SeatOrDisableEndPole( AFGSplineHologram* hologram, float cutDistance, const FEndSupports& supports,
		const FVector& oldEndWorld, float oldEndYaw, const TCHAR* debugName );

	/** Disable a support hologram and remember that it was us, not vanilla, who did it. */
	static void DisableSupport( AFGHologram* support );

	/** Re-enable any of the given supports that we disabled earlier. */
	static void RestoreSupports( const FEndSupports& supports );

	/** Tallest height the pole's buildable class can be built at. */
	static float GetMaxPoleHeight( const AFGPoleHologram* pole );

	static void OnBeltCheckValidPlacement( AFGConveyorBeltHologram* self );
	static void OnTrackCheckValidPlacement( AFGRailroadTrackHologram* self );
	static void OnPipelineCheckValidPlacement( AFGPipelineHologram* self );

	/** Supports we switched off. Vanilla may legitimately disable the same holograms (snapping), so we only ever restore our own. */
	static TSet< TWeakObjectPtr< AFGHologram > > sDisabledByUs;
};
