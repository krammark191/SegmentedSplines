#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Engine/HitResult.h"
#include "Containers/Ticker.h"
#include "Templates/SubclassOf.h"
#include "FGBuildableSubsystem.h"
#include "Components/SplineComponent.h"

class AFGHologram;
class AFGSplineHologram;
class AFGPoleHologram;
class AFGConveyorBeltHologram;
class AFGRailroadTrackHologram;
class AFGPipelineHologram;
class AFGBuildableConveyorBelt;
class UFGBuildGunStateBuild;
class UFGRecipe;
class FArchive;
class IFGConstructionMessageInterface;

/**
 * Segmented Splines.
 *
 * Vanilla lets you drag a spline hologram past its length limit, keeps routing the curve toward
 * your cursor, then refuses to build with "too long". Everything here runs on the local player's
 * hologram only and never sends the server anything a vanilla client could not have sent.
 *
 * Belts (phase 2, "chain"): the full hologram stays as vanilla routed it and its "too long"
 * disqualifier is removed. When the player builds, the build gun's construct call is intercepted:
 * the curve is cut into N equal pieces, each under the limit, and they are sent one at a time.
 * Each piece after the first snaps its start to the previous belt's output, which means waiting
 * for that belt to replicate back from the server before the next piece can be sent. The last
 * piece ends where the cursor was, with vanilla's own end support (pole, wall or ceiling mount,
 * or a snapped machine input); the joints in between get a ground pole if there is a floor in
 * reach below them and are left free otherwise.
 *
 * Pipes (phase 1, "build what fits"): the spline is cut at the limit while keeping the routed
 * curve, and the end pole is seated on the floor below the cut or disabled if there is none.
 *
 * Disabling a support uses the same switch vanilla flips when a belt end snaps to a machine
 * (mIsDisabled, which is in the construction message), so the server sees the same thing.
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

	/** Longest chain we are willing to build in one go. */
	static constexpr int32 kMaxChainPieces = 32;

	/** A replicated belt whose ends are within this distance of a piece's ends is that piece. [cm] */
	static constexpr float kJointTolerance = 25.0f;

	/** Give up on a piece if the server has not sent its belt back within this time. [s] */
	static constexpr double kPieceTimeout = 10.0;

	/** The supports vanilla may have placed under one end of a spline. */
	struct FEndSupports
	{
		/** The height-adjustable ground pole (belts, pipes). May be null (rails). */
		AFGPoleHologram* Pole = nullptr;
		/** Wall / ceiling mounts. Disabled whenever we take over an end. */
		TArray< AFGHologram*, TInlineAllocator< 2 > > Mounts;
	};

	/** One belt chain in flight. Only one can exist: the build gun only builds one thing at a time. */
	struct FChainPlan
	{
		TWeakObjectPtr< UFGBuildGunStateBuild > BuildState;
		TWeakObjectPtr< UWorld > World;
		TSubclassOf< UFGRecipe > Recipe;
		/** Transform of the hologram the plan was made from; every piece is in this space. */
		FTransform Frame;
		TArray< TArray< FSplinePointData > > Pieces;
		/** What the cursor hit when the player placed the far end; recreates vanilla's end support. */
		FHitResult EndHit;
		bool bHasEndHit = false;
		/** Index of the next piece to send. */
		int32 NextPiece = 0;
		/** Waiting for the belt of piece NextPiece-1 to replicate. */
		bool bWaiting = false;
		FVector WaitStart = FVector::ZeroVector;
		FVector WaitEnd = FVector::ZeroVector;
		double SentAt = 0.0;
		TWeakObjectPtr< AFGBuildableConveyorBelt > LastBelt;
	};

	// --- phase 1: cut at the limit (pipes; also used for the belt pieces' joints) -------------

	/** Shared implementation. Returns true if the hologram was truncated. */
	static bool TruncateIfTooLong( AFGSplineHologram* hologram, float maxLength, const FEndSupports& supports, const TCHAR* debugName );

	/**
	 * Seat the end pole on the floor below the spline end, or disable it if there is none in reach.
	 * May nudge the last spline point vertically and re-trim; returns the final spline length.
	 * @param oldEndWorld / oldEndYaw  where the end was before we moved it, if the pole was placed
	 *        there by vanilla; its top offset and facing are carried over. Pass bPoleIsVanillaPlaced
	 *        false for a fresh hologram, where the pole's current state means nothing.
	 */
	static float SeatOrDisableEndPole( AFGSplineHologram* hologram, float cutDistance, const FEndSupports& supports,
		const FVector& oldEndWorld, float oldEndYaw, bool bPoleIsVanillaPlaced, const TCHAR* debugName );

	/** Disable a support hologram and remember that it was us, not vanilla, who did it. */
	static void DisableSupport( AFGHologram* support );

	/** Re-enable any of the given supports that we disabled earlier. */
	static void RestoreSupports( const FEndSupports& supports );

	/** Tallest height the pole's buildable class can be built at. */
	static float GetMaxPoleHeight( const AFGPoleHologram* pole );

	static FEndSupports GetBeltSupports( AFGConveyorBeltHologram* belt, int32 endIndex );

	// --- phase 2: chain (belts) ---------------------------------------------------------------

	/** True while the chain driver is configuring or constructing a piece; the placement hooks stand aside. */
	static bool sDriverBusy;

	/** @param bRequireDragStep  true while validating (the player is still dragging); false at build time. */
	static bool IsChainCandidate( AFGConveyorBeltHologram* belt, bool bRequireDragStep );
	static void OnBeltCheckValidPlacement( AFGConveyorBeltHologram* self );
	static void OnBeltAfterCheckValidPlacement( AFGConveyorBeltHologram* self );
	static void OnBeltPlacementHit( AFGConveyorBeltHologram* self, const FHitResult& hit );
	/** The player's click was accepted as the final placement step; vanilla serialises and sends next. */
	static void OnBeltAfterMultiStepPlacement( bool bFinalStep, AFGConveyorBeltHologram* self );
	/** The client turns a hologram into a construction message (also runs on loading; fallback trigger). */
	static void OnSerializeConstructMessage( IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id );
	/** Reshape a chainable belt into piece 0 and start the driver, once. */
	static void StartChain( AFGConveyorBeltHologram* belt, const TCHAR* trigger );

	/** The build gun's build state for the player who owns the hologram. */
	static UFGBuildGunStateBuild* FindBuildState( AFGHologram* hologram );
	/** Cut the belt's full curve into equal pieces under the limit. Leaves the hologram on piece 0. */
	static bool PlanChain( AFGConveyorBeltHologram* belt, UFGBuildGunStateBuild* buildState );
	/** Configure the build gun's current hologram as the next piece and have the build gun build it. */
	static void SendNextPiece();
	static bool TickChain( float deltaTime );
	static void AbortChain( const TCHAR* reason );
	static void ValidateForConstruction( AFGConveyorBeltHologram* belt );

	static void OnTrackCheckValidPlacement( AFGRailroadTrackHologram* self );
	static void OnPipelineCheckValidPlacement( AFGPipelineHologram* self );

	/** Supports we switched off. Vanilla may legitimately disable the same holograms (snapping), so we only ever restore our own. */
	static TSet< TWeakObjectPtr< AFGHologram > > sDisabledByUs;
	/** Last cursor hit each belt hologram was placed with; the chain's final piece is placed with it again. */
	static TMap< TWeakObjectPtr< AFGHologram >, FHitResult > sLastHit;
	static TUniquePtr< FChainPlan > sChain;
	static FTSTicker::FDelegateHandle sTicker;
};
