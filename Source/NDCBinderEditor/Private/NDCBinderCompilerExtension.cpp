// NDCBinderCompilerExtension.cpp
//
// WHERE IN THE COMPILE THIS RUNS, and why the two obvious reads are wrong.
//
// FBlueprintCompilationManagerImpl::ProcessExtensions is called between STAGE XII (class layout) and
// STAGE XIII (functions and the new CDO). Errors go into the same FCompilerResultsLog the rest of
// the compile uses and are counted right after, which is what turns one into BS_Error; nothing
// resets that log in between (the error-state reset is STAGE IV, and it clears node flags). Epic's
// own comment on the call site is "give extension a chance to raise errors".
//
// 1. THE VALUES ARE NOT ON THE CLASS BEING COMPILED. By this point CleanAndSanitizeClass has purged
//    it — "properties, functions, params, as they will be regenerated" — and pointed its property
//    chain at the parent's. The authored defaults live on the pre-compile CDO, which the reinstancer
//    moved aside before any of that: MoveCDOToNewClass duplicates the class (with an
//    ensure(PrevStructSize == NewStructSize) on the copy) and unconditionally does
//    OldCDO->SetClass(CopyOfOwnerClass). So the old CDO is described by a class that matches its own
//    memory, and reading it by reflection is sound; reading it through the class being compiled would
//    be old bytes at new offsets. The compiler context hands that duplicate over as OldClass.
//    FKismetCompilerContext::OldCDO is NOT it: the compilation manager passes AvoidCDODuplication,
//    which clears the CDO off the class, so CleanAndSanitizeClass finds nothing there to record.
//
// 2. THE NAMES DO NOT RESOLVE AGAINST EITHER OF THOSE CLASSES. A binding stores a bare function
//    name, and the question is whether it still exists — on the class as it is NOW. The class being
//    compiled has no functions yet, and the pre-compile duplicate has the old ones, so it would
//    answer for a Blueprint function that was just renamed or deleted (and Create Binding authors
//    Blueprint functions). The skeleton class is the one that is both current and complete here: it
//    is recompiled at STAGE VIII, carries every declared function, and inherits the natives.
//
// Component templates are the exception to all of the above: SaveSubObjectsFromCleanAndSanitizeClass
// explicitly saves the SCS nodes, ComponentTemplates and the inheritable component handler from the
// purge, and their classes have nothing to do with the compile — so both their values and their own
// class are the live ones.

#include "NDCBinderCompilerExtension.h"

#include "BlueprintCompilationManager.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "KismetCompiler.h"
#include "Misc/DataValidation.h"
#include "NDCBinder.h"
#include "NDCBinderAssetWalk.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NDCBinderCompilerExtension)

UNDCBinderCompilerExtension::UNDCBinderCompilerExtension(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UNDCBinderCompilerExtension::Register()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	// The manager keeps its extensions alive through its own AddReferencedObjects, so this needs no
	// root of its own.
	FBlueprintCompilationManager::RegisterCompilerExtension(
		UBlueprint::StaticClass(), NewObject<UNDCBinderCompilerExtension>(GetTransientPackage()));
}

void UNDCBinderCompilerExtension::ProcessBlueprintCompiled(const FKismetCompilerContext& CompilationContext, const FBlueprintCompiledData& Data)
{
	using namespace NDCBinderAssetWalk;

	UBlueprint* Blueprint = CompilationContext.Blueprint;
	if (!Blueprint)
	{
		return;
	}

	// Compile-on-load is left alone, for the same reason the engine leaves its own data validation
	// out of it: dependencies are still coming in, so a binding to a function declared on a parent
	// Blueprint can read as missing when it is merely not compiled yet. A false compile error on
	// load would be worse than a true one on the next real compile.
	if (Blueprint->bIsRegeneratingOnLoad)
	{
		return;
	}

	// See note 2 at the top of the file.
	const UClass* LookupClass = Blueprint->SkeletonGeneratedClass;
	if (!LookupClass)
	{
		return;
	}

	// Collected here rather than logged as we go: the writer reports into the standard validation
	// context — the same call the Save-time validator makes, so there is one implementation of what
	// is wrong with a row — and this turns those issues into compiler messages.
	FDataValidationContext Reported;

	// See note 1. Nothing to validate on a Blueprint that never had a CDO (a brand new asset).
	UObject* Defaults = CompilationContext.OldClass ? CompilationContext.OldClass->GetDefaultObject(false) : nullptr;
	ForEachWriterOn(Defaults, FString(),
		[&Reported, LookupClass](const FNDCBinder& Writer, const FString& WriterName, UObject*)
		{
			Writer.ValidateBindings(LookupClass, WriterName, Reported);
		});

	// Off the UBlueprint, not the class being compiled: see the note at the top of this file — the
	// templates are saved from the purge, and their own classes have nothing to do with this compile,
	// so both their values and the class their bindings resolve against are the live ones.
	ForEachWriterOnComponentTemplates(
		Blueprint->SimpleConstructionScript,
		Blueprint->ComponentTemplates,
		Blueprint->GetInheritableComponentHandler(/*bCreateIfNecessary=*/ false),
		[&Reported](const FNDCBinder& Writer, const FString& WriterName, UObject* Owner)
		{
			Writer.ValidateBindings(Owner->GetClass(), WriterName, Reported);
		});

	// The log's format string treats "@@" as a node/pin placeholder and takes everything else
	// literally, so a message goes in as it is.
	FCompilerResultsLog& MessageLog = CompilationContext.MessageLog;
	for (const FDataValidationContext::FIssue& Issue : Reported.GetIssues())
	{
		const FString Text = Issue.Message.ToString();
		if (Issue.Severity == EMessageSeverity::Error)
		{
			MessageLog.Error(*Text);
		}
		else
		{
			MessageLog.Warning(*Text);
		}
	}
}
