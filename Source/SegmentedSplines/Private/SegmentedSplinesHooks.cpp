#include "SegmentedSplinesHooks.h"
#include "SegmentedSplines.h"

#include "Patching/NativeHookManager.h"

#include "Hologram/FGSplineHologram.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGRailroadTrackHologram.h"
#include "Hologram/FGPipelineHologram.h"
#include "Hologram/FGConveyorPoleHologram.h"
#include "Hologram/FGPipelinePoleHologram.h"
#include "Hologram/FGWallAttachmentHologram.h"
#include "Hologram/HologramHelpers.h"
#include "Buildables/FGBuildablePole.h"

#include "Components/SplineComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

TSet< TWeakObjectPtr< AFGHologram > > FSegmentedSplinesHooks::sDisabledByUs;

// The build gun's trace channel. FactoryGame.h declares TC_BuildGun but the SML header stubs never
// define it, so it does not link; the value comes from Config/DefaultEngine.ini ("BuildGun").
static constexpr ECollisionChannel kBuildGunTraceChannel = ECC_GameTraceChannel5;

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------

void FSegmentedSplinesHooks::RegisterHooks()
{
	// SUBSCRIBE_UOBJECT_METHOD patches the vtable slot of the given native class (via its CDO).
	// Blueprint subclasses (the actual per-tier belt holograms) share that native vtable, so one
	// hook per C++ hologram class covers every vanilla recipe. These are *pre* hooks: we trim the
	// spline, then let vanilla's CheckValidPlacement run on the trimmed data.
	//
	// CheckValidPlacement is protected, which is why this class is declared a friend in
	// Config/AccessTransformers.ini: taking &AFGConveyorBeltHologram::CheckValidPlacement is only
	// legal from inside a friend.

	SUBSCRIBE_UOBJECT_METHOD( AFGConveyorBeltHologram, CheckValidPlacement,
		[]( auto& scope, AFGConveyorBeltHologram* self )
		{
			OnBeltCheckValidPlacement( self );
		} );

	SUBSCRIBE_UOBJECT_METHOD( AFGRailroadTrackHologram, CheckValidPlacement,
		[]( auto& scope, AFGRailroadTrackHologram* self )
		{
			OnTrackCheckValidPlacement( self );
		} );

	// AFGPipelineHologram is used for fluid pipes. Hypertubes are expected to share it (both
	// buildables derive from AFGBuildablePipeBase) but that is unverified: confirm in the editor by
	// opening the hypertube's hologram class, and if it is a different class add a hook for it.
	SUBSCRIBE_UOBJECT_METHOD( AFGPipelineHologram, CheckValidPlacement,
		[]( auto& scope, AFGPipelineHologram* self )
		{
			OnPipelineCheckValidPlacement( self );
		} );

	UE_LOG( LogSegmentedSplines, Log, TEXT( "Spline hologram hooks registered" ) );
}

// ---------------------------------------------------------------------------------------------
// Per-class entry points
// ---------------------------------------------------------------------------------------------

void FSegmentedSplinesHooks::OnBeltCheckValidPlacement( AFGConveyorBeltHologram* self )
{
	if( !self ) return;
	// Index 1 of each child array is the far end. Vanilla activates exactly one of the three
	// depending on what the cursor hit (floor, wall, ceiling); after a trim only the ground pole
	// can make sense, so the mounts are always disabled.
	FEndSupports supports;
	supports.Pole = self->mChildPoleHologram[ 1 ].Get();
	supports.Mounts.Add( self->mChildWallPoleHologram[ 1 ].Get() );
	supports.Mounts.Add( self->mChildCeilingPoleHologram[ 1 ].Get() );
	TruncateIfTooLong( self, self->mMaxSplineLength, supports, TEXT( "belt" ) );
}

void FSegmentedSplinesHooks::OnTrackCheckValidPlacement( AFGRailroadTrackHologram* self )
{
	if( !self ) return;
	// Rails have no end pole. mMaxLength is set per-recipe in the hologram's Blueprint defaults.
	TruncateIfTooLong( self, self->mMaxLength, FEndSupports(), TEXT( "track" ) );
}

void FSegmentedSplinesHooks::OnPipelineCheckValidPlacement( AFGPipelineHologram* self )
{
	if( !self ) return;
	FEndSupports supports;
	supports.Pole = self->mChildPoleHologram[ 1 ].Get();
	supports.Mounts.Add( self->mChildWallPoleHologram[ 1 ].Get() );
	TruncateIfTooLong( self, self->mMaxSplineLength, supports, TEXT( "pipeline" ) );
}

// ---------------------------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------------------------

bool FSegmentedSplinesHooks::TruncateIfTooLong( AFGSplineHologram* hologram, float maxLength, const FEndSupports& supports, const TCHAR* debugName )
{
	if( !hologram || maxLength <= 0.f )
	{
		return false;
	}

	// Only the player's own hologram. On a listen host the host's hologram is local too, which is fine.
	if( !hologram->IsLocalHologram() )
	{
		return false;
	}

	// Only act while the player is dragging out the far end. Before that there is no spline to
	// speak of, and after that (adjusting the end pole) the length is already settled. A support
	// we disabled must stay disabled through the pole-adjust step and construction, so nothing is
	// restored here.
	if( hologram->GetCurrentBuildStep() != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding )
	{
		return false;
	}

	// If the far end is snapped to a connection (a machine input, another belt...), the player is
	// clearly aiming at that connection. Cutting the belt short would leave it floating next to
	// the target, so keep vanilla's "too long" behaviour in that case. Vanilla disables the pole
	// itself while snapped, so we must not touch it either.
	if( hologram->IsConnectionSnapped( true ) )
	{
		return false;
	}

	USplineComponent* spline = hologram->mSplineComponent;
	if( !spline || hologram->mSplineData.Num() < 2 )
	{
		return false;
	}

	const float currentLength = spline->GetSplineLength();
	if( currentLength <= maxLength )
	{
		// Back within the limit: the player gets vanilla's own support choice again.
		RestoreSupports( supports );
		return false;
	}

	const float cutDistance = FMath::Max( maxLength - kSafetyMargin, 0.f );

	// Remember where the end used to be, and which way it pointed, before we move it. The pole's
	// relation to the old end (top offset, facing) is vanilla's own and is carried over to the new end.
	const FTransform hologramTransform = hologram->GetActorTransform();
	const FVector oldEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );
	const FVector oldEndDir = hologramTransform.TransformVector( hologram->mSplineData.Last().ArriveTangent ).GetSafeNormal2D();
	const float oldEndYaw = oldEndDir.IsNearlyZero() ? 0.f : oldEndDir.Rotation().Yaw;

	// The game ships a tangent-preserving splitter (used for belt welding / blueprint splitting).
	// It gives back the two halves as local-space point data; we keep the first and drop the rest.
	// This is what preserves the curve the player aimed at the far destination: we are not
	// re-routing a shorter spline, we are cutting the long one.
	TArray< FSplinePointData > firstHalf;
	TArray< FSplinePointData > secondHalf;
	FSplineUtils::SplitSplineAtDistance( spline, cutDistance, firstHalf, secondHalf );

	if( firstHalf.Num() < 2 )
	{
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[%s] SplitSplineAtDistance returned %d points, leaving hologram alone" ), debugName, firstHalf.Num() );
		return false;
	}

	hologram->mSplineData = MoveTemp( firstHalf );
	// Rebuild the visual spline + spline meshes + clearance from the trimmed data, exactly as the
	// hologram does after routing. Vanilla's CheckValidPlacement (which runs right after this hook)
	// measures mSplineComponent, so it must be rebuilt before we return.
	hologram->UpdateSplineComponent();

	float finalLength = hologram->mSplineComponent->GetSplineLength();
	if( supports.Pole )
	{
		finalLength = SeatOrDisableEndPole( hologram, cutDistance, supports, oldEndWorld, oldEndYaw, debugName );
	}

	UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] trimmed spline from %.0f cm to %.0f cm (limit %.0f)" ),
		debugName, currentLength, finalLength, maxLength );
	return true;
}

float FSegmentedSplinesHooks::SeatOrDisableEndPole( AFGSplineHologram* hologram, float cutDistance, const FEndSupports& supports,
	const FVector& oldEndWorld, float oldEndYaw, const TCHAR* debugName )
{
	AFGPoleHologram* pole = supports.Pole;
	USplineComponent* spline = hologram->mSplineComponent;
	const FTransform hologramTransform = hologram->GetActorTransform();

	// Wall / ceiling mounts belong to the cursor's surface, which the cut end is nowhere near.
	for( AFGHologram* mount : supports.Mounts )
	{
		if( mount && !mount->IsDisabled() )
		{
			DisableSupport( mount );
		}
	}

	FVector newEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );

	// Vertical gap between the spline end and the pole top as vanilla placed them. Normally ~0, but
	// read it from the live hologram rather than assuming. Only meaningful while the pole is active;
	// if vanilla had switched to a wall mount the ground pole is stale, so fall back to 0.
	float topOffset = 0.f;
	if( !pole->IsDisabled() )
	{
		topOffset = FMath::Clamp( oldEndWorld.Z - ( pole->GetActorLocation().Z + pole->GetPoleHeight() ), -50.f, 50.f );
	}

	const float maxPoleHeight = GetMaxPoleHeight( pole );

	// Straight down from the cut end, as far as the tallest pole could reach. Same channel as the
	// build gun, so we see what the player could have aimed at. The hologram, its children and the
	// player are not floors.
	FCollisionQueryParams params( SCENE_QUERY_STAT( SegmentedSplinesFloor ), false );
	params.AddIgnoredActor( hologram );
	for( AFGHologram* child : hologram->GetHologramChildren() )
	{
		if( child ) params.AddIgnoredActor( child );
	}
	if( APawn* instigator = hologram->GetConstructionInstigator() )
	{
		params.AddIgnoredActor( instigator );
	}

	const FVector traceStart = newEndWorld + FVector( 0.f, 0.f, kTraceStartLift );
	const FVector traceEnd = newEndWorld - FVector( 0.f, 0.f, maxPoleHeight + kMaxPoleSeatNudge + FMath::Abs( topOffset ) );

	FHitResult hit;
	UWorld* world = hologram->GetWorld();
	const bool hasFloor = world
		&& world->LineTraceSingleByChannel( hit, traceStart, traceEnd, kBuildGunTraceChannel, params )
		&& hit.ImpactNormal.Z >= kMinFloorNormalZ;

	if( !hasFloor )
	{
		if( !pole->IsDisabled() )
		{
			DisableSupport( pole );
		}
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] no floor within %.0f cm below the cut end, end left free (pole disabled)" ),
			debugName, maxPoleHeight );
		return spline->GetSplineLength();
	}

	// Ask for the exact height; the pole picks its closest fixed variation and reports what it got.
	const float desiredHeight = ( newEndWorld.Z - topOffset ) - hit.ImpactPoint.Z;
	pole->SetPoleHeight( desiredHeight );
	const float actualHeight = pole->GetPoleHeight();
	const float mismatch = ( hit.ImpactPoint.Z + actualHeight + topOffset ) - newEndWorld.Z;

	if( FMath::Abs( mismatch ) > kMaxPoleSeatNudge )
	{
		if( !pole->IsDisabled() )
		{
			DisableSupport( pole );
		}
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] floor %.0f cm below the cut end but closest pole height is %.0f, end left free (pole disabled)" ),
			debugName, desiredHeight, actualHeight );
		return spline->GetSplineLength();
	}

	// Seat the pole: on the floor, under the end, turned by however much the end direction changed.
	if( sDisabledByUs.Contains( pole ) )
	{
		pole->SetDisabled( false );
		sDisabledByUs.Remove( pole );
	}
	const FVector newEndDir = hologramTransform.TransformVector( hologram->mSplineData.Last().ArriveTangent ).GetSafeNormal2D();
	FRotator poleRotation = pole->GetActorRotation();
	if( !newEndDir.IsNearlyZero() )
	{
		poleRotation.Yaw += newEndDir.Rotation().Yaw - oldEndYaw;
	}
	pole->SetActorLocationAndRotation( hit.ImpactPoint, poleRotation );

	// Put the spline end on the pole top. Vanilla routes the belt to the pole's connection, which
	// is what makes the two meet; a trimmed end has to be moved there by hand.
	if( !FMath::IsNearlyZero( mismatch, 0.5f ) )
	{
		hologram->mSplineData.Last().Location += hologramTransform.InverseTransformVector( FVector( 0.f, 0.f, mismatch ) );
		hologram->UpdateSplineComponent();

		// The nudge lengthened the curve slightly. If that pushed it back over the limit, cut
		// once more; the end moves a few cm back along the curve, so keep the pole under it.
		if( spline->GetSplineLength() > cutDistance + kSafetyMargin )
		{
			TArray< FSplinePointData > firstHalf;
			TArray< FSplinePointData > secondHalf;
			FSplineUtils::SplitSplineAtDistance( spline, cutDistance, firstHalf, secondHalf );
			if( firstHalf.Num() >= 2 )
			{
				hologram->mSplineData = MoveTemp( firstHalf );
				hologram->UpdateSplineComponent();
				newEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );
				pole->SetActorLocation( FVector( newEndWorld.X, newEndWorld.Y, hit.ImpactPoint.Z ) );
			}
		}
	}

	UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] pole seated on floor %.0f cm below the cut end (height %.0f, nudged end by %.1f cm)" ),
		debugName, desiredHeight, actualHeight, mismatch );
	return spline->GetSplineLength();
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

void FSegmentedSplinesHooks::DisableSupport( AFGHologram* support )
{
	support->SetDisabled( true );
	sDisabledByUs.Add( support );
}

void FSegmentedSplinesHooks::RestoreSupports( const FEndSupports& supports )
{
	if( sDisabledByUs.Num() == 0 )
	{
		return;
	}
	auto restore = [] ( AFGHologram* support )
	{
		if( support && sDisabledByUs.Remove( support ) > 0 )
		{
			support->SetDisabled( false );
		}
	};
	restore( supports.Pole );
	for( AFGHologram* mount : supports.Mounts )
	{
		restore( mount );
	}
	// Holograms that were destroyed drop out on their own.
	for( auto it = sDisabledByUs.CreateIterator(); it; ++it )
	{
		if( !it->IsValid() ) it.RemoveCurrent();
	}
}

float FSegmentedSplinesHooks::GetMaxPoleHeight( const AFGPoleHologram* pole )
{
	float maxHeight = 0.f;
	if( UClass* buildClass = pole->GetBuildClass() )
	{
		if( const AFGBuildablePole* cdo = Cast< AFGBuildablePole >( buildClass->GetDefaultObject() ) )
		{
			for( const FPoleVariation& variation : cdo->GetPoleVariations() )
			{
				maxHeight = FMath::Max( maxHeight, variation.Height );
			}
		}
	}
	return maxHeight > 0.f ? maxHeight : kFallbackMaxPoleHeight;
}
