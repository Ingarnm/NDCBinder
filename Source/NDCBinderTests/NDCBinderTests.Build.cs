// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NDCBinderTests : ModuleRules
{
	public NDCBinderTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Niagara",
				"NDCBinder",
			}
			);
	}
}
