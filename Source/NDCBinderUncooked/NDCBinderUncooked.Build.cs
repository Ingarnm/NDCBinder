// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * Where the plugin's K2Nodes live, because they cannot live anywhere else.
 *
 * A Blueprint that contains a node keeps a reference to the node's class, and that reference has to
 * resolve while the asset is being cooked — which an Editor module is not around for. The compiler
 * says so out loud: "The node is from an Editor Only module, but is placed in a runtime blueprint!
 * K2 Nodes should only be defined in a Developer or UncookedOnly module." UncookedOnly is exactly
 * that: present in the editor and in commandlets, absent from a cooked game.
 */
public class NDCBinderUncooked : ModuleRules
{
	public NDCBinderUncooked(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				// The function the node calls, and FInstancedStruct for the type it gives an
				// unconnected Event Data pin at expansion time.
				"NDCBinder",
				// UK2Node_CallFunction, UBlueprintNodeSpawner, FBlueprintActionDatabaseRegistrar.
				"BlueprintGraph",
				// FKismetCompilerContext, which ExpandNode is handed.
				"KismetCompiler",
			}
			);
	}
}
