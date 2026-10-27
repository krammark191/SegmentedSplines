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
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGFactoryConnectionComponent.h"
#include "FGConstructionMessageInterface.h"

#include "Components/SplineComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

TSet< TWeakObjectPtr< AFGHologram > > FSegmentedSplinesHooks::sDisabledByUs;
TMap< TWeakObjectPtr< AFGHologram >, FHitResult > FSegmentedSplinesHooks::sLastHit;
TUniquePtr< FSegmentedSplinesHooks::FChainPlan > FSegmentedSplinesHooks::sChain;
FTSTicker::FDelegateHandle FSegmentedSplinesHooks::sTicker;
bool FSegmentedSplinesHooks::sDriverBusy = false;

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
	// hook per C++ hologram class covers every vanilla recipe.
	//
	// CheckValidPlacement is protected, which is why this class is declared a friend in
	// Config/AccessTransformers.ini: taking &AFGConveyorBeltHologram::CheckValidPlacement is only
	// legal from inside a friend.

	// Belts: chain. Before vanilla validates we only bookkeep; after it validates we strike the
	// "too long" verdict for a chainable hologram.
	SUBSCRIBE_UOBJECT_METHOD( AFGConveyorBeltHologram, CheckValidPlacement,
		[]( auto& scope, AFGConveyorBeltHologram* self )
		{
			OnBeltCheckValidPlacement( self );
		} );
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGConveyorBeltHologram, CheckValidPlacement,
		[]( AFGConveyorBeltHologram* self )
		{
			OnBeltAfterCheckValidPlacement( self );
		} );

	// Remember what the cursor hit when the belt's far end was placed. The build gun tries
	// snapping first and only falls through to free placement if that fails, so both are hooked.
	SUBSCRIBE_UOBJECT_METHOD( AFGConveyorBeltHologram, TrySnapToActor,
		[]( auto& scope, AFGConveyorBeltHologram* self, const FHitResult& hitResult )
		{
			OnBeltPlacementHit( self, hitResult );
		} );
	SUBSCRIBE_UOBJECT_METHOD( AFGConveyorBeltHologram, SetHologramLocationAndRotation,
		[]( auto& scope, AFGConveyorBeltHologram* self, const FHitResult& hitResult )
		{
			OnBeltPlacementHit( self, hitResult );
		} );

	// The moment the player builds. DoMultiStepPlacement returning true is the build gun accepting
	// the click as the final step; it serialises and sends the hologram right after. For a chainable
	// belt the hologram is reshaped into piece 0 in between, and the driver takes over the rest.
	SUBSCRIBE_UOBJECT_METHOD_AFTER( AFGConveyorBeltHologram, DoMultiStepPlacement,
		[]( bool bFinalStep, AFGConveyorBeltHologram* self, bool isInputFromARelease )
		{
			OnBeltAfterMultiStepPlacement( bFinalStep, self );
		} );

	// Fallback trigger and diagnostics: the serialisation itself. The function is introduced by
	// IFGConstructionMessageInterface, so it is hooked through that type: the implementation is
	// entered with the interface subobject as `this`, and a handler typed on AFGHologram would be
	// handed a pointer 0x2b0 bytes into the object (SML logs that adjustment at install).
	SUBSCRIBE_METHOD_VIRTUAL( IFGConstructionMessageInterface::SerializeConstructMessage,
		static_cast< const IFGConstructionMessageInterface* >( GetDefault< AFGConveyorBeltHologram >() ),
		[]( auto& scope, IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id )
		{
			OnSerializeConstructMessage( self, ar, id );
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
// Belts: chain
// ---------------------------------------------------------------------------------------------

FSegmentedSplinesHooks::FEndSupports FSegmentedSplinesHooks::GetBeltSupports( AFGConveyorBeltHologram* belt, int32 endIndex )
{
	// Vanilla activates exactly one of the three per end depending on what the cursor hit
	// (floor, wall, ceiling).
	FEndSupports supports;
	supports.Pole = belt->mChildPoleHologram[ endIndex ].Get();
	supports.Mounts.Add( belt->mChildWallPoleHologram[ endIndex ].Get() );
	supports.Mounts.Add( belt->mChildCeilingPoleHologram[ endIndex ].Get() );
	return supports;
}

bool FSegmentedSplinesHooks::IsChainCandidate( AFGConveyorBeltHologram* belt, bool bRequireDragStep )
{
	if( !belt || sDriverBusy || !belt->IsLocalHologram() )
	{
		return false;
	}
	const ESplineHologramBuildStep step = belt->GetCurrentBuildStep();
	if( bRequireDragStep && step != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding && step != ESplineHologramBuildStep::SHBS_AdjustPole )
	{
		return false;
	}
	if( !belt->mSplineComponent || belt->mSplineData.Num() < 2 || belt->mMaxSplineLength <= 0.f )
	{
		return false;
	}
	const float length = belt->mSplineComponent->GetSplineLength();
	if( length <= belt->mMaxSplineLength )
	{
		return false;
	}
	const int32 pieces = FMath::CeilToInt( length / ( belt->mMaxSplineLength - kSafetyMargin ) );
	return pieces <= kMaxChainPieces;
}

void FSegmentedSplinesHooks::OnBeltCheckValidPlacement( AFGConveyorBeltHologram* self )
{
	if( !self || sDriverBusy || !self->IsLocalHologram() )
	{
		return;
	}
	// A belt back within the limit after a chain preview gets vanilla's own support choice again.
	if( self->GetCurrentBuildStep() == ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding
		&& self->mSplineComponent && self->mSplineComponent->GetSplineLength() <= self->mMaxSplineLength )
	{
		RestoreSupports( GetBeltSupports( self, 1 ) );
	}
}

void FSegmentedSplinesHooks::OnBeltAfterCheckValidPlacement( AFGConveyorBeltHologram* self )
{
	if( !IsChainCandidate( self, true ) )
	{
		return;
	}
	// Vanilla has just judged the full curve too long. Every piece we will send is under the limit,
	// so that verdict does not apply; everything else it found (clearance, cost, snapping) stands.
	const int32 removed = self->mConstructDisqualifiers.Remove( UFGCDConveyorTooLong::StaticClass() );
	if( removed > 0 )
	{
		UE_LOG( LogSegmentedSplines, VeryVerbose, TEXT( "[belt] %.0f cm hologram over the %.0f limit, chainable: cleared TooLong" ),
			self->mSplineComponent->GetSplineLength(), self->mMaxSplineLength );
	}
}

void FSegmentedSplinesHooks::OnBeltPlacementHit( AFGConveyorBeltHologram* self, const FHitResult& hit )
{
	if( self && !sDriverBusy && self->IsLocalHologram() )
	{
		sLastHit.FindOrAdd( self ) = hit;
	}
}

void FSegmentedSplinesHooks::OnBeltAfterMultiStepPlacement( bool bFinalStep, AFGConveyorBeltHologram* self )
{
	if( !self || !self->IsLocalHologram() )
	{
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[diag] DoMultiStepPlacement -> %s, step %d, length %.0f (limit %.0f)%s" ),
		bFinalStep ? TEXT( "final" ) : TEXT( "more steps" ), static_cast< int32 >( self->GetCurrentBuildStep() ),
		self->mSplineComponent ? self->mSplineComponent->GetSplineLength() : -1.f, self->mMaxSplineLength,
		sDriverBusy ? TEXT( " [driver]" ) : TEXT( "" ) );
	if( bFinalStep && !sDriverBusy )
	{
		StartChain( self, TEXT( "DoMultiStepPlacement" ) );
	}
}

void FSegmentedSplinesHooks::OnSerializeConstructMessage( IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id )
{
	AFGConveyorBeltHologram* belt = Cast< AFGConveyorBeltHologram >( self );
	if( !belt )
	{
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[diag] SerializeConstructMessage %s id=%s, step %d, length %.0f, local %d, driver busy %d" ),
		ar.IsSaving() ? TEXT( "saving" ) : TEXT( "loading" ), *id.ToString(), static_cast< int32 >( belt->GetCurrentBuildStep() ),
		belt->mSplineComponent ? belt->mSplineComponent->GetSplineLength() : -1.f, belt->IsLocalHologram() ? 1 : 0, sDriverBusy ? 1 : 0 );
	// Loading happens on the server and on the client's pending-construction copy; neither is ours.
	if( ar.IsSaving() && !sDriverBusy )
	{
		StartChain( belt, TEXT( "SerializeConstructMessage" ) );
	}
	// The client serialises twice per build: once with an empty ID to make its pending copy, then
	// with the real ID for the server. The built belt replicates back carrying that ID.
	if( ar.IsSaving() && sChain && id.NetPlayerID >= 0 )
	{
		sChain->PendingId = id;
		sChain->bHasPendingId = true;
		UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] piece %d sent as %s" ), sChain->NextPiece, *id.ToString() );
	}
}

void FSegmentedSplinesHooks::StartChain( AFGConveyorBeltHologram* belt, const TCHAR* trigger )
{
	// At build time the step no longer matters; only the length does. Once piece 0 is in place
	// the belt is under the limit, so a second trigger for the same build is a no-op.
	if( !IsChainCandidate( belt, false ) )
	{
		return;
	}
	if( sChain )
	{
		AbortChain( TEXT( "a new build started while a chain was still running" ) );
	}
	UFGBuildGunStateBuild* buildState = FindBuildState( belt );
	if( !buildState )
	{
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] could not find the build gun's build state; sending the full belt as vanilla would" ) );
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] build accepted (%s), reshaping into piece 1" ), trigger );
	// While the hologram is reshaped and re-validated the placement hooks stand aside.
	sDriverBusy = true;
	PlanChain( belt, buildState );
	sDriverBusy = false;
}

UFGBuildGunStateBuild* FSegmentedSplinesHooks::FindBuildState( AFGHologram* hologram )
{
	AFGCharacterPlayer* player = Cast< AFGCharacterPlayer >( hologram->GetConstructionInstigator() );
	AFGBuildGun* buildGun = player ? player->GetBuildGun() : nullptr;
	return buildGun ? Cast< UFGBuildGunStateBuild >( buildGun->GetBuildGunStateFor( EBuildGunState::BGS_BUILD ) ) : nullptr;
}

bool FSegmentedSplinesHooks::PlanChain( AFGConveyorBeltHologram* belt, UFGBuildGunStateBuild* buildState )
{
	USplineComponent* spline = belt->mSplineComponent;
	const float maxLength = belt->mMaxSplineLength;
	const float totalLength = spline->GetSplineLength();
	const int32 numPieces = FMath::CeilToInt( totalLength / ( maxLength - kSafetyMargin ) );
	const float pieceLength = totalLength / numPieces;

	TUniquePtr< FChainPlan > plan = MakeUnique< FChainPlan >();
	plan->BuildState = buildState;
	plan->World = belt->GetWorld();
	plan->Recipe = belt->GetRecipe();
	plan->Frame = belt->GetActorTransform();
	if( const FHitResult* hit = sLastHit.Find( belt ) )
	{
		plan->EndHit = *hit;
		plan->bHasEndHit = true;
	}

	// Cut the curve into equal pieces. The game's splitter works on a spline component, so the
	// hologram's own component is reused for each remainder and put back on piece 0 at the end.
	TArray< FSplinePointData > remaining = belt->mSplineData;
	for( int32 i = 0; i < numPieces - 1; ++i )
	{
		belt->mSplineData = remaining;
		belt->UpdateSplineComponent();
		TArray< FSplinePointData > first;
		TArray< FSplinePointData > rest;
		FSplineUtils::SplitSplineAtDistance( spline, pieceLength, first, rest );
		if( first.Num() < 2 || rest.Num() < 2 )
		{
			belt->mSplineData = remaining;
			belt->UpdateSplineComponent();
			UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] split %d of %d produced %d + %d points, building vanilla's way" ), i + 1, numPieces, first.Num(), rest.Num() );
			return false;
		}
		if( !MakeContinuous( rest, first.Last().Location, *FString::Printf( TEXT( "split %d" ), i + 1 ) ) )
		{
			belt->mSplineData = remaining;
			belt->UpdateSplineComponent();
			return false;
		}
		plan->Pieces.Add( MoveTemp( first ) );
		remaining = MoveTemp( rest );
	}
	plan->Pieces.Add( MoveTemp( remaining ) );
	for( TArray< FSplinePointData >& piece : plan->Pieces )
	{
		ToWorld( plan->Frame, piece );
	}
	for( int32 i = 0; i < plan->Pieces.Num(); ++i )
	{
		UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] piece %d: %d points, %s -> %s" ), i + 1, plan->Pieces[ i ].Num(),
			*plan->Pieces[ i ][ 0 ].Location.ToString(), *plan->Pieces[ i ].Last().Location.ToString() );
	}

	// Piece 0 keeps the player's start (its snap and start pole are already vanilla's) and gets a
	// joint at its far end: no snap, no mount, a pole only if there is a floor under it.
	ApplyPiece( belt, plan->Pieces[ 0 ] );
	belt->mSnappedConnectionComponents[ 1 ] = nullptr;
	belt->mSnappedWallPassthrough[ 1 ] = nullptr;
	const FVector endWorld = plan->Frame.TransformPosition( belt->mSplineData.Last().Location );
	SeatOrDisableEndPole( belt, maxLength - kSafetyMargin, GetBeltSupports( belt, 1 ), endWorld, 0.f, false, TEXT( "chain piece 1" ) );
	ValidateForConstruction( belt );
	if( !belt->CanConstruct() )
	{
		TArray< TSubclassOf< UFGConstructDisqualifier > > why;
		belt->GetConstructDisqualifiers( why );
		FString names;
		for( auto& d : why ) names += ( d ? d->GetName() : TEXT( "null" ) ) + TEXT( " " );
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] piece 1 of %d is not constructible (%s); building it anyway so vanilla reports why" ), numPieces, *names );
	}

	plan->NextPiece = 1;
	plan->bWaiting = true;
	plan->WaitStart = plan->Pieces[ 0 ][ 0 ].Location;
	plan->WaitEnd = plan->Pieces[ 0 ].Last().Location;
	plan->SentAt = FPlatformTime::Seconds();
	SnapshotBelts( *plan, belt->GetWorld() );
	sChain = MoveTemp( plan );

	if( !sTicker.IsValid() )
	{
		sTicker = FTSTicker::GetCoreTicker().AddTicker( FTickerDelegate::CreateStatic( &FSegmentedSplinesHooks::TickChain ) );
	}

	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] %.0f cm belt -> %d pieces of %.0f cm (limit %.0f); piece 1 goes with the player's build" ),
		totalLength, numPieces, pieceLength, maxLength );
	return true;
}

void FSegmentedSplinesHooks::ValidateForConstruction( AFGConveyorBeltHologram* belt )
{
	UFGInventoryComponent* inventory = nullptr;
	if( AFGCharacterPlayer* player = Cast< AFGCharacterPlayer >( belt->GetConstructionInstigator() ) )
	{
		inventory = player->GetInventory();
	}
	belt->ValidatePlacementAndCost( inventory );
}

void FSegmentedSplinesHooks::ToWorld( const FTransform& frame, TArray< FSplinePointData >& points )
{
	for( FSplinePointData& point : points )
	{
		point.Location = frame.TransformPosition( point.Location );
		point.ArriveTangent = frame.TransformVector( point.ArriveTangent );
		point.LeaveTangent = frame.TransformVector( point.LeaveTangent );
	}
}

bool FSegmentedSplinesHooks::MakeContinuous( TArray< FSplinePointData >& piece, const FVector& expectedStart, const TCHAR* what )
{
	// The splitter's second half is relative to the split point: its first point is at the local
	// origin (confirmed in-game). Move it so that it starts at the split point. It must never be
	// flipped on the strength of its end position: a straight belt cut in half has its relative
	// second half END exactly where the split point is in hologram space, and the ninth run's
	// 8000 cm belt got its piece 2 reversed back over piece 1 that way.
	const float startGap = FVector::Dist( piece[ 0 ].Location, expectedStart );
	if( startGap > kJointTolerance )
	{
		const FVector shift = expectedStart - piece[ 0 ].Location;
		for( FSplinePointData& point : piece )
		{
			point.Location += shift;
		}
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] %s: second half started %.0f cm from the split point, translated onto it" ), what, startGap );
	return true;
}

void FSegmentedSplinesHooks::ApplyPiece( AFGConveyorBeltHologram* hologram, const TArray< FSplinePointData >& worldPiece )
{
	const FTransform frame = hologram->GetActorTransform();
	hologram->mSplineData = worldPiece;
	for( FSplinePointData& point : hologram->mSplineData )
	{
		point.Location = frame.InverseTransformPosition( point.Location );
		point.ArriveTangent = frame.InverseTransformVector( point.ArriveTangent );
		point.LeaveTangent = frame.InverseTransformVector( point.LeaveTangent );
	}
	hologram->UpdateSplineComponent();
}

void FSegmentedSplinesHooks::SnapshotBelts( FChainPlan& plan, UWorld* world )
{
	plan.KnownBelts.Reset();
	if( !world ) return;
	for( TActorIterator< AFGBuildableConveyorBelt > it( world ); it; ++it )
	{
		plan.KnownBelts.Add( *it );
	}
}

bool FSegmentedSplinesHooks::TickChain( float deltaTime )
{
	if( !sChain )
	{
		sTicker.Reset();
		return false;
	}
	FChainPlan& plan = *sChain;
	UWorld* world = plan.World.Get();
	if( !world || !plan.BuildState.IsValid() )
	{
		AbortChain( TEXT( "world or build gun went away" ) );
		return false;
	}

	if( plan.bWaiting )
	{
		// The server builds the piece and replicates the belt back. It carries the construction ID
		// the client sent (replicated); failing that, match it by its ends.
		for( TActorIterator< AFGBuildableConveyorBelt > it( world ); it; ++it )
		{
			AFGBuildableConveyorBelt* candidate = *it;
			// Client IDs restart every launch, so a belt from an earlier session can carry the same
			// ID; only a belt that did not exist when the piece was sent can be the piece.
			if( plan.KnownBelts.Contains( candidate ) )
			{
				continue;
			}
			UFGFactoryConnectionComponent* c0 = candidate->GetConnection0();
			UFGFactoryConnectionComponent* c1 = candidate->GetConnection1();
			if( !c0 || !c1 )
			{
				continue;
			}
			// A belt stores its spline in flow direction (connection0 = input, connection1 = output).
			// When the player drags against the flow, the server reverses the points, so the joint can
			// be at either connector: take whichever sits at the expected far end.
			const float d1e = FVector::Dist( c1->GetConnectorLocation(), plan.WaitEnd );
			const float d0e = FVector::Dist( c0->GetConnectorLocation(), plan.WaitEnd );
			bool idMatch = false;
			if( plan.bHasPendingId )
			{
				const FNetConstructionID cid = candidate->GetNetConstructionID();
				idMatch = cid.NetPlayerID == plan.PendingId.NetPlayerID && cid.Client_ID == plan.PendingId.Client_ID;
			}
			UFGFactoryConnectionComponent* joint = nullptr;
			float jointDist = 0.f;
			if( d1e <= 4.f * kJointTolerance )       { joint = c1; jointDist = d1e; }
			else if( d0e <= 4.f * kJointTolerance )  { joint = c0; jointDist = d0e; }
			if( !joint )
			{
				UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] new belt %s ignored: neither connector at the expected joint (c1 %.0f cm, c0 %.0f cm away; id match %d)" ),
					*candidate->GetName(), d1e, d0e, idMatch ? 1 : 0 );
				continue;
			}
			plan.LastBelt = candidate;
			plan.JointConnection = joint;
			plan.bWaiting = false;
			UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] piece %d of %d arrived after %.0f ms: %s (id match %d, joint is %s, %.0f cm from expected)" ),
				plan.NextPiece, plan.Pieces.Num(), ( FPlatformTime::Seconds() - plan.SentAt ) * 1000.0, *candidate->GetName(), idMatch ? 1 : 0,
				joint == c1 ? TEXT( "connection1/output (drawn with the flow)" ) : TEXT( "connection0/input (drawn against the flow)" ), jointDist );
			break;
		}
		if( plan.bWaiting )
		{
			if( FPlatformTime::Seconds() - plan.SentAt > kPieceTimeout )
			{
				// Say what was there, so a miss explains itself.
				UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] expected piece start %s end %s, sent as %s; belts within 30 m of the start:" ),
					*plan.WaitStart.ToString(), *plan.WaitEnd.ToString(), plan.bHasPendingId ? *plan.PendingId.ToString() : TEXT( "(no id seen)" ) );
				int32 listed = 0;
				for( TActorIterator< AFGBuildableConveyorBelt > it( world ); it && listed < 8; ++it )
				{
					UFGFactoryConnectionComponent* c0 = it->GetConnection0();
					UFGFactoryConnectionComponent* c1 = it->GetConnection1();
					const FVector a = c0 ? c0->GetConnectorLocation() : it->GetActorLocation();
					const FVector b = c1 ? c1->GetConnectorLocation() : it->GetActorLocation();
					if( FVector::Dist( a, plan.WaitStart ) > 3000.f && FVector::Dist( b, plan.WaitStart ) > 3000.f )
					{
						continue;
					}
					UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain]   %s id=%s c0 %s (%.0f cm from start) c1 %s (%.0f cm from end)" ),
						*it->GetName(), *it->GetNetConstructionID().ToString(), *a.ToString(), FVector::Dist( a, plan.WaitStart ), *b.ToString(), FVector::Dist( b, plan.WaitEnd ) );
					++listed;
				}
				AbortChain( TEXT( "timed out waiting for the server to build a piece (rejected, or out of belts?)" ) );
				return false;
			}
			return true;
		}
	}

	if( plan.NextPiece >= plan.Pieces.Num() )
	{
		UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] done, %d pieces built" ), plan.Pieces.Num() );
		sChain.Reset();
		sTicker.Reset();
		return false;
	}

	SendNextPiece();
	return sChain.IsValid();
}

void FSegmentedSplinesHooks::SendNextPiece()
{
	FChainPlan& plan = *sChain;
	UFGBuildGunStateBuild* buildState = plan.BuildState.Get();
	AFGConveyorBeltHologram* holo = Cast< AFGConveyorBeltHologram >( buildState ? buildState->GetHologram() : nullptr );
	if( !holo )
	{
		AbortChain( TEXT( "the build gun no longer holds a belt hologram" ) );
		return;
	}
	if( holo->GetRecipe() != plan.Recipe )
	{
		AbortChain( TEXT( "the build gun switched recipe" ) );
		return;
	}
	AFGBuildableConveyorBelt* previous = plan.LastBelt.Get();
	UFGFactoryConnectionComponent* jointConnection = plan.JointConnection.Get();
	if( !previous || !jointConnection )
	{
		AbortChain( TEXT( "the previous piece vanished" ) );
		return;
	}

	const int32 i = plan.NextPiece;
	const bool bFinal = ( i == plan.Pieces.Num() - 1 );
	const float maxLength = holo->mMaxSplineLength;

	// The built belt's output is where this piece really starts. Move the piece's first point onto
	// it (the server may have adjusted the end by a few cm); anything larger means it is not our belt.
	{
		const FVector connector = jointConnection->GetConnectorLocation();
		const FVector delta = connector - plan.Pieces[ i ][ 0 ].Location;
		if( delta.Size() > 4.f * kJointTolerance )
		{
			AbortChain( *FString::Printf( TEXT( "previous belt's output is %.0f cm from where piece %d starts" ), delta.Size(), i + 1 ) );
			return;
		}
		plan.Pieces[ i ][ 0 ].Location = connector;
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] piece %d start moved %.1f cm onto the previous belt's output" ), i + 1, delta.Size() );
	}

	// From here the placement hooks stand aside: the hologram is ours until it is sent.
	sDriverBusy = true;
	plan.bHologramIsOurs = true;

	// Start: snapped to the previous belt's output, so the two connect. Vanilla hides the start
	// supports for a snapped start; this hologram never went through that step, so do it by hand.
	holo->SetActorTransform( plan.Frame );
	holo->mSnappedConnectionComponents[ 0 ] = jointConnection;
	holo->mSnappedWallPassthrough[ 0 ] = nullptr;
	// Flow direction follows from what the start is snapped to (an output feeds this piece, an
	// input is fed by it); this is vanilla's own routine for that.
	holo->SetupSnappedConnectionDirections();
	{
		FEndSupports startSupports = GetBeltSupports( holo, 0 );
		if( startSupports.Pole && !startSupports.Pole->IsDisabled() ) DisableSupport( startSupports.Pole );
		for( AFGHologram* mount : startSupports.Mounts )
		{
			if( mount && !mount->IsDisabled() ) DisableSupport( mount );
		}
	}
	holo->mBuildStep = ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding;

	if( bFinal && plan.bHasEndHit )
	{
		// The last piece ends where the player's cursor was. Let vanilla place that end from the
		// same hit so it chooses the support (pole, wall or ceiling mount) or snaps to the machine
		// exactly as it did for the original hologram; then put our piece's curve back under it.
		ApplyPiece( holo, plan.Pieces[ i ] );
		const bool snapped = holo->TrySnapToActor( plan.EndHit );
		if( !snapped )
		{
			holo->SetHologramLocationAndRotation( plan.EndHit );
		}
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] final piece end placed by vanilla (%s); hologram now at %s" ),
			snapped ? TEXT( "snapped" ) : TEXT( "free placement" ), *holo->GetActorLocation().ToString() );
	}
	else
	{
		holo->mSnappedConnectionComponents[ 1 ] = nullptr;
		holo->mSnappedWallPassthrough[ 1 ] = nullptr;
	}

	// Vanilla may have moved the hologram while placing the end; the piece is in world space, so
	// it lands in the same place regardless.
	ApplyPiece( holo, plan.Pieces[ i ] );

	if( !bFinal || !plan.bHasEndHit )
	{
		const FVector endWorld = plan.Pieces[ i ].Last().Location;
		SeatOrDisableEndPole( holo, maxLength - kSafetyMargin, GetBeltSupports( holo, 1 ), endWorld, 0.f, false, TEXT( "chain joint" ) );
	}

	ValidateForConstruction( holo );
	if( !holo->CanConstruct() )
	{
		TArray< TSubclassOf< UFGConstructDisqualifier > > why;
		holo->GetConstructDisqualifiers( why );
		FString names;
		for( auto& d : why ) names += ( d ? d->GetName() : TEXT( "null" ) ) + TEXT( " " );
		sDriverBusy = false;
		AbortChain( *FString::Printf( TEXT( "piece %d of %d is not constructible: %s" ), i + 1, plan.Pieces.Num(), *names ) );
		return;
	}

	// Hand the finished hologram to the build gun as if the player had clicked on the last step:
	// vanilla assigns the construction ID, serialises (our hook stands aside), sends, shows the
	// pending copy and spawns a fresh hologram for whatever comes next.
	holo->mBuildStep = ESplineHologramBuildStep::SHBS_AdjustPole;

	if( bFinal )
	{
		plan.NextPiece = plan.Pieces.Num();
		plan.bWaiting = false;
	}
	else
	{
		plan.NextPiece = i + 1;
		plan.bWaiting = true;
		plan.bHasPendingId = false;
		plan.WaitStart = plan.Pieces[ i ][ 0 ].Location;
		plan.WaitEnd = plan.Pieces[ i ].Last().Location;
		plan.SentAt = FPlatformTime::Seconds();
	}
	SnapshotBelts( plan, buildState->GetWorld() );

	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] sending piece %d of %d" ), i + 1, plan.Pieces.Num() );
	// The player's click reaches this function; PrimaryFire() itself does nothing when called from
	// code (seventh run: the hologram stayed, and the player's own click then built it as is).
	// At the pole-adjust step the FIRST step input is swallowed (DoMultiStepPlacement answers
	// "more steps" and stays on step 3, whether it is a press or a release: eighth and ninth
	// runs) and the SECOND one finishes the belt. A player's click is a press and a release,
	// which is why their click always built it. Send a release, then a press if needed.
	buildState->InternalExecuteDuBuildStepInput( true );
	if( sChain && buildState->GetHologram() == holo )
	{
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] first step input left the hologram in place (expected), sending the second" ) );
		buildState->InternalExecuteDuBuildStepInput( false );
	}
	sDriverBusy = false;
	plan.bHologramIsOurs = false;

	// A send replaces the build gun's hologram with a fresh one. If it is still ours, the build gun
	// did not accept the input as a final step and nothing went out. Leave the hologram as it is:
	// it is valid, and the player can click to build it by hand.
	if( sChain && buildState->GetHologram() == holo )
	{
		plan.bHologramIsOurs = false;
		AbortChain( TEXT( "the build gun did not construct the piece (hologram unchanged); it is left in place, a click builds it" ) );
	}
}

void FSegmentedSplinesHooks::AbortChain( const TCHAR* reason )
{
	if( sChain )
	{
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] stopped after %d of %d pieces: %s" ), sChain->NextPiece, sChain->Pieces.Num(), reason );
		// Do not leave the player holding a hologram we half-configured.
		if( sChain->bHologramIsOurs )
		{
			if( UFGBuildGunStateBuild* buildState = sChain->BuildState.Get() )
			{
				buildState->ResetHologram();
			}
		}
		sChain.Reset();
	}
	if( sTicker.IsValid() )
	{
		FTSTicker::GetCoreTicker().RemoveTicker( sTicker );
		sTicker.Reset();
	}
}

// ---------------------------------------------------------------------------------------------
// Tracks and pipes: cut at the limit
// ---------------------------------------------------------------------------------------------

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

	// If the far end is snapped to a connection (a machine input, another pipe...), the player is
	// clearly aiming at that connection. Cutting short would leave it floating next to the target,
	// so keep vanilla's "too long" behaviour in that case. Vanilla disables the pole itself while
	// snapped, so we must not touch it either.
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
		finalLength = SeatOrDisableEndPole( hologram, cutDistance, supports, oldEndWorld, oldEndYaw, true, debugName );
	}

	UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] trimmed spline from %.0f cm to %.0f cm (limit %.0f)" ),
		debugName, currentLength, finalLength, maxLength );
	return true;
}

// ---------------------------------------------------------------------------------------------
// End supports
// ---------------------------------------------------------------------------------------------

float FSegmentedSplinesHooks::SeatOrDisableEndPole( AFGSplineHologram* hologram, float cutDistance, const FEndSupports& supports,
	const FVector& oldEndWorld, float oldEndYaw, bool bPoleIsVanillaPlaced, const TCHAR* debugName )
{
	AFGPoleHologram* pole = supports.Pole;
	USplineComponent* spline = hologram->mSplineComponent;
	const FTransform hologramTransform = hologram->GetActorTransform();

	// Wall / ceiling mounts belong to the cursor's surface, which this end is nowhere near.
	for( AFGHologram* mount : supports.Mounts )
	{
		if( mount && !mount->IsDisabled() )
		{
			DisableSupport( mount );
		}
	}
	if( !pole )
	{
		return spline->GetSplineLength();
	}

	FVector newEndWorld = hologramTransform.TransformPosition( hologram->mSplineData.Last().Location );

	// Vertical gap between the spline end and the pole top as vanilla placed them. Normally ~0, but
	// read it from the live hologram rather than assuming. Only meaningful while the pole is active
	// and was placed by vanilla for this end; otherwise its state is stale and 0 is the safe guess.
	float topOffset = 0.f;
	if( bPoleIsVanillaPlaced && !pole->IsDisabled() )
	{
		topOffset = FMath::Clamp( oldEndWorld.Z - ( pole->GetActorLocation().Z + pole->GetPoleHeight() ), -50.f, 50.f );
	}

	const float maxPoleHeight = GetMaxPoleHeight( pole );

	// Straight down from the end, as far as the tallest pole could reach. Same channel as the
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
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] no floor within %.0f cm below the end, end left free (pole disabled)" ),
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
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] floor %.0f cm below the end but closest pole height is %.0f, end left free (pole disabled)" ),
			debugName, desiredHeight, actualHeight );
		return spline->GetSplineLength();
	}

	// Seat the pole: on the floor, under the end, facing along the end of the curve. For a pole
	// vanilla placed, keep its convention by turning it by however much the end direction changed.
	if( sDisabledByUs.Contains( pole ) )
	{
		pole->SetDisabled( false );
		sDisabledByUs.Remove( pole );
	}
	const FVector newEndDir = hologramTransform.TransformVector( hologram->mSplineData.Last().ArriveTangent ).GetSafeNormal2D();
	FRotator poleRotation = pole->GetActorRotation();
	if( !newEndDir.IsNearlyZero() )
	{
		const float newEndYaw = newEndDir.Rotation().Yaw;
		poleRotation.Yaw = bPoleIsVanillaPlaced ? poleRotation.Yaw + ( newEndYaw - oldEndYaw ) : newEndYaw;
	}
	pole->SetActorLocationAndRotation( hit.ImpactPoint, poleRotation );

	// Put the spline end on the pole top. Vanilla routes the belt to the pole's connection, which
	// is what makes the two meet; an end we placed has to be moved there by hand.
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

	UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[%s] pole seated on floor %.0f cm below the end (height %.0f, nudged end by %.1f cm)" ),
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
