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
#include "Buildables/FGBuildablePipeBase.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Buildables/FGBuildablePassthrough.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGConnectionComponent.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadTrackConnectionComponent.h"
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

static const FSegmentedSplinesHooks::FSplineKind kBeltKind  { FSegmentedSplinesHooks::ESplineKind::Belt,  TEXT( "belt" ),  true };
static const FSegmentedSplinesHooks::FSplineKind kPipeKind  { FSegmentedSplinesHooks::ESplineKind::Pipe,  TEXT( "pipe" ),  true };
static const FSegmentedSplinesHooks::FSplineKind kTrackKind { FSegmentedSplinesHooks::ESplineKind::Track, TEXT( "track" ), false };

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------

// One set of hooks per hologram class. SUBSCRIBE_UOBJECT_METHOD patches the vtable slot of the
// given native class (via its CDO); Blueprint subclasses (the actual per-tier holograms) share
// that native vtable, so one hook per C++ class covers every vanilla recipe. The hooked methods
// are protected, which is why this class is a friend of each hologram class in
// Config/AccessTransformers.ini: taking &Class::CheckValidPlacement is only legal from a friend.
#define SS_HOOK_SPLINE_HOLOGRAM( HologramClass ) \
	SUBSCRIBE_UOBJECT_METHOD( HologramClass, CheckValidPlacement, \
		[]( auto& scope, HologramClass* self ) { OnCheckValidPlacement( self ); } ); \
	SUBSCRIBE_UOBJECT_METHOD_AFTER( HologramClass, CheckValidPlacement, \
		[]( HologramClass* self ) { OnAfterCheckValidPlacement( self ); } ); \
	SUBSCRIBE_UOBJECT_METHOD( HologramClass, TrySnapToActor, \
		[]( auto& scope, HologramClass* self, const FHitResult& hitResult ) { OnPlacementHit( self, hitResult ); } ); \
	SUBSCRIBE_UOBJECT_METHOD( HologramClass, SetHologramLocationAndRotation, \
		[]( auto& scope, HologramClass* self, const FHitResult& hitResult ) { OnPlacementHit( self, hitResult ); } ); \
	SUBSCRIBE_UOBJECT_METHOD_AFTER( HologramClass, DoMultiStepPlacement, \
		[]( bool bFinalStep, HologramClass* self, bool isInputFromARelease ) { OnAfterMultiStepPlacement( bFinalStep, self ); } );

void FSegmentedSplinesHooks::RegisterHooks()
{
	// Before vanilla validates we only bookkeep; after it validates we strike the "too long"
	// verdict for a chainable hologram. The two placement calls remember what the cursor hit when
	// the far end was placed (the build gun tries snapping first and only falls through to free
	// placement if that fails, so both are hooked). DoMultiStepPlacement returning true is the
	// build gun accepting the click as the final step; it serialises and sends the hologram right
	// after, and for a chainable hologram it is reshaped into piece 0 in between.
	SS_HOOK_SPLINE_HOLOGRAM( AFGConveyorBeltHologram );
	SS_HOOK_SPLINE_HOLOGRAM( AFGPipelineHologram );       // fluid pipes and hypertubes
	SS_HOOK_SPLINE_HOLOGRAM( AFGRailroadTrackHologram );

	// Fallback trigger and diagnostics: the serialisation itself. The function is introduced by
	// IFGConstructionMessageInterface, so it is hooked through that type: the implementation is
	// entered with the interface subobject as `this`, and a handler typed on AFGHologram would be
	// handed a pointer 0x2b0 bytes into the object (SML logs that adjustment at install). None of
	// the spline holograms override it, so the belt CDO resolves to the one shared implementation.
	SUBSCRIBE_METHOD_VIRTUAL( IFGConstructionMessageInterface::SerializeConstructMessage,
		static_cast< const IFGConstructionMessageInterface* >( GetDefault< AFGConveyorBeltHologram >() ),
		[]( auto& scope, IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id )
		{
			OnSerializeConstructMessage( self, ar, id );
		} );

	// The server's answer to a refused construction message. Vanilla uses it to restore the
	// hologram; the chain uses it to stop at once instead of waiting out the timeout.
	SUBSCRIBE_UOBJECT_METHOD( UFGBuildGunStateBuild, Client_OnBuildableFailedConstruction_Implementation,
		[]( auto& scope, UFGBuildGunStateBuild* self, FNetConstructionID id )
		{
			OnBuildableFailedConstruction( self, id );
		} );

	UE_LOG( LogSegmentedSplines, Log, TEXT( "Spline hologram hooks registered (belts, pipes, tracks)" ) );
}

#undef SS_HOOK_SPLINE_HOLOGRAM

// ---------------------------------------------------------------------------------------------
// Per-type knowledge
// ---------------------------------------------------------------------------------------------

const FSegmentedSplinesHooks::FSplineKind* FSegmentedSplinesHooks::FindKind( const AFGHologram* hologram )
{
	if( !hologram ) return nullptr;
	if( hologram->IsA< AFGConveyorBeltHologram >() )  return &kBeltKind;
	if( hologram->IsA< AFGPipelineHologram >() )      return &kPipeKind;
	if( hologram->IsA< AFGRailroadTrackHologram >() ) return &kTrackKind;
	return nullptr;
}

UClass* FSegmentedSplinesHooks::KindTooLongDisqualifier( const FSplineKind* kind )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:  return UFGCDConveyorTooLong::StaticClass();
	case ESplineKind::Pipe:  return UFGCDPipeTooLong::StaticClass();
	case ESplineKind::Track: return UFGCDTrackTooLong::StaticClass();
	}
	return nullptr;
}

float FSegmentedSplinesHooks::KindMaxLength( const FSplineKind* kind, const AFGSplineHologram* hologram )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:  return static_cast< const AFGConveyorBeltHologram* >( hologram )->mMaxSplineLength;
	case ESplineKind::Pipe:  return static_cast< const AFGPipelineHologram* >( hologram )->mMaxSplineLength;
	// Set per recipe in the hologram's Blueprint defaults (no C++ default).
	case ESplineKind::Track: return static_cast< const AFGRailroadTrackHologram* >( hologram )->mMaxLength;
	}
	return 0.f;
}

FSegmentedSplinesHooks::FEndSupports FSegmentedSplinesHooks::KindSupports( const FSplineKind* kind, AFGSplineHologram* hologram, int32 endIndex )
{
	// Vanilla activates at most one of these per end depending on what the cursor hit
	// (floor, wall, ceiling). Tracks have no supports.
	FEndSupports supports;
	switch( kind->Type )
	{
	case ESplineKind::Belt:
	{
		AFGConveyorBeltHologram* belt = static_cast< AFGConveyorBeltHologram* >( hologram );
		supports.Pole = belt->mChildPoleHologram[ endIndex ].Get();
		supports.Mounts.Add( belt->mChildWallPoleHologram[ endIndex ].Get() );
		supports.Mounts.Add( belt->mChildCeilingPoleHologram[ endIndex ].Get() );
		break;
	}
	case ESplineKind::Pipe:
	{
		AFGPipelineHologram* pipe = static_cast< AFGPipelineHologram* >( hologram );
		supports.Pole = pipe->mChildPoleHologram[ endIndex ].Get();
		supports.Mounts.Add( pipe->mChildWallPoleHologram[ endIndex ].Get() );
		break;
	}
	case ESplineKind::Track:
		break;
	}
	return supports;
}

void FSegmentedSplinesHooks::KindSnapStart( const FSplineKind* kind, AFGSplineHologram* hologram, UFGConnectionComponent* joint )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:
	{
		AFGConveyorBeltHologram* belt = static_cast< AFGConveyorBeltHologram* >( hologram );
		belt->mSnappedConnectionComponents[ 0 ] = Cast< UFGFactoryConnectionComponent >( joint );
		belt->mSnappedWallPassthrough[ 0 ] = nullptr;
		// Flow direction follows from what the start is snapped to (an output feeds this piece, an
		// input is fed by it); this is vanilla's own routine for that.
		belt->SetupSnappedConnectionDirections();
		break;
	}
	case ESplineKind::Pipe:
	{
		AFGPipelineHologram* pipe = static_cast< AFGPipelineHologram* >( hologram );
		pipe->mSnappedConnectionComponents[ 0 ] = Cast< UFGPipeConnectionComponentBase >( joint );
		// Wall holes the player's hologram went through belong to the player's piece, not this one.
		// The list keeps a slot per end that vanilla indexes without checking (ValidatePipeline
		// crashed on an emptied list in the twelfth run), so the slots are nulled, never removed.
		for( TObjectPtr< AFGBuildablePassthrough >& passthrough : pipe->mSnappedPassthroughs )
		{
			passthrough = nullptr;
		}
		break;
	}
	case ESplineKind::Track:
	{
		AFGRailroadTrackHologram* track = static_cast< AFGRailroadTrackHologram* >( hologram );
		UFGRailroadTrackConnectionComponent* connection = Cast< UFGRailroadTrackConnectionComponent >( joint );
		track->mSnappedRailroadTrack = nullptr;
		// Vanilla's own start snap, given the cursor location a player would have aimed at: it records
		// the connection plus the start location/tangent and switch-control state that go with it.
		const FVector location = connection ? connection->GetConnectorLocation() : FVector::ZeroVector;
		const bool vanillaSnapped = connection && track->TryFindAndSnapToOverlappingConnection( 0, location )
			&& track->mSnappedConnectionComponents[ 0 ] == connection;
		if( !vanillaSnapped )
		{
			track->mSnappedConnectionComponents[ 0 ] = connection;
			track->mSnappedStartLocation = location;
			track->mSnappedStartTangent = connection ? connection->GetConnectorNormal() : FVector::ForwardVector;
		}
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] track start snapped %s" ), vanillaSnapped ? TEXT( "by vanilla's overlap snap" ) : TEXT( "by hand" ) );
		break;
	}
	}
}

void FSegmentedSplinesHooks::KindClearEndSnap( const FSplineKind* kind, AFGSplineHologram* hologram )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:
	{
		AFGConveyorBeltHologram* belt = static_cast< AFGConveyorBeltHologram* >( hologram );
		belt->mSnappedConnectionComponents[ 1 ] = nullptr;
		belt->mSnappedWallPassthrough[ 1 ] = nullptr;
		break;
	}
	case ESplineKind::Pipe:
	{
		AFGPipelineHologram* pipe = static_cast< AFGPipelineHologram* >( hologram );
		pipe->mSnappedConnectionComponents[ 1 ] = nullptr;
		// The passthrough list does not say which end each wall hole belongs to; keep the ones at
		// the start (the player's own) and null any other, which can only have been at the far end.
		// Slots are nulled, never removed: vanilla indexes the list without checking its size.
		const FVector startWorld = hologram->GetActorTransform().TransformPosition( hologram->mSplineData[ 0 ].Location );
		for( TObjectPtr< AFGBuildablePassthrough >& passthrough : pipe->mSnappedPassthroughs )
		{
			if( passthrough && FVector::Dist( passthrough->GetActorLocation(), startWorld ) > 600.f )
			{
				passthrough = nullptr;
			}
		}
		break;
	}
	case ESplineKind::Track:
	{
		AFGRailroadTrackHologram* track = static_cast< AFGRailroadTrackHologram* >( hologram );
		track->mSnappedConnectionComponents[ 1 ] = nullptr;
		track->mUseCustomEndRotation = false;
		break;
	}
	}
}

void FSegmentedSplinesHooks::KindOnSplineReplaced( const FSplineKind* kind, AFGSplineHologram* hologram )
{
	if( kind->Type != ESplineKind::Track )
	{
		return;
	}
	// A track keeps one floor sample per spline point (local space; its CDO starts with two of
	// each). Keep the two arrays the same length: the floor under each point if there is one in
	// reach, else the point itself.
	AFGRailroadTrackHologram* track = static_cast< AFGRailroadTrackHologram* >( hologram );
	const FTransform frame = hologram->GetActorTransform();
	UWorld* world = hologram->GetWorld();
	FCollisionQueryParams params( SCENE_QUERY_STAT( SegmentedSplinesTrackFloor ), false );
	params.AddIgnoredActor( hologram );
	for( AFGHologram* child : hologram->GetHologramChildren() )
	{
		if( child ) params.AddIgnoredActor( child );
	}
	track->mFloorData.SetNum( track->mSplineData.Num() );
	for( int32 i = 0; i < track->mSplineData.Num(); ++i )
	{
		const FVector pointWorld = frame.TransformPosition( track->mSplineData[ i ].Location );
		FVector floorWorld = pointWorld;
		FHitResult hit;
		if( world && world->LineTraceSingleByChannel( hit, pointWorld + FVector( 0.f, 0.f, kTraceStartLift ),
			pointWorld - FVector( 0.f, 0.f, kFallbackMaxPoleHeight ), kBuildGunTraceChannel, params ) )
		{
			floorWorld = hit.ImpactPoint;
		}
		track->mFloorData[ i ] = frame.InverseTransformPosition( floorWorld );
	}
}

void FSegmentedSplinesHooks::KindForEachBuildable( const FSplineKind* kind, UWorld* world, TFunctionRef< bool( AFGBuildable* ) > visit )
{
	if( !world ) return;
	switch( kind->Type )
	{
	case ESplineKind::Belt:
		for( TActorIterator< AFGBuildableConveyorBelt > it( world ); it; ++it ) { if( !visit( *it ) ) return; }
		break;
	case ESplineKind::Pipe:
		for( TActorIterator< AFGBuildablePipeBase > it( world ); it; ++it ) { if( !visit( *it ) ) return; }
		break;
	case ESplineKind::Track:
		for( TActorIterator< AFGBuildableRailroadTrack > it( world ); it; ++it ) { if( !visit( *it ) ) return; }
		break;
	}
}

UFGConnectionComponent* FSegmentedSplinesHooks::KindConnection( const FSplineKind* kind, AFGBuildable* buildable, int32 index )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:
	{
		AFGBuildableConveyorBelt* belt = Cast< AFGBuildableConveyorBelt >( buildable );
		return belt ? ( index == 0 ? belt->GetConnection0() : belt->GetConnection1() ) : nullptr;
	}
	case ESplineKind::Pipe:
	{
		AFGBuildablePipeBase* pipe = Cast< AFGBuildablePipeBase >( buildable );
		return pipe ? ( index == 0 ? pipe->GetConnection0() : pipe->GetConnection1() ) : nullptr;
	}
	case ESplineKind::Track:
	{
		AFGBuildableRailroadTrack* track = Cast< AFGBuildableRailroadTrack >( buildable );
		return track ? track->GetConnection( index ) : nullptr;
	}
	}
	return nullptr;
}

FVector FSegmentedSplinesHooks::KindConnectorLocation( const FSplineKind* kind, UFGConnectionComponent* connection )
{
	switch( kind->Type )
	{
	case ESplineKind::Belt:  return static_cast< UFGFactoryConnectionComponent* >( connection )->GetConnectorLocation();
	case ESplineKind::Pipe:  return static_cast< UFGPipeConnectionComponentBase* >( connection )->GetConnectorLocation();
	case ESplineKind::Track: return static_cast< UFGRailroadTrackConnectionComponent* >( connection )->GetConnectorLocation();
	}
	return connection->GetComponentLocation();
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

bool FSegmentedSplinesHooks::IsChainCandidate( AFGSplineHologram* hologram, const FSplineKind* kind, bool bRequireDragStep )
{
	if( !hologram || !kind || sDriverBusy || !hologram->IsLocalHologram() )
	{
		return false;
	}
	const ESplineHologramBuildStep step = hologram->GetCurrentBuildStep();
	if( bRequireDragStep && step != ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding && step != ESplineHologramBuildStep::SHBS_AdjustPole )
	{
		return false;
	}
	const float maxLength = KindMaxLength( kind, hologram );
	if( !hologram->mSplineComponent || hologram->mSplineData.Num() < 2 || maxLength <= 0.f )
	{
		return false;
	}
	const float length = hologram->mSplineComponent->GetSplineLength();
	if( length <= maxLength )
	{
		return false;
	}
	const int32 pieces = FMath::CeilToInt( length / ( maxLength - kSafetyMargin ) );
	return pieces <= kMaxChainPieces;
}

void FSegmentedSplinesHooks::OnCheckValidPlacement( AFGSplineHologram* self )
{
	const FSplineKind* kind = FindKind( self );
	if( !kind || sDriverBusy || !self->IsLocalHologram() )
	{
		return;
	}
	// A hologram back within the limit after a chain preview gets vanilla's own support choice again.
	if( self->GetCurrentBuildStep() == ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding
		&& self->mSplineComponent && self->mSplineComponent->GetSplineLength() <= KindMaxLength( kind, self ) )
	{
		RestoreSupports( KindSupports( kind, self, 1 ) );
	}
}

void FSegmentedSplinesHooks::OnAfterCheckValidPlacement( AFGSplineHologram* self )
{
	const FSplineKind* kind = FindKind( self );
	if( !IsChainCandidate( self, kind, true ) )
	{
		return;
	}
	// Vanilla has just judged the full curve too long. Every piece we will send is under the limit,
	// so that verdict does not apply; everything else it found (clearance, cost, snapping) stands.
	const int32 removed = self->mConstructDisqualifiers.Remove( KindTooLongDisqualifier( kind ) );
	if( removed > 0 )
	{
		UE_LOG( LogSegmentedSplines, VeryVerbose, TEXT( "[%s] %.0f cm hologram over the %.0f limit, chainable: cleared TooLong" ),
			kind->Name, self->mSplineComponent->GetSplineLength(), KindMaxLength( kind, self ) );
	}
}

void FSegmentedSplinesHooks::OnPlacementHit( AFGSplineHologram* self, const FHitResult& hit )
{
	if( self && !sDriverBusy && self->IsLocalHologram() )
	{
		sLastHit.FindOrAdd( self ) = hit;
	}
}

void FSegmentedSplinesHooks::OnAfterMultiStepPlacement( bool bFinalStep, AFGSplineHologram* self )
{
	const FSplineKind* kind = FindKind( self );
	if( !kind || !self->IsLocalHologram() )
	{
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[diag] %s DoMultiStepPlacement -> %s, step %d, length %.0f (limit %.0f)%s" ),
		kind->Name, bFinalStep ? TEXT( "final" ) : TEXT( "more steps" ), static_cast< int32 >( self->GetCurrentBuildStep() ),
		self->mSplineComponent ? self->mSplineComponent->GetSplineLength() : -1.f, KindMaxLength( kind, self ),
		sDriverBusy ? TEXT( " [driver]" ) : TEXT( "" ) );
	if( bFinalStep && !sDriverBusy )
	{
		StartChain( self, kind, TEXT( "DoMultiStepPlacement" ) );
	}
}

void FSegmentedSplinesHooks::OnSerializeConstructMessage( IFGConstructionMessageInterface* self, FArchive& ar, FNetConstructionID id )
{
	AFGSplineHologram* hologram = Cast< AFGSplineHologram >( self );
	const FSplineKind* kind = FindKind( hologram );
	if( !kind )
	{
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[diag] %s SerializeConstructMessage %s id=%s, step %d, length %.0f, local %d, driver busy %d" ),
		kind->Name, ar.IsSaving() ? TEXT( "saving" ) : TEXT( "loading" ), *id.ToString(), static_cast< int32 >( hologram->GetCurrentBuildStep() ),
		hologram->mSplineComponent ? hologram->mSplineComponent->GetSplineLength() : -1.f, hologram->IsLocalHologram() ? 1 : 0, sDriverBusy ? 1 : 0 );
	// Loading happens on the server and on the client's pending-construction copy; neither is ours.
	if( ar.IsSaving() && !sDriverBusy )
	{
		StartChain( hologram, kind, TEXT( "SerializeConstructMessage" ) );
	}
	// The client serialises twice per build: once with an empty ID to make its pending copy, then
	// with the real ID for the server. The built actor replicates back carrying that ID.
	if( ar.IsSaving() && sChain && id.NetPlayerID >= 0 )
	{
		sChain->PendingId = id;
		sChain->bHasPendingId = true;
		UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] piece %d sent as %s" ), sChain->NextPiece, *id.ToString() );
	}
}

void FSegmentedSplinesHooks::OnBuildableFailedConstruction( UFGBuildGunStateBuild* self, FNetConstructionID id )
{
	if( !sChain || !sChain->bWaiting )
	{
		return;
	}
	const bool bOurs = !sChain->bHasPendingId
		|| ( id.NetPlayerID == sChain->PendingId.NetPlayerID && id.Client_ID == sChain->PendingId.Client_ID );
	UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] server refused construction %s while waiting for piece %d of %d%s" ),
		*id.ToString(), sChain->NextPiece, sChain->Pieces.Num(), bOurs ? TEXT( "" ) : TEXT( " (a different build; ignored)" ) );
	if( bOurs )
	{
		AbortChain( TEXT( "the server rejected the piece" ) );
	}
}

// ---------------------------------------------------------------------------------------------
// Chain driver
// ---------------------------------------------------------------------------------------------

void FSegmentedSplinesHooks::StartChain( AFGSplineHologram* hologram, const FSplineKind* kind, const TCHAR* trigger )
{
	// At build time the step no longer matters; only the length does. Once piece 0 is in place
	// the hologram is under the limit, so a second trigger for the same build is a no-op.
	if( !IsChainCandidate( hologram, kind, false ) )
	{
		return;
	}
	if( sChain )
	{
		AbortChain( TEXT( "a new build started while a chain was still running" ) );
	}
	UFGBuildGunStateBuild* buildState = FindBuildState( hologram );
	if( !buildState )
	{
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] could not find the build gun's build state; sending the full %s as vanilla would" ), kind->Name );
		return;
	}
	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] %s build accepted (%s), reshaping into piece 1" ), kind->Name, trigger );
	// While the hologram is reshaped and re-validated the placement hooks stand aside.
	sDriverBusy = true;
	PlanChain( hologram, kind, buildState );
	sDriverBusy = false;
}

UFGBuildGunStateBuild* FSegmentedSplinesHooks::FindBuildState( AFGHologram* hologram )
{
	AFGCharacterPlayer* player = Cast< AFGCharacterPlayer >( hologram->GetConstructionInstigator() );
	AFGBuildGun* buildGun = player ? player->GetBuildGun() : nullptr;
	return buildGun ? Cast< UFGBuildGunStateBuild >( buildGun->GetBuildGunStateFor( EBuildGunState::BGS_BUILD ) ) : nullptr;
}

bool FSegmentedSplinesHooks::PlanChain( AFGSplineHologram* hologram, const FSplineKind* kind, UFGBuildGunStateBuild* buildState )
{
	USplineComponent* spline = hologram->mSplineComponent;
	const float maxLength = KindMaxLength( kind, hologram );
	const float totalLength = spline->GetSplineLength();
	const int32 numPieces = FMath::CeilToInt( totalLength / ( maxLength - kSafetyMargin ) );
	const float pieceLength = totalLength / numPieces;

	TUniquePtr< FChainPlan > plan = MakeUnique< FChainPlan >();
	plan->Kind = kind;
	plan->BuildState = buildState;
	plan->World = hologram->GetWorld();
	plan->Recipe = hologram->GetRecipe();
	plan->Frame = hologram->GetActorTransform();
	plan->FinalStep = hologram->GetCurrentBuildStep();
	if( const FHitResult* hit = sLastHit.Find( hologram ) )
	{
		plan->EndHit = *hit;
		plan->bHasEndHit = true;
	}

	// Cut the curve into equal pieces. The game's splitter works on a spline component, so the
	// hologram's own component is reused for each remainder and put back on piece 0 at the end.
	const TArray< FSplinePointData > original = hologram->mSplineData;
	TArray< FSplinePointData > remaining = original;
	for( int32 i = 0; i < numPieces - 1; ++i )
	{
		SetSplineData( hologram, kind, CopyTemp( remaining ) );
		TArray< FSplinePointData > first;
		TArray< FSplinePointData > rest;
		FSplineUtils::SplitSplineAtDistance( spline, pieceLength, first, rest );
		if( first.Num() < 2 || rest.Num() < 2 )
		{
			SetSplineData( hologram, kind, CopyTemp( original ) );
			UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] split %d of %d produced %d + %d points, building vanilla's way" ), i + 1, numPieces, first.Num(), rest.Num() );
			return false;
		}
		MakeContinuous( rest, first.Last().Location, *FString::Printf( TEXT( "split %d" ), i + 1 ) );
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

	// Piece 0 keeps the player's start (its snap and start support are already vanilla's) and gets
	// a joint at its far end: no snap, no mount, a pole only if there is a floor under it.
	ApplyPiece( hologram, kind, plan->Pieces[ 0 ] );
	KindClearEndSnap( kind, hologram );
	SeatOrDisableEndPole( hologram, kind, maxLength - kSafetyMargin, KindSupports( kind, hologram, 1 ), TEXT( "chain piece 1" ) );
	ValidateForConstruction( hologram );
	if( !hologram->CanConstruct() )
	{
		UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] piece 1 of %d is not constructible (%s); building it anyway so vanilla reports why" ),
			numPieces, *DisqualifierNames( hologram ) );
	}

	plan->NextPiece = 1;
	plan->bWaiting = true;
	plan->WaitStart = plan->Pieces[ 0 ][ 0 ].Location;
	plan->WaitEnd = plan->Pieces[ 0 ].Last().Location;
	plan->SentAt = FPlatformTime::Seconds();
	SnapshotBuildables( *plan, hologram->GetWorld() );
	sChain = MoveTemp( plan );

	if( !sTicker.IsValid() )
	{
		sTicker = FTSTicker::GetCoreTicker().AddTicker( FTickerDelegate::CreateStatic( &FSegmentedSplinesHooks::TickChain ) );
	}

	UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] %.0f cm %s -> %d pieces of %.0f cm (limit %.0f); piece 1 goes with the player's build" ),
		totalLength, kind->Name, numPieces, pieceLength, maxLength );
	return true;
}

void FSegmentedSplinesHooks::ValidateForConstruction( AFGHologram* hologram )
{
	UFGInventoryComponent* inventory = nullptr;
	if( AFGCharacterPlayer* player = Cast< AFGCharacterPlayer >( hologram->GetConstructionInstigator() ) )
	{
		inventory = player->GetInventory();
	}
	hologram->ValidatePlacementAndCost( inventory );
}

FString FSegmentedSplinesHooks::DisqualifierNames( AFGHologram* hologram )
{
	TArray< TSubclassOf< UFGConstructDisqualifier > > why;
	hologram->GetConstructDisqualifiers( why );
	FString names;
	for( auto& d : why ) names += ( d ? d->GetName() : TEXT( "null" ) ) + TEXT( " " );
	return names;
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

void FSegmentedSplinesHooks::MakeContinuous( TArray< FSplinePointData >& piece, const FVector& expectedStart, const TCHAR* what )
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
}

void FSegmentedSplinesHooks::SetSplineData( AFGSplineHologram* hologram, const FSplineKind* kind, TArray< FSplinePointData >&& localPoints )
{
	hologram->mSplineData = MoveTemp( localPoints );
	KindOnSplineReplaced( kind, hologram );
	// Rebuild the visual spline, spline meshes and clearance from the new data, exactly as the
	// hologram does after routing. Vanilla's validation measures mSplineComponent.
	hologram->UpdateSplineComponent();
}

void FSegmentedSplinesHooks::ApplyPiece( AFGSplineHologram* hologram, const FSplineKind* kind, const TArray< FSplinePointData >& worldPiece )
{
	const FTransform frame = hologram->GetActorTransform();
	TArray< FSplinePointData > local = worldPiece;
	for( FSplinePointData& point : local )
	{
		point.Location = frame.InverseTransformPosition( point.Location );
		point.ArriveTangent = frame.InverseTransformVector( point.ArriveTangent );
		point.LeaveTangent = frame.InverseTransformVector( point.LeaveTangent );
	}
	SetSplineData( hologram, kind, MoveTemp( local ) );
}

void FSegmentedSplinesHooks::SnapshotBuildables( FChainPlan& plan, UWorld* world )
{
	plan.Known.Reset();
	KindForEachBuildable( plan.Kind, world, [ &plan ]( AFGBuildable* buildable )
	{
		plan.Known.Add( buildable );
		return true;
	} );
}

bool FSegmentedSplinesHooks::TickChain( float deltaTime )
{
	if( !sChain )
	{
		sTicker.Reset();
		return false;
	}
	FChainPlan& plan = *sChain;
	const FSplineKind* kind = plan.Kind;
	UWorld* world = plan.World.Get();
	if( !world || !plan.BuildState.IsValid() )
	{
		AbortChain( TEXT( "world or build gun went away" ) );
		return false;
	}

	if( plan.bWaiting )
	{
		// The server builds the piece and replicates the actor back. It carries the construction ID
		// the client sent (replicated); failing that, match it by its ends.
		KindForEachBuildable( kind, world, [ & ]( AFGBuildable* candidate )
		{
			// Client IDs restart every launch, so an actor from an earlier session can carry the same
			// ID; only one that did not exist when the piece was sent can be the piece.
			if( plan.Known.Contains( candidate ) )
			{
				return true;
			}
			UFGConnectionComponent* c0 = KindConnection( kind, candidate, 0 );
			UFGConnectionComponent* c1 = KindConnection( kind, candidate, 1 );
			if( !c0 || !c1 )
			{
				return true;
			}
			// A belt stores its spline in flow direction (connection0 = input, connection1 = output).
			// When the player drags against the flow, the server reverses the points, so the joint can
			// be at either connector: take whichever sits at the expected far end.
			const float d1e = FVector::Dist( KindConnectorLocation( kind, c1 ), plan.WaitEnd );
			const float d0e = FVector::Dist( KindConnectorLocation( kind, c0 ), plan.WaitEnd );
			bool idMatch = false;
			if( plan.bHasPendingId )
			{
				const FNetConstructionID cid = candidate->GetNetConstructionID();
				idMatch = cid.NetPlayerID == plan.PendingId.NetPlayerID && cid.Client_ID == plan.PendingId.Client_ID;
			}
			UFGConnectionComponent* joint = nullptr;
			float jointDist = 0.f;
			if( d1e <= 4.f * kJointTolerance )       { joint = c1; jointDist = d1e; }
			else if( d0e <= 4.f * kJointTolerance )  { joint = c0; jointDist = d0e; }
			if( !joint )
			{
				UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] new %s %s ignored: neither connector at the expected joint (c1 %.0f cm, c0 %.0f cm away; id match %d)" ),
					kind->Name, *candidate->GetName(), d1e, d0e, idMatch ? 1 : 0 );
				return true;
			}
			plan.Last = candidate;
			plan.JointConnection = joint;
			plan.bWaiting = false;
			UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] piece %d of %d arrived after %.0f ms: %s (id match %d, joint is %s, %.0f cm from expected)" ),
				plan.NextPiece, plan.Pieces.Num(), ( FPlatformTime::Seconds() - plan.SentAt ) * 1000.0, *candidate->GetName(), idMatch ? 1 : 0,
				joint == c1 ? TEXT( "connection1 (drawn with the flow)" ) : TEXT( "connection0 (drawn against the flow)" ), jointDist );
			return false;
		} );
		if( plan.bWaiting )
		{
			const double waited = FPlatformTime::Seconds() - plan.SentAt;
			if( !plan.bSlowNoticed && waited > kPieceSlowNotice )
			{
				plan.bSlowNoticed = true;
				UE_LOG( LogSegmentedSplines, Log, TEXT( "[chain] still waiting for piece %d of %d after %.0f s (slow server or link; giving it %.0f s)" ),
					plan.NextPiece, plan.Pieces.Num(), waited, kPieceTimeout );
			}
			if( waited > kPieceTimeout )
			{
				// Say what was there, so a miss explains itself.
				UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain] expected piece start %s end %s, sent as %s; %ss within 30 m of the start:" ),
					*plan.WaitStart.ToString(), *plan.WaitEnd.ToString(), plan.bHasPendingId ? *plan.PendingId.ToString() : TEXT( "(no id seen)" ), kind->Name );
				int32 listed = 0;
				KindForEachBuildable( kind, world, [ & ]( AFGBuildable* buildable )
				{
					UFGConnectionComponent* c0 = KindConnection( kind, buildable, 0 );
					UFGConnectionComponent* c1 = KindConnection( kind, buildable, 1 );
					const FVector a = c0 ? KindConnectorLocation( kind, c0 ) : buildable->GetActorLocation();
					const FVector b = c1 ? KindConnectorLocation( kind, c1 ) : buildable->GetActorLocation();
					if( FVector::Dist( a, plan.WaitStart ) > 3000.f && FVector::Dist( b, plan.WaitStart ) > 3000.f )
					{
						return true;
					}
					UE_LOG( LogSegmentedSplines, Warning, TEXT( "[chain]   %s id=%s c0 %s (%.0f cm from start) c1 %s (%.0f cm from end)" ),
						*buildable->GetName(), *buildable->GetNetConstructionID().ToString(), *a.ToString(), FVector::Dist( a, plan.WaitStart ), *b.ToString(), FVector::Dist( b, plan.WaitEnd ) );
					return ++listed < 8;
				} );
				AbortChain( TEXT( "timed out waiting for the server to build a piece (rejected, or out of parts?)" ) );
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
	const FSplineKind* kind = plan.Kind;
	UFGBuildGunStateBuild* buildState = plan.BuildState.Get();
	AFGSplineHologram* holo = Cast< AFGSplineHologram >( buildState ? buildState->GetHologram() : nullptr );
	if( !holo || FindKind( holo ) != kind )
	{
		AbortChain( *FString::Printf( TEXT( "the build gun no longer holds a %s hologram" ), kind->Name ) );
		return;
	}
	if( holo->GetRecipe() != plan.Recipe )
	{
		AbortChain( TEXT( "the build gun switched recipe" ) );
		return;
	}
	AFGBuildable* previous = plan.Last.Get();
	UFGConnectionComponent* jointConnection = plan.JointConnection.Get();
	if( !previous || !jointConnection )
	{
		AbortChain( TEXT( "the previous piece vanished" ) );
		return;
	}

	const int32 i = plan.NextPiece;
	const bool bFinal = ( i == plan.Pieces.Num() - 1 );
	const float maxLength = KindMaxLength( kind, holo );

	// The built piece's connector is where this piece really starts. Move the piece's first point
	// onto it (the server may have adjusted the end by a few cm, and seating a joint pole can have
	// nudged it); anything larger means it is not our piece.
	{
		const FVector connector = KindConnectorLocation( kind, jointConnection );
		const FVector delta = connector - plan.Pieces[ i ][ 0 ].Location;
		if( delta.Size() > 4.f * kJointTolerance )
		{
			AbortChain( *FString::Printf( TEXT( "previous piece's connector is %.0f cm from where piece %d starts" ), delta.Size(), i + 1 ) );
			return;
		}
		plan.Pieces[ i ][ 0 ].Location = connector;
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] piece %d start moved %.1f cm onto the previous piece's connector" ), i + 1, delta.Size() );
	}

	// From here the placement hooks stand aside: the hologram is ours until it is sent.
	sDriverBusy = true;
	plan.bHologramIsOurs = true;

	// Start: snapped to the previous piece's connector, so the two connect. Vanilla hides the start
	// supports for a snapped start; this hologram never went through that step, so do it by hand.
	holo->SetActorTransform( plan.Frame );
	KindSnapStart( kind, holo, jointConnection );
	{
		FEndSupports startSupports = KindSupports( kind, holo, 0 );
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
		// same hit so it chooses the support (pole, wall or ceiling mount) or snaps to the target
		// exactly as it did for the original hologram; then put our piece's curve back under it.
		ApplyPiece( holo, kind, plan.Pieces[ i ] );
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
		KindClearEndSnap( kind, holo );
	}

	// Vanilla may have moved the hologram while placing the end; the piece is in world space, so
	// it lands in the same place regardless.
	ApplyPiece( holo, kind, plan.Pieces[ i ] );

	if( !bFinal || !plan.bHasEndHit )
	{
		SeatOrDisableEndPole( holo, kind, maxLength - kSafetyMargin, KindSupports( kind, holo, 1 ), TEXT( "chain joint" ) );
	}

	ValidateForConstruction( holo );
	if( !holo->CanConstruct() )
	{
		const FString why = DisqualifierNames( holo );
		sDriverBusy = false;
		AbortChain( *FString::Printf( TEXT( "piece %d of %d is not constructible: %s" ), i + 1, plan.Pieces.Num(), *why ) );
		return;
	}

	// Hand the finished hologram to the build gun as if the player had clicked on the last step:
	// vanilla assigns the construction ID, serialises (our hook stands aside), sends, shows the
	// pending copy and spawns a fresh hologram for whatever comes next. Belts and pipes always
	// finish from the pole-adjust step (proven for free and snapped belt ends); a track has no
	// such step, so it is put on whatever step the player's own click was accepted from.
	holo->mBuildStep = kind->bHasPoleAdjustStep ? ESplineHologramBuildStep::SHBS_AdjustPole : plan.FinalStep;

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
		plan.bSlowNoticed = false;
	}
	SnapshotBuildables( plan, buildState->GetWorld() );

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
		UE_LOG( LogSegmentedSplines, Verbose, TEXT( "[chain] first step input left the hologram in place, sending the second" ) );
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
// End supports
// ---------------------------------------------------------------------------------------------

float FSegmentedSplinesHooks::SeatOrDisableEndPole( AFGSplineHologram* hologram, const FSplineKind* kind, float cutDistance,
	const FEndSupports& supports, const TCHAR* debugName )
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
	const FVector traceEnd = newEndWorld - FVector( 0.f, 0.f, maxPoleHeight + kMaxPoleSeatNudge );

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
	const float desiredHeight = newEndWorld.Z - hit.ImpactPoint.Z;
	pole->SetPoleHeight( desiredHeight );
	const float actualHeight = pole->GetPoleHeight();
	const float mismatch = ( hit.ImpactPoint.Z + actualHeight ) - newEndWorld.Z;

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

	// Seat the pole: on the floor, under the end, facing along the end of the curve.
	if( sDisabledByUs.Contains( pole ) )
	{
		pole->SetDisabled( false );
		sDisabledByUs.Remove( pole );
	}
	const FVector newEndDir = hologramTransform.TransformVector( hologram->mSplineData.Last().ArriveTangent ).GetSafeNormal2D();
	FRotator poleRotation = pole->GetActorRotation();
	if( !newEndDir.IsNearlyZero() )
	{
		poleRotation.Yaw = newEndDir.Rotation().Yaw;
	}
	pole->SetActorLocationAndRotation( hit.ImpactPoint, poleRotation );

	// Put the spline end on the pole top. Vanilla routes the spline to the pole's connection, which
	// is what makes the two meet; an end we placed has to be moved there by hand.
	if( !FMath::IsNearlyZero( mismatch, 0.5f ) )
	{
		TArray< FSplinePointData > nudged = hologram->mSplineData;
		nudged.Last().Location += hologramTransform.InverseTransformVector( FVector( 0.f, 0.f, mismatch ) );
		SetSplineData( hologram, kind, MoveTemp( nudged ) );

		// The nudge lengthened the curve slightly. If that pushed it back over the limit, cut
		// once more; the end moves a few cm back along the curve, so keep the pole under it.
		if( spline->GetSplineLength() > cutDistance + kSafetyMargin )
		{
			TArray< FSplinePointData > firstHalf;
			TArray< FSplinePointData > secondHalf;
			FSplineUtils::SplitSplineAtDistance( spline, cutDistance, firstHalf, secondHalf );
			if( firstHalf.Num() >= 2 )
			{
				SetSplineData( hologram, kind, MoveTemp( firstHalf ) );
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
