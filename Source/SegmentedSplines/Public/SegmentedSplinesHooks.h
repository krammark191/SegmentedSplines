#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Engine/HitResult.h"
#include "Containers/Ticker.h"
#include "Templates/SubclassOf.h"
#include "FGBuildableSubsystem.h"
#include "Components/SplineComponent.h"
#include "Hologram/FGSplineHologram.h"

class AFGHologram;
class AFGSplineHologram;
class AFGPoleHologram;
class AFGBuildable;
class UFGConnectionComponent;
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
 * Chain (belts, pipes and hypertubes, railroad tracks): the full hologram stays as vanilla routed
 * it and its "too long" disqualifier is removed. When the player builds, the build gun's construct
 * call is intercepted: the curve is cut into N equal pieces, each under the limit, and they are
 * sent one at a time. Each piece after the first snaps its start to the previous piece's far
 * connector, which means waiting for that buildable to replicate back from the server before the
 * next piece can be sent. The last piece ends where the cursor was, with vanilla's own end
 * support (pole, wall or ceiling mount, or a snapped connection); the joints in between get a
 * ground pole if there is a floor in reach below them and are left free otherwise (tracks have no
 * supports at all).
 *
 * Disabling a support uses the same switch vanilla flips when a belt end snaps to a machine
 * (mIsDisabled, which is in the construction message), so the server sees the same thing.
 *
 * What differs between the three spline types (hologram class, length limit, disqualifier, which
 * connectors the built actor has, how a snapped start is recorded) lives in FSplineKind.
 *
 * The class name is referenced from Config/AccessTransformers.ini (Friend=...), which is what
 * lets it touch protected/private hologram members. Keep the two in sync if you rename it.
 */
class FSegmentedSplinesHooks
{
public:
	/** Install all native hooks. Call once from the module's StartupModule, never in the editor. */
	static void RegisterHooks();

	/** The supports vanilla may have placed under one end of a spline. */
	struct FEndSupports
	{
		/** The height-adjustable ground pole (belts, pipes). May be null (tracks). */
		AFGPoleHologram* Pole = nullptr;
		/** Wall / ceiling mounts. Disabled whenever we take over an end. */
		TArray< AFGHologram*, TInlineAllocator< 2 > > Mounts;
	};

	/** Which spline type a hologram is. Everything type-specific is a switch on this inside the
	 *  hook class, so the Friend access from AccessTransformers.ini covers all of it. */
	enum class ESplineKind : uint8 { Belt, Pipe, Track };
	struct FSplineKind
	{
		ESplineKind Type;
		const TCHAR* Name;
		/** Belts and pipes finish on the pole-adjust step whatever the end did; tracks have no such step. */
		bool bHasPoleAdjustStep;
	};

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

	/** A piece's planned start is moved onto the built connector if it is within this. The joint
	 *  match itself allows four times this, since seating a pole can nudge an end by up to 60 cm. [cm] */
	static constexpr float kJointTolerance = 25.0f;

	/** How long to wait for the server to build a piece. A lag spike on a remote server (tenth run:
	 *  the link dropped for ~45 s, the piece showed up when it came back) must not end the chain;
	 *  an outright rejection ends it at once through the build gun's failure RPC. [s] */
	static constexpr double kPieceTimeout = 60.0;
	/** After this long a "still waiting" line is logged once, so a slow piece explains itself. [s] */
	static constexpr double kPieceSlowNotice = 5.0;

	/** One chain in flight. Only one can exist: the build gun only builds one thing at a time. */
	struct FChainPlan
	{
		const FSplineKind* Kind = nullptr;
		TWeakObjectPtr< UFGBuildGunStateBuild > BuildState;
		TWeakObjectPtr< UWorld > World;
		TSubclassOf< UFGRecipe > Recipe;
		/** Transform of the hologram the plan was made from; fresh holograms are moved here. */
		FTransform Frame;
		/** Pieces in world space; applied to whatever transform the hologram has at the time. */
		TArray< TArray< FSplinePointData > > Pieces;
		/** What the cursor hit when the player placed the far end; recreates vanilla's end support. */
		FHitResult EndHit;
		bool bHasEndHit = false;
		/** The build step the player's hologram was on when its click was accepted as final. */
		ESplineHologramBuildStep FinalStep = ESplineHologramBuildStep::SHBS_AdjustPole;
		/** Index of the next piece to send. */
		int32 NextPiece = 0;
		/** Construction ID the client sent for the piece being waited on; the built actor carries it (replicated). */
		FNetConstructionID PendingId;
		bool bHasPendingId = false;
		/** Buildables that already existed when the piece was sent; the built one is one not in here. */
		TSet< TWeakObjectPtr< AActor > > Known;
		/** Whether the build gun's current hologram was configured by the driver (reset it on abort). */
		bool bHologramIsOurs = false;
		/** Waiting for the buildable of piece NextPiece-1 to replicate. */
		bool bWaiting = false;
		FVector WaitStart = FVector::ZeroVector;
		FVector WaitEnd = FVector::ZeroVector;
		double SentAt = 0.0;
		bool bSlowNoticed = false;
		TWeakObjectPtr< AFGBuildable > Last;
		/** The built actor's connector at the joint, whichever of its two it turned out to be. */
		TWeakObjectPtr< UFGConnectionComponent > JointConnection;
	};

	// --- per-type knowledge --------------------------------------------------------------------

	static const FSplineKind* FindKind( const AFGHologram* hologram );
	static UClass* KindTooLongDisqualifier( const FSplineKind* kind );
	static float KindMaxLength( const FSplineKind* kind, const AFGSplineHologram* hologram );
	static FEndSupports KindSupports( const FSplineKind* kind, AFGSplineHologram* hologram, int32 endIndex );
	/** Record a snapped start on a fresh hologram, the way vanilla would after the player clicked on that connector. */
	static void KindSnapStart( const FSplineKind* kind, AFGSplineHologram* hologram, UFGConnectionComponent* joint );
	/** Forget whatever the far end was snapped to. */
	static void KindClearEndSnap( const FSplineKind* kind, AFGSplineHologram* hologram );
	/** mSplineData was replaced wholesale; fix anything that has to stay parallel to it. */
	static void KindOnSplineReplaced( const FSplineKind* kind, AFGSplineHologram* hologram );
	/** Visit every built actor of this type; return false from the visitor to stop. */
	static void KindForEachBuildable( const FSplineKind* kind, UWorld* world, TFunctionRef< bool( AFGBuildable* ) > visit );
	static UFGConnectionComponent* KindConnection( const FSplineKind* kind, AFGBuildable* buildable, int32 index );
	static FVector KindConnectorLocation( const FSplineKind* kind, UFGConnectionComponent* connection );

	// --- hooks (one set per hologram class, all funnelled here) --------------------------------

	/** @param bRequireDragStep  true while validating (the player is still dragging); false at build time. */
	static bool IsChainCandidate( AFGSplineHologram* hologram, const FSplineKind* kind, bool bRequireDragStep );
	static void OnCheckValidPlacement( AFGSplineHologram* self );
	static void OnAfterCheckValidPlacement( AFGSplineHologram* self );
	static void OnPlacementHit( AFGSplineHologram* self, const FHitResult& hit );
	/** The player's click was accepted as the final placement step; vanilla serialises and sends next. */
	static void OnAfterMultiStepPlacement( bool bFinalStep, AFGSplineHologram* self );
	/** The client turns a hologram into a construction message (also runs on loading; fallback trigger). */
	static void OnSerializeConstructMessage( IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id );
	/** The server refused a construction message: if it is the piece being waited on, stop the chain. */
	static void OnBuildableFailedConstruction( UFGBuildGunStateBuild* self, FNetConstructionID id );

	// --- chain driver --------------------------------------------------------------------------

	/** Reshape a chainable hologram into piece 0 and start the driver, once. */
	static void StartChain( AFGSplineHologram* hologram, const FSplineKind* kind, const TCHAR* trigger );
	/** The build gun's build state for the player who owns the hologram. */
	static UFGBuildGunStateBuild* FindBuildState( AFGHologram* hologram );
	/** Cut the full curve into equal pieces under the limit. Leaves the hologram on piece 0. */
	static bool PlanChain( AFGSplineHologram* hologram, const FSplineKind* kind, UFGBuildGunStateBuild* buildState );
	/** Replace the hologram's spline data and rebuild its component. */
	static void SetSplineData( AFGSplineHologram* hologram, const FSplineKind* kind, TArray< FSplinePointData >&& localPoints );
	/** Put a world-space piece on a hologram, in the hologram's current transform. */
	static void ApplyPiece( AFGSplineHologram* hologram, const FSplineKind* kind, const TArray< FSplinePointData >& worldPiece );
	static void ToWorld( const FTransform& frame, TArray< FSplinePointData >& points );
	/** Move the splitter's second half (relative to the split point) onto the split point. */
	static void MakeContinuous( TArray< FSplinePointData >& piece, const FVector& expectedStart, const TCHAR* what );
	static void SnapshotBuildables( FChainPlan& plan, UWorld* world );
	/** Configure the build gun's current hologram as the next piece and have the build gun build it. */
	static void SendNextPiece();
	static bool TickChain( float deltaTime );
	static void AbortChain( const TCHAR* reason );
	static void ValidateForConstruction( AFGHologram* hologram );
	static FString DisqualifierNames( AFGHologram* hologram );

	// --- end supports --------------------------------------------------------------------------

	/**
	 * Seat the end pole on the floor below the spline end, or disable it if there is none in reach.
	 * May nudge the last spline point vertically and re-trim; returns the final spline length.
	 * Wall / ceiling mounts in `supports` are always disabled: a joint is nowhere near a wall.
	 */
	static float SeatOrDisableEndPole( AFGSplineHologram* hologram, const FSplineKind* kind, float cutDistance,
		const FEndSupports& supports, const TCHAR* debugName );

	/** Disable a support hologram and remember that it was us, not vanilla, who did it. */
	static void DisableSupport( AFGHologram* support );
	/** Re-enable any of the given supports that we disabled earlier. */
	static void RestoreSupports( const FEndSupports& supports );
	/** Tallest height the pole's buildable class can be built at. */
	static float GetMaxPoleHeight( const AFGPoleHologram* pole );

	/** True while the chain driver is configuring or constructing a piece; the placement hooks stand aside. */
	static bool sDriverBusy;
	/** Supports we switched off. Vanilla may legitimately disable the same holograms (snapping), so we only ever restore our own. */
	static TSet< TWeakObjectPtr< AFGHologram > > sDisabledByUs;
	/** Last cursor hit each hologram was placed with; the chain's final piece is placed with it again. */
	static TMap< TWeakObjectPtr< AFGHologram >, FHitResult > sLastHit;
	static TUniquePtr< FChainPlan > sChain;
	static FTSTicker::FDelegateHandle sTicker;
};
