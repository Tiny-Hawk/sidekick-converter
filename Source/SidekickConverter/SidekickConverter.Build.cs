// Editor-only module for the Sidekick Converter (Synty Sidekick Unity packs to Unreal).

using UnrealBuildTool;

public class SidekickConverter : ModuleRules
{
	public SidekickConverter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MeshDescription",
			"SkeletalMeshDescription",
			"AnimationCore",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"DesktopPlatform",
				"Projects",
				"AssetRegistry",
				"WorkspaceMenuStructure",
				"ContentBrowser",
			});
		}
	}
}
