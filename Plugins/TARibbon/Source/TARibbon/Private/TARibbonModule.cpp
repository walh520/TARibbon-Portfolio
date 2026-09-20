// UE integration shell. The numerical core and HLSL do not depend on this file.
#include "TARibbonModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogTARibbon);

void FTARibbonModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TARibbon"));
    checkf(Plugin.IsValid(), TEXT("TARibbon plugin descriptor was not found"));
    AddShaderSourceDirectoryMapping(
        TEXT("/Plugin/TARibbon"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
}

void FTARibbonModule::ShutdownModule()
{
    // Shader directory mappings live for the process lifetime. Never reset other plugins' mappings.
}

IMPLEMENT_MODULE(FTARibbonModule, TARibbon)
