// NDCBinderValidator.h
#pragma once

#include "EditorValidatorBase.h"
#include "NDCBinderValidator.generated.h"

/**
 * Reports any asset holding an FNDCBinder whose bindings cannot run.
 *
 * A binding is stored as a bare function name, so it outlives everything that decides whether it is
 * usable — the function's signature, the channel variable's type, the writer's Event Data Type. The
 * details panel marks a broken one, but a marker only helps someone who happens to open that asset.
 *
 * What it reports is FNDCBinder::ValidateBindings, the same entry point
 * UNDCBinderCompilerExtension uses to fail a Blueprint's compile. This pass is the other half of
 * that: it runs on Save, on "Validate Assets" and in the DataValidation commandlet, which is what
 * catches a broken binding in a build where nobody compiled anything — and it is the ONLY half for
 * an asset that is not a Blueprint (a Data Asset carrying a writer never compiles at all).
 */
UCLASS()
class UNDCBinderValidator : public UEditorValidatorBase
{
	GENERATED_BODY()

public:
	//~ UEditorValidatorBase
	virtual bool CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context) override;
	//~ End UEditorValidatorBase
};
