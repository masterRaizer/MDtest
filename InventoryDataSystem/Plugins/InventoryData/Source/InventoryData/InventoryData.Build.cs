using UnrealBuildTool;

public class InventoryData : ModuleRules
{
	public InventoryData(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore",
			// UDeveloperSettings lives in its own module since UE 4.26. Remove this line for UE 4.25.
			"DeveloperSettings"
		});
	}
}
