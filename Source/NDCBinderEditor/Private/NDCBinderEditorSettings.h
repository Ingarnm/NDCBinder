// NDCBinderEditorSettings.h
#pragma once

#include "Engine/DeveloperSettings.h"

#include "NDCBinderEditorSettings.generated.h"

/**
 * One access context field the writer's details panel does not draw a row for.
 *
 * Keyed by the context TYPE as well as the name, which is the whole reason this is a setting and not a
 * list of names in the code: a name on its own both under-hides (another context's field of the same
 * nature, under a different name, stays listed) and over-hides (someone else's context with a field
 * that happens to share the name loses its row silently).
 */
USTRUCT()
struct FNDCHiddenContextField
{
	GENERATED_BODY()

	/** The access context type the rule is written against, and everything derived from it. */
	UPROPERTY(EditAnywhere, Category = "NDC", meta = (MetaStruct = "/Script/Niagara.NDCAccessContextBase"))
	TObjectPtr<UScriptStruct> ContextType;

	/** Name of the input field to leave out of the panel. */
	UPROPERTY(EditAnywhere, Category = "NDC")
	FName FieldName;
};

/**
 * Editor-side settings for the NDC Binder panel.
 *
 * HIDING IS NOT DISABLING. A field left out of the panel keeps whatever its context type defaults to
 * and is still writable from C++; what goes away is the row, not the value. Hiding one is worth doing
 * when the row could only ever mislead — an input the write provably cannot act on — and not as a way
 * to switch something off.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "NDC Binder"))
class UNDCBinderEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UNDCBinderEditorSettings();

	//~ UDeveloperSettings
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	//~ End UDeveloperSettings

	/** True when the panel should not draw a row for this field of this context type. */
	bool IsContextFieldHidden(const UScriptStruct* ContextType, const FProperty& Field) const;

	/**
	 * Access context inputs the panel leaves out. Ships with one entry; see the struct for why the
	 * context type is part of the key, and the class comment for what hiding does and does not do.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Access Context", meta = (TitleProperty = "FieldName"))
	TArray<FNDCHiddenContextField> HiddenContextFields;
};
