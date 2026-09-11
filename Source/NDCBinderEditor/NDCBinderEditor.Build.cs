// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NDCBinderEditor : ModuleRules
{
	public NDCBinderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// The module has no public headers — everything it needs is a private dependency.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"NDCBinder",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"UnrealEd",
				"PropertyEditor",
				"BlueprintGraph",
				// UNDCBinderCompilerExtension: Kismet owns the extension point and the compilation
				// manager; KismetCompiler has to be named too, because Kismet's own public header for
				// it includes KismetCompiler.h while depending on that module only privately.
				"Kismet",
				"KismetCompiler",
				// FInstancedStructDataDetails renders the access context's own fields as flat rows,
				// without the struct-type picker — the type comes from the channel, not from a choice.
				"StructUtilsEditor",
				// Reports unusable bindings as real validation errors (UEditorValidatorBase).
				"DataValidation",
				// UNDCBinderEditorSettings: which access context inputs the panel leaves out.
				"DeveloperSettings",
				"Niagara",
			}
			);
	}
}
