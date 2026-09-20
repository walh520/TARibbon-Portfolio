using UnrealBuildTool;

public class TARibbonEditor : ModuleRules
{
    public TARibbonEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "SlateCore", "TARibbon"
        });
    }
}
