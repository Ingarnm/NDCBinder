// NDCBinderValidator.cpp

#include "NDCBinderValidator.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Misc/DataValidation.h"
#include "NDCBinder.h"
#include "NDCBinderAssetWalk.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NDCBinderValidator)

namespace NDCBinderValidatorPrivate
{
	/**
	 * The object whose properties actually carry the authored values. A Blueprint asset keeps them on
	 * its generated class default object; anything else is its own defaults.
	 */
	static UObject* GetDefaultsObject(UObject* Asset)
	{
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			return Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		}
		return Asset;
	}

	/**
	 * Every writer an asset carries a value for: on its own defaults, and on the component templates
	 * of an actor Blueprint.
	 *
	 * A component added in the Blueprint editor keeps its authored values on a template, not on the
	 * actor's defaults, so a writer configured there is invisible to the walk over the defaults.
	 * Bindings on it also resolve against the COMPONENT's class, which is why the body is handed the
	 * object holding each writer rather than the asset it was found through.
	 *
	 * Unlike the compiler extension's version of this, everything here is the live object: nothing is
	 * mid-compile, so the class default object and the class describing it are the current ones.
	 */
	template <typename BodyType>
	static void ForEachWriter(UObject* Asset, BodyType&& Body)
	{
		using namespace NDCBinderAssetWalk;

		UObject* Defaults = GetDefaultsObject(Asset);
		if (!Defaults)
		{
			return;
		}
		ForEachWriterOn(Defaults, FString(), Body);

		UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Defaults->GetClass());
		if (!GeneratedClass)
		{
			return;
		}

		// Off the generated class, which is where a loaded asset keeps them.
		ForEachWriterOnComponentTemplates(
			GeneratedClass->SimpleConstructionScript,
			GeneratedClass->ComponentTemplates,
			GeneratedClass->GetInheritableComponentHandler(/*bCreateIfNecessary=*/ false),
			Body);
	}
}

bool UNDCBinderValidator::CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const
{
	using namespace NDCBinderValidatorPrivate;

	bool bHasWriter = false;
	ForEachWriter(InObject, [&bHasWriter](const FNDCBinder&, const FString&, UObject*) { bHasWriter = true; });
	return bHasWriter;
}

EDataValidationResult UNDCBinderValidator::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	using namespace NDCBinderValidatorPrivate;

	EDataValidationResult Result = EDataValidationResult::Valid;
	ForEachWriter(InAsset, [&Result, &Context](const FNDCBinder& Writer, const FString& WriterName, UObject* Owner)
	{
		Result = CombineDataValidationResults(Result, Writer.ValidateBindings(Owner->GetClass(), WriterName, Context));
	});

	if (Result == EDataValidationResult::Invalid)
	{
		return Result;
	}

	AssetPasses(InAsset);
	return EDataValidationResult::Valid;
}
