// NDCBinderCompilerExtension.h
#pragma once

#include "BlueprintCompilerExtension.h"

#include "NDCBinderCompilerExtension.generated.h"

/**
 * Fails a Blueprint's compile when a writer it carries holds a row that cannot run.
 *
 * This is the whole reason the plugin can promise that without asking anything of the classes that
 * own a writer. The alternative — UObject::IsDataValid, which the compiler also runs — is a virtual
 * on the OWNER, so every consumer would have to forward to the writer by hand, and a consumer that
 * forgot would silently lose the report. A compiler extension is registered once, by the plugin,
 * for every Blueprint type there is; the writer stays self-contained.
 *
 * See NDCBinderCompilerExtension.cpp for what can and cannot be read at the moment this runs — the
 * compiler calls it mid-flight, between building the class layout and compiling functions, and both
 * halves of the question (the authored values, and the class those values name things on) live in
 * different places by then.
 */
UCLASS()
class UNDCBinderCompilerExtension : public UBlueprintCompilerExtension
{
	GENERATED_BODY()

public:
	UNDCBinderCompilerExtension(const FObjectInitializer& ObjectInitializer);

	/**
	 * Registers one instance for UBlueprint, which covers every Blueprint type: the compilation
	 * manager walks the asset's class hierarchy up to UBlueprint looking for extensions.
	 *
	 * Idempotent on purpose. The engine has no way to unregister an extension, so registering twice
	 * — which a module reload inside one editor session would do — would report every row twice with
	 * no way to take the second one back.
	 */
	static void Register();

protected:
	//~ UBlueprintCompilerExtension
	virtual void ProcessBlueprintCompiled(const FKismetCompilerContext& CompilationContext, const FBlueprintCompiledData& Data) override;
	//~ End UBlueprintCompilerExtension
};
