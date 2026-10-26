#include "SegmentedSplinesHooks.h"
#include "SegmentedSplines.h"

#include "Patching/NativeHookManager.h"

#include "Hologram/FGSplineHologram.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGRailroadTrackHologram.h"
#include "Hologram/FGPipelineHologram.h"
#include "Hologram/FGConveyorPoleHologram.h"
#include "Hologram/FGPipelinePoleHologram.h"
#include "Hologram/HologramHelpers.h"

#include "Components/SplineComponent.h"

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
	// mChildPoleHologram[1] is the pole vanilla auto-places under the free end of a belt.
	// (Wall/ceiling pole variants exist too; see TruncateIfTooLong for the current limitation.)
	TruncateIfTooLong( self, self->mMaxSplineLength, self->mChildPoleHologram[ 1 ].Get(), TEXT( "belt" ) );
}

void FSegmentedSplinesHooks::OnTrackCheckValidPlacement( AFGRailroadTrackHologram* self )
{
	if( !self ) return;
	// Rails have no end pole. mMaxLength is set per-recipe in the hologram's Blueprint defaults.
	TruncateIfTooLong( self, self->mMaxLength, nullptr, TEXT( "track" ) );
}

void FSegmentedSplinesHooks::OnPipelineCheckValidPlacement( AFGPipelineHologram* self )
{
	if( !self ) return;
	TruncateIfTooLong( self, self->mMaxSplineLength, self->mChildPoleHologram[ 1 ].Get(), TEXT( "pipeline" ) );
}

// ---------------------------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------------------------

bool FSegmentedSplinesHooks::TruncateIfTooLong( AFGSplineHologram* hologram, float maxLength, AActor* endPoleHologram, const TCHAR* debugName )
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
	// speak of, and after that (adjusting the end pole) the length is already settled.
	if( hologram->GetCurrentBuildStep() != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding )
	{
		return false;
	}

	// If the far end is snapped to a connection (a machine input, another belt...), the player is
	// clearly aiming at that connection. Cutting the belt short would leave it floating next to
	// the target, so keep vanilla's "too long" behaviour in that case.
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
		return false;
	}

	const float cutDistance = FMath::Max( maxLength - kSafetyMargin, 0.f );

	// Remember where the end used to be so we can shift the end pole by the same offset.
	const FTransform hologramTransform = hologram->GetActorTransform();
	const FVector oldEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );

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

	// Keep the end pole under the new end. Vanilla positioned it under the cursor, i.e. under the
	// old end. We move it by the same delta the belt end moved by.
	//
	// KNOWN LIMITATION (phase 1 draft): this ignores terrain height at the new end point and does
	// not re-aim the pole along the new end tangent. The proper fix is to line-trace down from the
	// new end and run the pole hologram's own placement with that hit, then re-anchor the belt end
	// to the pole's snap connection; do that once the basic cut is proven in-game.
	if( endPoleHologram )
	{
		const FVector newEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );
		const FVector delta = newEndWorld - oldEndWorld;
		if( !delta.IsNearlyZero() )
		{
			endPoleHologram->AddActorWorldOffset( delta );
		}
	}

	UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] trimmed spline from %.0f cm to %.0f cm (limit %.0f)" ),
		debugName, currentLength, hologram->mSplineComponent->GetSplineLength(), maxLength );
	return true;
}
