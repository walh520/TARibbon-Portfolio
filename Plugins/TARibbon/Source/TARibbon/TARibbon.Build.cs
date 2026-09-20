using UnrealBuildTool;

public class TARibbon : ModuleRules
{
    public TARibbon(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Keep the portable reference implementation intact. Its validation path
        // deliberately reports invalid data with standard exceptions.
        bEnableExceptions = true;
        PublicDependencyModuleNames.AddRange(new[] {
            "Core",
            "CoreUObject",
            "Engine",
            "SceneWind"
        });
        PrivateDependencyModuleNames.AddRange(new[] {
            "Projects",
            "RenderCore",
            "RHI",
            "Renderer"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] {
                "MeshDescription",
                "StaticMeshDescription"
            });
        }
    }
}
