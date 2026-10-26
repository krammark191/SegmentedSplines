#include "SegmentedSplines.h"
#include "SegmentedSplinesHooks.h"

DEFINE_LOG_CATEGORY( LogSegmentedSplines );

#define LOCTEXT_NAMESPACE "FSegmentedSplinesModule"

void FSegmentedSplinesModule::StartupModule()
{
	// Native hooks patch the running game's vtables/functions. Never install them inside the editor
	// process: the docs warn it can crash the editor.
#if !WITH_EDITOR
	FSegmentedSplinesHooks::RegisterHooks();
#endif
	UE_LOG( LogSegmentedSplines, Log, TEXT( "SegmentedSplines module started" ) );
}

void FSegmentedSplinesModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE( FSegmentedSplinesModule, SegmentedSplines )
