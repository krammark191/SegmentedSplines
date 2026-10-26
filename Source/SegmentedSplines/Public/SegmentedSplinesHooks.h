#pragma once

#include "CoreMinimal.h"

class AFGSplineHologram;
class AFGConveyorBeltHologram;
class AFGRailroadTrackHologram;
class AFGPipelineHologram;
class AActor;

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

	/** Shared implementation. Returns true if the hologram was truncated. */
	static bool TruncateIfTooLong( AFGSplineHologram* hologram, float maxLength, AActor* endPoleHologram, const TCHAR* debugName );

	static void OnBeltCheckValidPlacement( AFGConveyorBeltHologram* self );
	static void OnTrackCheckValidPlacement( AFGRailroadTrackHologram* self );
	static void OnPipelineCheckValidPlacement( AFGPipelineHologram* self );
};
