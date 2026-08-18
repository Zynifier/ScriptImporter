using UnrealBuildTool;
using System.IO;

public class ScriptImporter : ModuleRules
{
    public ScriptImporter(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AssetRegistry", "AssetTools",
            "Json", "JsonUtilities", "Slate", "SlateCore", "LevelEditor", "DesktopPlatform",
            "MainFrame", "ContentBrowser", "Niagara", "NiagaraCore", "NiagaraEditor"
        });
        PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Plugins/FX/Niagara/Source/NiagaraEditor/Private"));
    }
}
