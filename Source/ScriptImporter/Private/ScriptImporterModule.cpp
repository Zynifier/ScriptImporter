#include "ScriptImporterModule.h"

#include "AssetRegistryModule.h"
#include "DesktopPlatformModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "EdGraphSchema_Niagara.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/CommandLine.h"
#include "Misc/Base64.h"
#include "Misc/CoreDelegates.h"
#include "IDesktopPlatform.h"
#include "JsonObjectConverter.h"
#include "LevelEditor.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraNodeIf.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "HAL/IConsoleManager.h"
#include "Toolkits/AssetEditorManager.h"

#define LOCTEXT_NAMESPACE "ScriptImporter"

DEFINE_LOG_CATEGORY_STATIC(LogNiagaraJsonImporter, Log, All);

namespace ScriptImporter
{
struct FImportReport
{
    int32 Objects = 0;
    int32 Nodes = 0;
    int32 Pins = 0;
    int32 Links = 0;
    TArray<FString> Warnings;
    TArray<FString> Errors;

    FString Format() const
    {
        FString Result = FString::Printf(TEXT("Created %d objects, %d nodes, %d pins and %d links."), Objects, Nodes, Pins, Links);
        if (Warnings.Num()) Result += FString::Printf(TEXT("\n\nWarnings (%d):\n%s"), Warnings.Num(), *FString::Join(Warnings, TEXT("\n")));
        if (Errors.Num()) Result += FString::Printf(TEXT("\n\nErrors (%d):\n%s"), Errors.Num(), *FString::Join(Errors, TEXT("\n")));
        return Result;
    }
};

static bool JsonVariable(const TSharedPtr<FJsonObject>& Json, FNiagaraVariable& OutVariable, FImportReport& Report);
static FNiagaraTypeDefinition ResolveNiagaraType(const TSharedPtr<FJsonObject>& TypeJson, FImportReport& Report);

static void WriteReportToOutputLog(const FString& Filename, const FImportReport& Report)
{
    UE_LOG(LogNiagaraJsonImporter, Display, TEXT("Import finished: %s (%d objects, %d nodes, %d pins, %d links)"),
        *Filename, Report.Objects, Report.Nodes, Report.Pins, Report.Links);
    for (const FString& Warning : Report.Warnings)
        UE_LOG(LogNiagaraJsonImporter, Warning, TEXT("%s"), *Warning);
    for (const FString& Error : Report.Errors)
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("%s"), *Error);
}

static FString CleanName(FString Name)
{
    int32 Bracket;
    if (Name.FindChar(TEXT('['), Bracket)) Name.LeftInline(Bracket);
    return Name;
}

static bool ExportIndex(const TSharedPtr<FJsonObject>& Ref, int32& OutIndex)
{
    FString Path;
    if (!Ref.IsValid() || !Ref->TryGetStringField(TEXT("ObjectPath"), Path)) return false;
    int32 Dot;
    return Path.FindLastChar(TEXT('.'), Dot) && LexTryParseString(OutIndex, *Path.Mid(Dot + 1));
}

static UObject* LoadFModelReference(const TSharedPtr<FJsonObject>& Ref)
{
    if (!Ref.IsValid()) return nullptr;
    FString ObjectPath;
    FString ObjectName;
    Ref->TryGetStringField(TEXT("ObjectPath"), ObjectPath);
    Ref->TryGetStringField(TEXT("ObjectName"), ObjectName);
    int32 Quote = INDEX_NONE;
    if (ObjectName.FindChar(TEXT('\''), Quote)) ObjectName = ObjectName.Mid(Quote + 1).Replace(TEXT("'"), TEXT(""));
    if (ObjectPath.StartsWith(TEXT("/Script/")))
        return LoadObject<UObject>(nullptr, *(ObjectPath + TEXT(".") + ObjectName));
    int32 Dot = INDEX_NONE;
    if (ObjectPath.FindLastChar(TEXT('.'), Dot)) ObjectPath.LeftInline(Dot);
    if (!ObjectPath.IsEmpty() && !ObjectName.IsEmpty())
        if (UObject* Loaded = LoadObject<UObject>(nullptr, *(ObjectPath + TEXT(".") + ObjectName))) return Loaded;
    if (!ObjectName.IsEmpty())
    {
        TArray<FAssetData> Candidates;
        FAssetRegistryModule::GetRegistry().GetAssetsByClass(UNiagaraScript::StaticClass()->GetFName(), Candidates, true);
        for (const FAssetData& Candidate : Candidates)
            if (Candidate.AssetName.ToString().Equals(ObjectName, ESearchCase::IgnoreCase)) return Candidate.GetAsset();
    }
    return nullptr;
}

static UClass* ResolveClass(const FString& Type)
{
    // Static switches exist in every supported engine version, but their
    // reflected layout has changed several times. Never let class lookup
    // degrade them to a generic/missing Niagara node.
    if (Type == TEXT("NiagaraNodeStaticSwitch")) return UNiagaraNodeStaticSwitch::StaticClass();
    // UNiagaraNodeSelect was introduced after this 4.26 branch. A boolean
    // Select is semantically identical to the native UNiagaraNodeIf.
    if (Type == TEXT("NiagaraNodeSelect")) return UNiagaraNodeIf::StaticClass();
    // Convert nodes from the 41.30 graph contain type/layout combinations
    // which 17.XX dereferences unsafely during PostLoad. Preserve their graph
    // footprint as generic nodes instead of creating a crashable object.
    if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, *Type)) return Found;
    for (const TCHAR* Module : { TEXT("NiagaraEditor"), TEXT("Niagara"), TEXT("Engine") })
    {
        if (UClass* Found = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), Module, *Type))) return Found;
    }
    return nullptr;
}

static TSharedPtr<FJsonValue> Normalize(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) return Value;
    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Source = Value->AsObject();
        FString Path;
        if (Source->TryGetStringField(TEXT("ObjectPath"), Path)) return MakeShared<FJsonValueString>(Path);
        TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
        for (const auto& Pair : Source->Values) Copy->SetField(CleanName(Pair.Key), Normalize(Pair.Value));
        return MakeShared<FJsonValueObject>(Copy);
    }
    if (Value->Type == EJson::Array)
    {
        TArray<TSharedPtr<FJsonValue>> Copy;
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray()) Copy.Add(Normalize(Item));
        return MakeShared<FJsonValueArray>(Copy);
    }
    return Value;
}

static void ApplyScalarAndStructProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    if (!Target || !Properties.IsValid()) return;
    for (const auto& Pair : Properties->Values)
    {
        const FString Name = CleanName(Pair.Key);
        if (Target->GetClass()->GetName() == TEXT("NiagaraNodeStaticSwitch") && (Name == TEXT("OutputVars") || Name == TEXT("SwitchTypeData"))) continue;
        if (Name == TEXT("PropagatedStaticSwitchParameters")) continue;
        FProperty* Property = Target->GetClass()->FindPropertyByName(*Name);
        if (!Property)
        {
            Report.Warnings.AddUnique(FString::Printf(TEXT("%s: unsupported property %s"), *Target->GetClass()->GetName(), *Name));
            continue;
        }
        if (Property->IsA<FObjectPropertyBase>() || Name == TEXT("Nodes") || Name == TEXT("NodeGraph")) continue;
        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == FNiagaraTypeDefinition::StaticStruct() && Pair.Value->Type == EJson::Object)
            {
                *reinterpret_cast<FNiagaraTypeDefinition*>(Property->ContainerPtrToValuePtr<void>(Target)) = ResolveNiagaraType(Pair.Value->AsObject(), Report);
                continue;
            }
            if (StructProperty->Struct == FNiagaraVariable::StaticStruct() && Pair.Value->Type == EJson::Object)
            {
                JsonVariable(Pair.Value->AsObject(), *reinterpret_cast<FNiagaraVariable*>(Property->ContainerPtrToValuePtr<void>(Target)), Report);
                continue;
            }
            if (StructProperty->Struct == FNiagaraFunctionSignature::StaticStruct() && Pair.Value->Type == EJson::Object)
            {
                FNiagaraFunctionSignature* Signature = reinterpret_cast<FNiagaraFunctionSignature*>(Property->ContainerPtrToValuePtr<void>(Target));
                const TSharedPtr<FJsonObject> SignatureJson = Pair.Value->AsObject();
                FString SignatureName;
                if (SignatureJson->TryGetStringField(TEXT("Name"), SignatureName)) Signature->Name = *SignatureName;
                for (const TCHAR* Field : { TEXT("Inputs"), TEXT("Outputs") })
                {
                    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
                    if (!SignatureJson->TryGetArrayField(Field, Values)) continue;
                    TArray<FNiagaraVariable>& Variables = FCString::Strcmp(Field, TEXT("Inputs")) == 0 ? Signature->Inputs : Signature->Outputs;
                    Variables.Reset(Values->Num());
                    for (const TSharedPtr<FJsonValue>& Value : *Values) { FNiagaraVariable Variable; if (Value->Type == EJson::Object && JsonVariable(Value->AsObject(), Variable, Report)) Variables.Add(Variable); }
                }
                continue;
            }
        }
        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProperty->Inner);
            if (InnerStruct && InnerStruct->Struct == FNiagaraVariable::StaticStruct() && Pair.Value->Type == EJson::Array)
            {
                FScriptArrayHelper Array(ArrayProperty, Property->ContainerPtrToValuePtr<void>(Target));
                Array.EmptyValues();
                for (const TSharedPtr<FJsonValue>& Value : Pair.Value->AsArray())
                {
                    const int32 Index = Array.AddValue();
                    if (Value->Type == EJson::Object) JsonVariable(Value->AsObject(), *reinterpret_cast<FNiagaraVariable*>(Array.GetRawPtr(Index)), Report);
                }
                continue;
            }
        }
        if (!FJsonObjectConverter::JsonValueToUProperty(Normalize(Pair.Value), Property, Property->ContainerPtrToValuePtr<void>(Target)))
            Report.Warnings.Add(FString::Printf(TEXT("%s: could not set %s"), *Target->GetPathName(), *Name));
    }
    // FModel suffixes duplicate serialized fields (for example NodeGuid[21]).
    // JsonObjectConverter does not reliably convert its string GUID form in
    // this engine version, so preserve core graph-node identity explicitly.
    if (UEdGraphNode* Node = Cast<UEdGraphNode>(Target))
    {
        for (const auto& Pair : Properties->Values)
        {
            const FString Name = CleanName(Pair.Key);
            if (Name == TEXT("NodeGuid") && Pair.Value->Type == EJson::String) FGuid::Parse(Pair.Value->AsString(), Node->NodeGuid);
            else if (Name == TEXT("NodePosX") && Pair.Value->Type == EJson::Number) Node->NodePosX = (int32)Pair.Value->AsNumber();
            else if (Name == TEXT("NodePosY") && Pair.Value->Type == EJson::Number) Node->NodePosY = (int32)Pair.Value->AsNumber();
        }
        if (!Node->NodeGuid.IsValid()) Node->CreateNewGuid();
    }
}

static void ApplyObjectProperties(UObject* Target, const TSharedPtr<FJsonObject>& Properties, const TMap<int32, UObject*>& Objects, FImportReport& Report)
{
    if (!Target || !Properties.IsValid()) return;
    for (const auto& Pair : Properties->Values)
    {
        FProperty* Property = Target->GetClass()->FindPropertyByName(*CleanName(Pair.Key));
        FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
        if (!ObjectProperty || Pair.Value->Type != EJson::Object) continue;
        int32 Index;
        UObject* Resolved = nullptr;
        if (ExportIndex(Pair.Value->AsObject(), Index))
            if (UObject* const* Found = Objects.Find(Index))
                if ((*Found)->IsA(ObjectProperty->PropertyClass)) Resolved = *Found;
        // Cross-version external Niagara scripts can load successfully while
        // containing no output usage recognized by 17.XX; its function-call
        // history code dereferences that missing output. Keep those references
        // unresolved unless the function is part of this imported object set.
        if (!Resolved && Property->GetName() != TEXT("FunctionScript")) Resolved = LoadFModelReference(Pair.Value->AsObject());
        if (Resolved && Resolved->IsA(ObjectProperty->PropertyClass)) ObjectProperty->SetObjectPropertyValue_InContainer(Target, Resolved);
        else Report.Warnings.Add(FString::Printf(TEXT("%s: unresolved reference %s"), *Target->GetPathName(), *Property->GetName()));
    }
}

static FGuid GuidField(const TSharedPtr<FJsonObject>& Json, const TCHAR* Name)
{
    FGuid Guid;
    FString Text;
    if (Json.IsValid() && Json->TryGetStringField(Name, Text)) FGuid::Parse(Text, Guid);
    return Guid;
}

static FNiagaraTypeDefinition ResolveNiagaraType(const TSharedPtr<FJsonObject>& TypeJson, FImportReport& Report)
{
    const TSharedPtr<FJsonObject>* TypeRef = nullptr;
    if (!TypeJson.IsValid() || !TypeJson->TryGetObjectField(TEXT("ClassStructOrEnum"), TypeRef))
        return FNiagaraTypeDefinition::GetIntDef();
    FString ObjectName;
    FString ObjectPath;
    (*TypeRef)->TryGetStringField(TEXT("ObjectName"), ObjectName);
    (*TypeRef)->TryGetStringField(TEXT("ObjectPath"), ObjectPath);
    int32 Quote = INDEX_NONE;
    if (ObjectName.FindChar(TEXT('\''), Quote)) ObjectName = ObjectName.Mid(Quote + 1).Replace(TEXT("'"), TEXT(""));
    if (ObjectName == TEXT("NiagaraParameterMap")) return FNiagaraTypeDefinition::GetParameterMapDef();
    if (ObjectName == TEXT("NiagaraInt32")) return FNiagaraTypeDefinition::GetIntDef();
    if (ObjectName == TEXT("NiagaraFloat")) return FNiagaraTypeDefinition::GetFloatDef();
    if (ObjectName == TEXT("NiagaraBool")) return FNiagaraTypeDefinition::GetBoolDef();
    if (ObjectName == TEXT("NiagaraNumeric")) return FNiagaraTypeDefinition::GetGenericNumericDef();
    if (ObjectName == TEXT("Vector2f")) return FNiagaraTypeDefinition::GetVec2Def();
    if (ObjectName == TEXT("Vector3f") || ObjectName == TEXT("NiagaraPosition")) return FNiagaraTypeDefinition::GetVec3Def();
    if (ObjectName == TEXT("Vector4f")) return FNiagaraTypeDefinition::GetVec4Def();
    if (ObjectName == TEXT("Quat4f") || ObjectName == TEXT("Quat")) return FNiagaraTypeDefinition::GetQuatDef();
    if (ObjectName == TEXT("NiagaraMatrix") || ObjectName == TEXT("Matrix44f")) return FNiagaraTypeDefinition::GetMatrix4Def();
    if (ObjectName == TEXT("LinearColor")) return FNiagaraTypeDefinition::GetColorDef();
    if (ObjectName == TEXT("NiagaraID")) return FNiagaraTypeDefinition::GetIDDef();
    if (!ObjectPath.IsEmpty())
    {
        UObject* Resolved = LoadFModelReference(*TypeRef);
        if (UEnum* Enum = Cast<UEnum>(Resolved)) return FNiagaraTypeDefinition(Enum);
        if (UScriptStruct* Struct = Cast<UScriptStruct>(Resolved)) return FNiagaraTypeDefinition(Struct);
        if (UClass* Class = Cast<UClass>(Resolved)) return FNiagaraTypeDefinition(Class);
    }
    Report.Warnings.AddUnique(FString::Printf(TEXT("Niagara type is unavailable in 17.XX and was replaced with int: %s"), *ObjectName));
    return FNiagaraTypeDefinition::GetIntDef();
}

static bool JsonVariable(const TSharedPtr<FJsonObject>& Json, FNiagaraVariable& OutVariable, FImportReport& Report)
{
    if (!Json.IsValid()) return false;
    FString Name;
    const TSharedPtr<FJsonObject>* Type = nullptr;
    if (!Json->TryGetStringField(TEXT("Name"), Name) || !Json->TryGetObjectField(TEXT("TypeDef"), Type)) return false;
    OutVariable = FNiagaraVariable(ResolveNiagaraType(*Type, Report), *Name);
    TArray<uint8> Bytes;
    FString Base64;
    if (Json->TryGetStringField(TEXT("VarData"), Base64) && !Base64.IsEmpty()) FBase64::Decode(Base64, Bytes);
    if (Bytes.Num())
    {
        OutVariable.AllocateData();
        FMemory::Memcpy(OutVariable.GetData(), Bytes.GetData(), FMath::Min(Bytes.Num(), OutVariable.GetSizeInBytes()));
    }
    return true;
}

static void ApplyScriptVariable(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    if (!Object || !Properties.IsValid()) return;
    // The class lives in NiagaraEditor's private API in this engine branch, so
    // use its reflected object plus the matching in-engine layout without
    // linking against non-exported class functions.
    UNiagaraScriptVariable* Target = reinterpret_cast<UNiagaraScriptVariable*>(Object);
    FString DefaultMode;
    if (Properties->TryGetStringField(TEXT("DefaultMode"), DefaultMode))
    {
        if (DefaultMode.EndsWith(TEXT("::Binding"))) Target->DefaultMode = ENiagaraDefaultMode::Binding;
        else if (DefaultMode.EndsWith(TEXT("::Custom"))) Target->DefaultMode = ENiagaraDefaultMode::Custom;
        else Target->DefaultMode = ENiagaraDefaultMode::Value; // includes newer FailIfPreviouslyNotSet
    }
    const TSharedPtr<FJsonObject>* Variable = nullptr;
    if (Properties->TryGetObjectField(TEXT("Variable"), Variable)) JsonVariable(*Variable, Target->Variable, Report);

    // DefaultValueVariant is the authoritative value in newer Niagara. Copy it
    // into the old FNiagaraVariable buffer used by 17.XX.
    const TSharedPtr<FJsonObject>* Variant = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* ByteValues = nullptr;
    if (Properties->TryGetObjectField(TEXT("DefaultValueVariant"), Variant) && (*Variant)->TryGetArrayField(TEXT("Bytes"), ByteValues) && ByteValues->Num())
    {
        Target->Variable.AllocateData();
        const int32 Count = FMath::Min(ByteValues->Num(), Target->Variable.GetSizeInBytes());
        for (int32 Index = 0; Index < Count; ++Index) Target->Variable.GetData()[Index] = (uint8)(*ByteValues)[Index]->AsNumber();
    }

    // Convert FModel's structured localized text to the FText representation
    // understood by this older engine, then let reflection handle the rest.
    TSharedRef<FJsonObject> Compatible = MakeShared<FJsonObject>();
    for (const auto& Pair : Properties->Values)
    {
        if (Pair.Key == TEXT("Variable") || Pair.Key == TEXT("DefaultValueVariant") || Pair.Key == TEXT("DefaultMode")) continue;
        if (Pair.Key == TEXT("Metadata") && Pair.Value->Type == EJson::Object)
        {
            TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>(*Pair.Value->AsObject());
            const TSharedPtr<FJsonObject>* Description = nullptr;
            if (Metadata->TryGetObjectField(TEXT("Description"), Description))
            {
                FString Text;
                if (!(*Description)->TryGetStringField(TEXT("LocalizedString"), Text)) (*Description)->TryGetStringField(TEXT("SourceString"), Text);
                Metadata->SetStringField(TEXT("Description"), Text);
            }
            Compatible->SetObjectField(Pair.Key, Metadata);
        }
        else Compatible->SetField(Pair.Key, Pair.Value);
    }
    ApplyScalarAndStructProperties(Object, Compatible, Report);

    double StaticSwitch = 0;
    if (Properties->TryGetNumberField(TEXT("StaticSwitchDefaultValue"), StaticSwitch))
    {
        Target->Metadata.SetIsStaticSwitch(true);
        Target->Metadata.SetStaticSwitchDefaultValue((int32)StaticSwitch);
    }
}

static void FillPin(UEdGraphPin* Pin, const TSharedPtr<FJsonObject>& Json, FImportReport& Report)
{
    FString Text;
    if (Json->TryGetStringField(TEXT("PinName"), Text)) Pin->PinName = *Text;
    if (Json->TryGetStringField(TEXT("Direction"), Text)) Pin->Direction = Text.Contains(TEXT("Output")) ? EGPD_Output : EGPD_Input;
    Pin->PinId = GuidField(Json, TEXT("PinId"));
    Pin->PersistentGuid = GuidField(Json, TEXT("PersistentGuid"));
    if (Json->TryGetStringField(TEXT("PinToolTip"), Text)) Pin->PinToolTip = Text;
    if (Json->TryGetStringField(TEXT("DefaultValue"), Text)) Pin->DefaultValue = Text;
    if (Json->TryGetStringField(TEXT("AutogeneratedDefaultValue"), Text)) Pin->AutogeneratedDefaultValue = Text;
    const TSharedPtr<FJsonObject>* Type = nullptr;
    if (Json->TryGetObjectField(TEXT("PinType"), Type))
    {
        // Do not feed object references to JsonObjectToUStruct.  FModel's
        // reference objects are not Unreal object paths and can partially
        // initialize TWeakObjectPtr with invalid data.
        Pin->PinType = FEdGraphPinType();
        if ((*Type)->TryGetStringField(TEXT("PinCategory"), Text)) Pin->PinType.PinCategory = *Text;
        if ((*Type)->TryGetStringField(TEXT("PinSubCategory"), Text)) Pin->PinType.PinSubCategory = *Text;
        if ((*Type)->TryGetStringField(TEXT("ContainerType"), Text))
        {
            if (Text == TEXT("Array")) Pin->PinType.ContainerType = EPinContainerType::Array;
            else if (Text == TEXT("Set")) Pin->PinType.ContainerType = EPinContainerType::Set;
            else if (Text == TEXT("Map")) Pin->PinType.ContainerType = EPinContainerType::Map;
        }
        bool Flag = false;
        if ((*Type)->TryGetBoolField(TEXT("bIsReference"), Flag)) Pin->PinType.bIsReference = Flag;
        if ((*Type)->TryGetBoolField(TEXT("bIsConst"), Flag)) Pin->PinType.bIsConst = Flag;
        if ((*Type)->TryGetBoolField(TEXT("bIsWeakPointer"), Flag)) Pin->PinType.bIsWeakPointer = Flag;
        if ((*Type)->TryGetBoolField(TEXT("bIsUObjectWrapper"), Flag)) Pin->PinType.bIsUObjectWrapper = Flag;

        const TSharedPtr<FJsonObject>* SubObject = nullptr;
        if ((*Type)->TryGetObjectField(TEXT("PinSubCategoryObject"), SubObject))
        {
            FString ObjectName;
            FString ObjectPath;
            (*SubObject)->TryGetStringField(TEXT("ObjectName"), ObjectName);
            (*SubObject)->TryGetStringField(TEXT("ObjectPath"), ObjectPath);
            int32 FirstQuote = INDEX_NONE;
            if (ObjectName.FindChar(TEXT('\''), FirstQuote)) ObjectName = ObjectName.Mid(FirstQuote + 1).Replace(TEXT("'"), TEXT(""));
            const FNiagaraTypeDefinition* Canonical = nullptr;
            if (ObjectName == TEXT("NiagaraParameterMap")) Canonical = &FNiagaraTypeDefinition::GetParameterMapDef();
            else if (ObjectName == TEXT("NiagaraInt32")) Canonical = &FNiagaraTypeDefinition::GetIntDef();
            else if (ObjectName == TEXT("NiagaraFloat")) Canonical = &FNiagaraTypeDefinition::GetFloatDef();
            else if (ObjectName == TEXT("NiagaraBool")) Canonical = &FNiagaraTypeDefinition::GetBoolDef();
            else if (ObjectName == TEXT("NiagaraNumeric")) Canonical = &FNiagaraTypeDefinition::GetGenericNumericDef();
            else if (ObjectName == TEXT("Vector3f") || ObjectName == TEXT("NiagaraPosition")) Canonical = &FNiagaraTypeDefinition::GetVec3Def();
            else if (ObjectName == TEXT("LinearColor")) Canonical = &FNiagaraTypeDefinition::GetColorDef();
            else if (ObjectName == TEXT("NiagaraID")) Canonical = &FNiagaraTypeDefinition::GetIDDef();

            if (Canonical)
            {
                Pin->PinType = GetDefault<UEdGraphSchema_Niagara>()->TypeDefinitionToPinType(*Canonical);
            }
            else
            {
                UObject* Resolved = nullptr;
                if (!ObjectPath.IsEmpty()) Resolved = LoadObject<UObject>(nullptr, *ObjectPath);
                if (UEnum* Enum = Cast<UEnum>(Resolved))
                    Pin->PinType = GetDefault<UEdGraphSchema_Niagara>()->TypeDefinitionToPinType(FNiagaraTypeDefinition(Enum));
                else if (UScriptStruct* Struct = Cast<UScriptStruct>(Resolved))
                    Pin->PinType = GetDefault<UEdGraphSchema_Niagara>()->TypeDefinitionToPinType(FNiagaraTypeDefinition(Struct));
                else if (UClass* Class = Cast<UClass>(Resolved))
                    Pin->PinType = GetDefault<UEdGraphSchema_Niagara>()->TypeDefinitionToPinType(FNiagaraTypeDefinition(Class));
                else
                {
                    // Niagara 17.XX assumes every Type-category pin resolves to
                    // a valid type and dereferences it while hashing the graph.
                    // Use canonical int as a safe compatibility placeholder.
                    Pin->PinType = GetDefault<UEdGraphSchema_Niagara>()->TypeDefinitionToPinType(FNiagaraTypeDefinition::GetIntDef());
                    Report.Warnings.AddUnique(FString::Printf(TEXT("Pin type is unavailable in 17.XX and was replaced with int: %s"), *ObjectName));
                }
            }
        }
    }
}

static UNiagaraNodeOutput* FindMatchingOutput(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, const FGuid& UsageId = FGuid())
{
    if (!Graph) return nullptr;
    for (UEdGraphNode* GraphNode : Graph->Nodes)
        if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(GraphNode))
            if (Output->GetUsage() == Usage && (!UsageId.IsValid() || Output->GetUsageId() == UsageId)) return Output;
    return nullptr;
}

static void RunSystemInsertionTest(const TArray<FString>& Args)
{
    if (Args.Num() < 2)
    {
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("NIAGARA_JSON_SYSTEM_TEST: expected <system path> <module path>"));
        return;
    }
    UNiagaraSystem* SourceSystem = LoadObject<UNiagaraSystem>(nullptr, *Args[0]);
    UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *Args[1]);
    if (!SourceSystem || !ModuleScript)
    {
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("NIAGARA_JSON_SYSTEM_TEST: asset load failed"));
        return;
    }
    UNiagaraScriptSource* ModuleSource = Cast<UNiagaraScriptSource>(ModuleScript->GetSource());
    UNiagaraNodeOutput* CallableOutput = ModuleSource && ModuleSource->NodeGraph
        ? FindMatchingOutput(ModuleSource->NodeGraph, ENiagaraScriptUsage::Module) : nullptr;
    if (!CallableOutput)
    {
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("NIAGARA_JSON_SYSTEM_TEST: module graph has no callable Module output"));
        return;
    }
    UE_LOG(LogNiagaraJsonImporter, Display, TEXT("NIAGARA_JSON_SYSTEM_TEST: callable Module output verified: %s"), *CallableOutput->GetPathName());

    UPackage* TestPackage = GetTransientPackage();
    UNiagaraSystem* TestSystem = Cast<UNiagaraSystem>(StaticDuplicateObject(SourceSystem, TestPackage,
        *FString::Printf(TEXT("NJI_SystemTest_%llu"), FPlatformTime::Cycles64())));
    TArray<UObject*> SystemObjects;
    GetObjectsWithOuter(TestSystem, SystemObjects, true);
    UNiagaraNodeOutput* TargetOutput = nullptr;
    for (UObject* Object : SystemObjects)
        if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Object))
            if (Output->GetUsage() == ENiagaraScriptUsage::ParticleUpdateScript)
            { TargetOutput = Output; break; }
    if (!TargetOutput || !TargetOutput->GetGraph())
    {
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("NIAGARA_JSON_SYSTEM_TEST: duplicate has no particle-update output"));
        return;
    }

    UEdGraph* Graph = TargetOutput->GetGraph();
    FGraphNodeCreator<UNiagaraNodeFunctionCall> Creator(*Graph);
    UNiagaraNodeFunctionCall* FunctionCall = Creator.CreateNode();
    FunctionCall->FunctionScript = ModuleScript;
    Creator.Finalize();

    const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
    UEdGraphPin* TargetInput = nullptr;
    UEdGraphPin* ModuleInput = nullptr;
    UEdGraphPin* ModuleOutput = nullptr;
    for (UEdGraphPin* Pin : TargetOutput->Pins)
        if (Pin && Pin->Direction == EGPD_Input && Schema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
        { TargetInput = Pin; break; }
    for (UEdGraphPin* Pin : FunctionCall->Pins)
        if (Pin && Schema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
        {
            if (Pin->Direction == EGPD_Input) ModuleInput = Pin;
            else if (Pin->Direction == EGPD_Output) ModuleOutput = Pin;
        }
    UEdGraphPin* PreviousOutput = TargetInput && TargetInput->LinkedTo.Num() ? TargetInput->LinkedTo[0] : nullptr;
    if (!TargetInput || !PreviousOutput || !ModuleInput || !ModuleOutput)
    {
        UE_LOG(LogNiagaraJsonImporter, Error, TEXT("NIAGARA_JSON_SYSTEM_TEST: could not resolve stack parameter-map pins"));
        return;
    }
    TargetInput->BreakAllPinLinks();
    PreviousOutput->MakeLinkTo(ModuleInput);
    ModuleOutput->MakeLinkTo(TargetInput);
    UE_LOG(LogNiagaraJsonImporter, Display, TEXT("NIAGARA_JSON_SYSTEM_TEST: module inserted; opening transient system"));
    FAssetEditorManager::Get().OpenEditorForAsset(TestSystem);
    UE_LOG(LogNiagaraJsonImporter, Display, TEXT("NIAGARA_JSON_SYSTEM_TEST: PASS stack editor opened"));
}

static FAutoConsoleCommand GNiagaraJsonSystemTestCommand(
    TEXT("NiagaraJson.TestSystemInsert"),
    TEXT("Insert a module into a transient duplicate of a Niagara system and open its stack editor."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunSystemInsertionTest));

static UEnum* ResolveOrCreateSwitchEnum(UObject* Owner, const TSharedPtr<FJsonObject>& EnumRef, int32 OptionCount, FImportReport& Report)
{
    FString ObjectName;
    FString ObjectPath;
    if (EnumRef.IsValid())
    {
        EnumRef->TryGetStringField(TEXT("ObjectName"), ObjectName);
        EnumRef->TryGetStringField(TEXT("ObjectPath"), ObjectPath);
    }
    int32 Quote = INDEX_NONE;
    if (ObjectName.FindChar(TEXT('\''), Quote)) ObjectName = ObjectName.Mid(Quote + 1).Replace(TEXT("'"), TEXT(""));
    if (UEnum* Existing = FindObject<UEnum>(ANY_PACKAGE, *ObjectName))
        if (Existing->NumEnums() - 1 == FMath::Max(OptionCount, 2)) return Existing;
    if (ObjectPath.StartsWith(TEXT("/Script/")))
    {
        FString Module = ObjectPath.Mid(8);
        if (UEnum* Existing = LoadObject<UEnum>(nullptr, *(TEXT("/Script/") + Module + TEXT(".") + ObjectName)))
            if (Existing->NumEnums() - 1 == FMath::Max(OptionCount, 2)) return Existing;
    }

    const FString SafeName = ObjectName.IsEmpty() ? TEXT("ImportedStaticSwitchEnum") : ObjectName;
    // Keep compatibility enums at package scope. Niagara duplicates script
    // sources/graphs into transient packages while compiling a module in a
    // system; an enum nested below the graph is duplicated too, registering
    // the same qualified value names repeatedly and destabilizing compilation.
    UObject* EnumOuter = Owner ? static_cast<UObject*>(Owner->GetOutermost()) : nullptr;
    if (!EnumOuter) EnumOuter = Owner;
    if (EnumOuter)
        if (UEnum* Shared = FindObject<UEnum>(EnumOuter, *SafeName))
            if (Shared->NumEnums() - 1 == FMath::Max(OptionCount, 2)) return Shared;
    UEnum* Synthetic = NewObject<UEnum>(EnumOuter, *SafeName, RF_Transactional);
    TArray<TPair<FName, int64>> Values;
    const int32 Count = FMath::Max(OptionCount, 2);
    for (int32 Index = 0; Index < Count; ++Index)
        Values.Emplace(*FString::Printf(TEXT("%s::Option%d"), *SafeName, Index), Index);
    Values.Emplace(*FString::Printf(TEXT("%s::%s_MAX"), *SafeName, *SafeName), Count);
    Synthetic->SetEnums(Values, UEnum::ECppForm::Namespaced);
    Report.Warnings.AddUnique(FString::Printf(TEXT("Static-switch enum %s is unavailable in 17.XX; synthesized a %d-option compatible enum"), *SafeName, Count));
    return Synthetic;
}

static void ApplyStaticSwitchType(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    UNiagaraNodeStaticSwitch* Node = Cast<UNiagaraNodeStaticSwitch>(Object);
    if (!Node || !Properties.IsValid()) return;
    FString InputName;
    if (Properties->TryGetStringField(TEXT("InputParameterName"), InputName)) Node->InputParameterName = *InputName;
    double OptionsValue = 2;
    Properties->TryGetNumberField(TEXT("NumOptionsPerVariable"), OptionsValue);
    const int32 OptionCount = FMath::Max(2, (int32)OptionsValue);
    const TArray<TSharedPtr<FJsonValue>>* OutputVariables = nullptr;
    if (Properties->TryGetArrayField(TEXT("OutputVars"), OutputVariables))
    {
        Node->OutputVars.Reset(OutputVariables->Num());
        for (const TSharedPtr<FJsonValue>& Value : *OutputVariables)
        {
            FNiagaraVariable Variable;
            if (Value->Type == EJson::Object && JsonVariable(Value->AsObject(), Variable, Report)) Node->OutputVars.Add(Variable);
        }
    }

    const TSharedPtr<FJsonObject>* SwitchData = nullptr;
    if (Properties->TryGetObjectField(TEXT("SwitchTypeData"), SwitchData))
    {
        FString SwitchType;
        (*SwitchData)->TryGetStringField(TEXT("SwitchType"), SwitchType);
        if (SwitchType.EndsWith(TEXT("::Integer"))) Node->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Integer;
        else if (SwitchType.EndsWith(TEXT("::Enum"))) Node->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Enum;
        else Node->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Bool;
        double MaxInt = OptionCount - 1;
        (*SwitchData)->TryGetNumberField(TEXT("MaxIntCount"), MaxInt);
        Node->SwitchTypeData.MaxIntCount = (int32)MaxInt;
        FString Constant;
        if ((*SwitchData)->TryGetStringField(TEXT("SwitchConstant"), Constant)) Node->SwitchTypeData.SwitchConstant = *Constant;
        const TSharedPtr<FJsonObject>* EnumRef = nullptr;
        if (Node->SwitchTypeData.SwitchType == ENiagaraStaticSwitchType::Enum && (*SwitchData)->TryGetObjectField(TEXT("Enum"), EnumRef))
            Node->SwitchTypeData.Enum = ResolveOrCreateSwitchEnum(Node, *EnumRef, OptionCount, Report);
        if (Node->SwitchTypeData.SwitchType == ENiagaraStaticSwitchType::Enum && !Node->SwitchTypeData.Enum)
            Node->SwitchTypeData.Enum = ResolveOrCreateSwitchEnum(Node, nullptr, OptionCount, Report);
    }
    else
    {
        // New boolean switches omit the type struct because Bool is the default.
        Node->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Bool;
        Node->SwitchTypeData.MaxIntCount = 1;
    }
}

static void ApplyFunctionCallCompatibility(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    if (!Object || !Object->IsA<UNiagaraNodeFunctionCall>() || !Properties.IsValid()) return;
    UNiagaraNodeFunctionCall* Node = CastChecked<UNiagaraNodeFunctionCall>(Object);
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Properties->TryGetArrayField(TEXT("PropagatedStaticSwitchParameters"), Values)) return;
    Node->PropagatedStaticSwitchParameters.Reset(Values->Num());
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        if (Value->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> EntryJson = Value->AsObject();
        const TSharedPtr<FJsonObject>* SwitchJson = nullptr;
        FNiagaraVariable Switch;
        if (!EntryJson->TryGetObjectField(TEXT("SwitchParameter"), SwitchJson) || !JsonVariable(*SwitchJson, Switch, Report)) continue;
        FNiagaraPropagatedVariable Entry(Switch);
        EntryJson->TryGetStringField(TEXT("PropagatedName"), Entry.PropagatedName);
        Node->PropagatedStaticSwitchParameters.Add(MoveTemp(Entry));
    }
}

static void ApplySelectCompatibility(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    UNiagaraNodeIf* Node = Cast<UNiagaraNodeIf>(Object);
    if (!Node || !Properties.IsValid()) return;
    const TSharedPtr<FJsonObject>* SelectorTypeJson = nullptr;
    if (Properties->TryGetObjectField(TEXT("SelectorPinType"), SelectorTypeJson))
    {
        const FNiagaraTypeDefinition SelectorType = ResolveNiagaraType(*SelectorTypeJson, Report);
        if (SelectorType != FNiagaraTypeDefinition::GetBoolDef())
        {
            Report.Warnings.AddUnique(FString::Printf(TEXT("%s uses a non-boolean Select unsupported by 17.XX; imported as a disconnected compatibility If"), *Node->GetName()));
            return;
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* OutputVariables = nullptr;
    if (Properties->TryGetArrayField(TEXT("OutputVars"), OutputVariables))
    {
        Node->OutputVars.Reset(OutputVariables->Num());
        for (const TSharedPtr<FJsonValue>& Value : *OutputVariables)
        {
            FNiagaraVariable Variable;
            if (Value->Type == EJson::Object && JsonVariable(Value->AsObject(), Variable, Report)) Node->OutputVars.Add(Variable);
        }
    }
}

static void ApplyConvertCompatibility(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FImportReport& Report)
{
    if (!Object || Object->GetClass()->GetName() != TEXT("NiagaraNodeConvert") || !Properties.IsValid()) return;
    const TArray<TSharedPtr<FJsonValue>>* JsonConnections = nullptr;
    if (!Properties->TryGetArrayField(TEXT("Connections"), JsonConnections)) return;
    FArrayProperty* ConnectionsProperty = FindField<FArrayProperty>(Object->GetClass(), TEXT("Connections"));
    FStructProperty* ConnectionStructProperty = ConnectionsProperty ? CastField<FStructProperty>(ConnectionsProperty->Inner) : nullptr;
    if (!ConnectionsProperty || !ConnectionStructProperty)
    {
        Report.Warnings.AddUnique(FString::Printf(TEXT("%s has no compatible Connections property"), *Object->GetName()));
        return;
    }
    FScriptArrayHelper Connections(ConnectionsProperty, ConnectionsProperty->ContainerPtrToValuePtr<void>(Object));
    Connections.EmptyValues();
    auto SetGuid = [&](void* ConnectionData, const TCHAR* PropertyName, const FGuid& Guid)
    {
        if (FStructProperty* GuidProperty = FindField<FStructProperty>(ConnectionStructProperty->Struct, PropertyName))
            GuidProperty->CopyCompleteValue(GuidProperty->ContainerPtrToValuePtr<void>(ConnectionData), &Guid);
    };
    auto SetPath = [&](void* ConnectionData, const TCHAR* PropertyName, const TSharedPtr<FJsonObject>& JsonConnection)
    {
        FArrayProperty* PathProperty = FindField<FArrayProperty>(ConnectionStructProperty->Struct, PropertyName);
        FNameProperty* NameProperty = PathProperty ? CastField<FNameProperty>(PathProperty->Inner) : nullptr;
        const TArray<TSharedPtr<FJsonValue>>* JsonPath = nullptr;
        if (!PathProperty || !NameProperty || !JsonConnection->TryGetArrayField(PropertyName, JsonPath)) return;
        FScriptArrayHelper Path(PathProperty, PathProperty->ContainerPtrToValuePtr<void>(ConnectionData));
        Path.EmptyValues();
        for (const TSharedPtr<FJsonValue>& PathPart : *JsonPath)
            if (PathPart->Type == EJson::String)
            {
                const int32 Index = Path.AddValue();
                NameProperty->SetPropertyValue(Path.GetRawPtr(Index), *PathPart->AsString());
            }
    };
    for (const TSharedPtr<FJsonValue>& Value : *JsonConnections)
    {
        if (Value->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> JsonConnection = Value->AsObject();
        const FGuid SourcePinId = GuidField(JsonConnection, TEXT("SourcePinId"));
        const FGuid DestinationPinId = GuidField(JsonConnection, TEXT("DestinationPinId"));
        if (!SourcePinId.IsValid() || !DestinationPinId.IsValid())
        {
            Report.Warnings.AddUnique(FString::Printf(TEXT("%s has a convert connection with an invalid pin id"), *Object->GetName()));
            continue;
        }
        const int32 Index = Connections.AddValue();
        void* ConnectionData = Connections.GetRawPtr(Index);
        SetGuid(ConnectionData, TEXT("SourcePinId"), SourcePinId);
        SetGuid(ConnectionData, TEXT("DestinationPinId"), DestinationPinId);
        SetPath(ConnectionData, TEXT("SourcePath"), JsonConnection);
        SetPath(ConnectionData, TEXT("DestinationPath"), JsonConnection);
    }
}

static UNiagaraScript* Import(const FString& Filename, const FString& PackageName, FImportReport& Report)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Filename)) { Report.Errors.Add(TEXT("Could not read JSON.")); return nullptr; }
    TArray<TSharedPtr<FJsonValue>> Exports;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Exports)) { Report.Errors.Add(TEXT("Invalid JSON export array.")); return nullptr; }

    int32 ScriptIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Exports.Num(); ++Index)
        if (Exports[Index]->AsObject()->GetStringField(TEXT("Type")) == TEXT("NiagaraScript")) { ScriptIndex = Index; break; }
    if (ScriptIndex == INDEX_NONE) { Report.Errors.Add(TEXT("No NiagaraScript export.")); return nullptr; }

    UPackage* Package = CreatePackage(*PackageName);
    UNiagaraScript* Script = NewObject<UNiagaraScript>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
    TMap<int32, UObject*> Objects;
    TSet<int32> MissingNodePlaceholders;
    Objects.Add(ScriptIndex, Script);
    ++Report.Objects;

    for (int32 Pass = 0; Pass < Exports.Num(); ++Pass)
    {
        bool Progress = false;
        for (int32 Index = 0; Index < Exports.Num(); ++Index)
        {
            if (Objects.Contains(Index)) continue;
            const TSharedPtr<FJsonObject> Json = Exports[Index]->AsObject();
            UObject* Outer = Script;
            const TSharedPtr<FJsonObject>* OuterJson = nullptr;
            if (Json->TryGetObjectField(TEXT("Outer"), OuterJson))
            {
                int32 OuterIndex;
                if (ExportIndex(*OuterJson, OuterIndex))
                {
                    UObject* const* Found = Objects.Find(OuterIndex);
                    if (!Found) continue;
                    Outer = *Found;
                }
            }
            const FString ExportType = Json->GetStringField(TEXT("Type"));
            UClass* Class = ResolveClass(ExportType);
            bool bMissingNodePlaceholder = false;
            if (!Class && ExportType.StartsWith(TEXT("NiagaraNode")))
            {
                Class = UEdGraphNode_Comment::StaticClass();
                bMissingNodePlaceholder = true;
                Report.Warnings.AddUnique(FString::Printf(TEXT("%s is unavailable in 17.XX; replaced with a missing-node comment"), *ExportType));
            }
            if (!Class) continue;
            UObject* Object = NewObject<UObject>(Outer, Class, *Json->GetStringField(TEXT("Name")), RF_Transactional);
            Objects.Add(Index, Object);
            if (bMissingNodePlaceholder)
            {
                MissingNodePlaceholders.Add(Index);
                UEdGraphNode_Comment* Comment = CastChecked<UEdGraphNode_Comment>(Object);
                Comment->NodeComment = FString::Printf(TEXT("MISSING NODE: %s (%s)"), *ExportType, *Json->GetStringField(TEXT("Name")));
                Comment->bCommentBubbleVisible = true;
            }
            ++Report.Objects;
            if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
            {
                if (UEdGraph* Graph = Cast<UEdGraph>(Outer)) Graph->Nodes.Add(Node);
                ++Report.Nodes;
            }
            Progress = true;
        }
        if (!Progress) break;
    }
    for (int32 Index = 0; Index < Exports.Num(); ++Index)
        if (!Objects.Contains(Index)) Report.Warnings.AddUnique(FString::Printf(TEXT("Unsupported class or outer: %s"), *Exports[Index]->AsObject()->GetStringField(TEXT("Type"))));

    for (const auto& Pair : Objects)
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties))
        {
            if (Pair.Value->GetClass()->GetName() == TEXT("NiagaraScriptVariable")) ApplyScriptVariable(Pair.Value, *Properties, Report);
            else ApplyScalarAndStructProperties(Pair.Value, *Properties, Report);
            // Newer exports serialize UNiagaraNodeOutput usage as ScriptType.
            // Generic reflection does not map that renamed enum reliably on
            // 17.XX, leaving a particle output in a callable module graph.
            if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Pair.Value))
            {
                FString ScriptType;
                if ((*Properties)->TryGetStringField(TEXT("ScriptType"), ScriptType) && ScriptType.EndsWith(TEXT("::Module")))
                    Output->SetUsage(ENiagaraScriptUsage::Module);
                else if (ScriptType.EndsWith(TEXT("::DynamicInput")))
                    Output->SetUsage(ENiagaraScriptUsage::DynamicInput);
            }
        }
    }

    for (const auto& Pair : Objects)
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties)) ApplyStaticSwitchType(Pair.Value, *Properties, Report);
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties)) ApplyFunctionCallCompatibility(Pair.Value, *Properties, Report);
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties)) ApplySelectCompatibility(Pair.Value, *Properties, Report);
    }
    for (const auto& Pair : Objects)
    {
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties))
        {
            ApplyObjectProperties(Pair.Value, *Properties, Objects, Report);
            ApplyConvertCompatibility(Pair.Value, *Properties, Report);
        }
    }
    // Object-reference application is the final generic property pass and can
    // restore a newer enum representation. Normalize callable output usage
    // only after that pass has finished.
    for (const auto& Pair : Objects)
        if (UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Pair.Value))
        {
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            FString ScriptType;
            if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties) &&
                (*Properties)->TryGetStringField(TEXT("ScriptType"), ScriptType))
            {
                if (ScriptType.EndsWith(TEXT("::Module"))) Output->SetUsage(ENiagaraScriptUsage::Module);
                else if (ScriptType.EndsWith(TEXT("::DynamicInput"))) Output->SetUsage(ENiagaraScriptUsage::DynamicInput);
                if (Output->GetUsage() == ENiagaraScriptUsage::Module || Output->GetUsage() == ENiagaraScriptUsage::Function ||
                    Output->GetUsage() == ENiagaraScriptUsage::DynamicInput)
                    Output->SetUsageId(FGuid());
            }
        }

    // 41.30 moved stack order/category into hierarchy objects. 17.XX has no
    // hierarchy classes, so translate their ordered GUID identities back onto
    // the older per-variable metadata representation.
    TMultiMap<FGuid, UObject*> VariablesByGuid;
    for (const auto& Pair : Objects)
    {
        if (Pair.Value->GetClass()->GetName() != TEXT("NiagaraScriptVariable")) continue;
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        const TSharedPtr<FJsonObject>* Metadata = nullptr;
        FString GuidText;
        FGuid Guid;
        if (Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties) &&
            (*Properties)->TryGetObjectField(TEXT("Metadata"), Metadata) && (*Metadata)->TryGetStringField(TEXT("VariableGuid"), GuidText) && FGuid::Parse(GuidText, Guid))
            VariablesByGuid.Add(Guid, Pair.Value);
    }
    TFunction<void(int32, const FString&, int32&)> ApplyHierarchy = [&](int32 ExportIndexValue, const FString& ParentCategory, int32& Priority)
    {
        if (!Exports.IsValidIndex(ExportIndexValue)) return;
        const TSharedPtr<FJsonObject> Item = Exports[ExportIndexValue]->AsObject();
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        if (!Item->TryGetObjectField(TEXT("Properties"), Properties)) return;
        const FString Type = Item->GetStringField(TEXT("Type"));
        FString Category = ParentCategory;
        if (Type == TEXT("NiagaraHierarchyScriptCategory")) (*Properties)->TryGetStringField(TEXT("Category"), Category);
        if (Type == TEXT("NiagaraHierarchyScriptParameter"))
        {
            const TSharedPtr<FJsonObject>* Identity = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Guids = nullptr;
            FGuid Guid;
            if ((*Properties)->TryGetObjectField(TEXT("Identity"), Identity) && (*Identity)->TryGetArrayField(TEXT("Guids"), Guids) && Guids->Num() &&
                FGuid::Parse((*Guids)[0]->AsString(), Guid))
            {
                TArray<UObject*> FoundVariables;
                VariablesByGuid.MultiFind(Guid, FoundVariables);
                for (UObject* Found : FoundVariables)
                {
                    UNiagaraScriptVariable* Variable = reinterpret_cast<UNiagaraScriptVariable*>(Found);
                    Variable->Metadata.EditorSortPriority = Priority;
                    if (!Category.IsEmpty()) Variable->Metadata.CategoryName = FText::FromString(Category);
                }
                ++Priority;
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if ((*Properties)->TryGetArrayField(TEXT("Children"), Children))
            for (const TSharedPtr<FJsonValue>& Child : *Children)
            {
                int32 ChildIndex = INDEX_NONE;
                if (Child->Type == EJson::Object && ExportIndex(Child->AsObject(), ChildIndex)) ApplyHierarchy(ChildIndex, Category, Priority);
            }
    };
    for (int32 Index = 0; Index < Exports.Num(); ++Index)
        if (Exports[Index]->AsObject()->GetStringField(TEXT("Type")) == TEXT("HierarchyRoot")) { int32 Priority = 0; ApplyHierarchy(Index, FString(), Priority); }

    // FJsonObjectConverter cannot resolve FModel export references embedded in
    // TMap values. Recreate each graph's canonical variable map explicitly.
    for (const auto& Pair : Objects)
    {
        UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Pair.Value);
        if (!Graph) continue;
        const TSharedPtr<FJsonObject>* Properties = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Exports[Pair.Key]->AsObject()->TryGetObjectField(TEXT("Properties"), Properties) ||
            !(*Properties)->TryGetArrayField(TEXT("VariableToScriptVariable"), Entries)) continue;
        FMapProperty* MapProperty = CastField<FMapProperty>(Graph->GetClass()->FindPropertyByName(TEXT("VariableToScriptVariable")));
        if (!MapProperty) continue;
        FScriptMapHelper Map(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(Graph));
        Map.EmptyValues();
        for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
        {
            const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
            const TSharedPtr<FJsonObject>* Key = nullptr;
            const TSharedPtr<FJsonObject>* Value = nullptr;
            int32 ValueIndex = INDEX_NONE;
            FNiagaraVariable Variable;
            if (!Entry.IsValid() || !Entry->TryGetObjectField(TEXT("Key"), Key) || !Entry->TryGetObjectField(TEXT("Value"), Value) ||
                !JsonVariable(*Key, Variable, Report) || !ExportIndex(*Value, ValueIndex)) continue;
            UObject* const* Found = Objects.Find(ValueIndex);
            if (Found && (*Found)->GetClass()->GetName() == TEXT("NiagaraScriptVariable"))
            {
                const int32 NewIndex = Map.AddDefaultValue_Invalid_NeedsRehash();
                MapProperty->KeyProp->CopyCompleteValue(Map.GetKeyPtr(NewIndex), &Variable);
                CastFieldChecked<FObjectPropertyBase>(MapProperty->ValueProp)->SetObjectPropertyValue(Map.GetValuePtr(NewIndex), *Found);
            }
        }
        Map.Rehash();
    }

    // Current Niagara stores the editable default and the static-switch flag
    // on the graph's script variable, not solely on the node. Older exports
    // may omit that flag even though a native static-switch node references
    // the variable, so normalize it before pin allocation/PostLoad.
    for (const auto& Pair : Objects)
    {
        UNiagaraNodeStaticSwitch* StaticSwitch = Cast<UNiagaraNodeStaticSwitch>(Pair.Value);
        if (!StaticSwitch || !StaticSwitch->SwitchTypeData.SwitchConstant.IsNone() || StaticSwitch->InputParameterName.IsNone()) continue;
        for (const auto& VariablePair : Objects)
            if (UNiagaraScriptVariable* Variable = VariablePair.Value && VariablePair.Value->GetClass()->GetName() == TEXT("NiagaraScriptVariable")
                ? reinterpret_cast<UNiagaraScriptVariable*>(VariablePair.Value) : nullptr)
                if (Variable->Variable.GetName() == StaticSwitch->InputParameterName)
                    Variable->Metadata.SetIsStaticSwitch(true);
    }

    TMap<FString, UEdGraphPin*> Pins;
    for (const auto& Pair : Objects)
    {
        UEdGraphNode* Node = Cast<UEdGraphNode>(Pair.Value);
        if (!Node) continue;
        if (MissingNodePlaceholders.Contains(Pair.Key)) continue;
        const TArray<TSharedPtr<FJsonValue>>* JsonPins = nullptr;
        if (!Exports[Pair.Key]->AsObject()->TryGetArrayField(TEXT("Pins"), JsonPins)) continue;
        UNiagaraNodeStaticSwitch* StaticSwitch = Cast<UNiagaraNodeStaticSwitch>(Node);
        UNiagaraNodeIf* CompatibilityIf = Cast<UNiagaraNodeIf>(Node);
        if (!StaticSwitch && !CompatibilityIf)
            Node->Pins.Reset();
        if (StaticSwitch)
        {
            // A raw JSON pin array from another engine version is not a valid
            // static-switch layout. Let this engine build pins from
            // SwitchTypeData/OutputVars, then transfer serialized identity and
            // defaults so the original links can still be reconstructed.
            StaticSwitch->Pins.Reset();
            StaticSwitch->AllocateDefaultPins();
        }
        else if (CompatibilityIf)
        {
            // UNiagaraNodeIf's compiler and stack history code look pins up by
            // its native names and cached GUIDs.  Keep that native layout and
            // map the newer Select pin identities onto it below.
            CompatibilityIf->PathAssociatedPinGuids.SetNum(CompatibilityIf->OutputVars.Num());
            CompatibilityIf->Pins.Reset();
            CompatibilityIf->AllocateDefaultPins();
        }
        TSet<UEdGraphPin*> ClaimedNativePins;
        for (const TSharedPtr<FJsonValue>& JsonPin : *JsonPins)
        {
            UEdGraphPin* Pin = nullptr;
            if (StaticSwitch || CompatibilityIf)
            {
                FString JsonName;
                FString Direction;
                JsonPin->AsObject()->TryGetStringField(TEXT("PinName"), JsonName);
                JsonPin->AsObject()->TryGetStringField(TEXT("Direction"), Direction);
                const EEdGraphPinDirection JsonDirection = Direction.EndsWith(TEXT("Output")) ? EGPD_Output : EGPD_Input;
                FString NativeName = JsonName;
                if (CompatibilityIf)
                {
                    if (JsonName.Equals(TEXT("Selector"), ESearchCase::IgnoreCase)) NativeName = TEXT("Condition");
                    else if (JsonName.EndsWith(TEXT(" if True"))) NativeName = JsonName.LeftChop(8) + TEXT(" A");
                    else if (JsonName.EndsWith(TEXT(" if False"))) NativeName = JsonName.LeftChop(9) + TEXT(" B");
                }
                const TArray<UEdGraphPin*>& NativePins = StaticSwitch ? StaticSwitch->Pins : CompatibilityIf->Pins;
                for (UEdGraphPin* Candidate : NativePins)
                    if (Candidate && !ClaimedNativePins.Contains(Candidate) && Candidate->Direction == JsonDirection &&
                        Candidate->PinName.ToString().Equals(NativeName, ESearchCase::IgnoreCase))
                    { Pin = Candidate; break; }
                if (!Pin)
                    for (UEdGraphPin* Candidate : NativePins)
                        if (Candidate && !ClaimedNativePins.Contains(Candidate) && Candidate->Direction == JsonDirection)
                        { Pin = Candidate; break; }
                if (Pin) ClaimedNativePins.Add(Pin);
            }
            if (!Pin)
            {
                Pin = UEdGraphPin::CreatePin(Node);
                Node->Pins.Add(Pin);
                if (StaticSwitch || CompatibilityIf)
                    Report.Warnings.AddUnique(FString::Printf(TEXT("%s needed a compatibility pin not generated by 17.XX"), *Node->GetName()));
            }
            const FName NativePinName = Pin->PinName;
            const EEdGraphPinDirection NativeDirection = Pin->Direction;
            FillPin(Pin, JsonPin->AsObject(), Report);
            if (CompatibilityIf)
            {
                Pin->PinName = NativePinName;
                Pin->Direction = NativeDirection;
            }
            FString OriginalPinId;
            JsonPin->AsObject()->TryGetStringField(TEXT("PinId"), OriginalPinId);
            Pins.Add(FString::Printf(TEXT("%d|%s"), Pair.Key, *OriginalPinId), Pin);
            ++Report.Pins;
        }
        if (UNiagaraNodeIf* IfNode = Cast<UNiagaraNodeIf>(Node))
        {
            IfNode->PathAssociatedPinGuids.SetNum(IfNode->OutputVars.Num());
            int32 OutputIndex = 0;
            for (UEdGraphPin* Pin : IfNode->Pins)
            {
                if (!Pin) continue;
                const FString PinName = Pin->PinName.ToString();
                if (Pin->Direction == EGPD_Input && PinName.Equals(TEXT("Condition"), ESearchCase::IgnoreCase)) IfNode->ConditionPinGuid = Pin->PersistentGuid;
                else if (Pin->Direction == EGPD_Input && PinName.EndsWith(TEXT(" A")))
                {
                    const FString BaseName = PinName.LeftChop(2);
                    for (int32 Index = 0; Index < IfNode->OutputVars.Num(); ++Index)
                        if (IfNode->OutputVars[Index].GetName().ToString() == BaseName) IfNode->PathAssociatedPinGuids[Index].InputAPinGuid = Pin->PersistentGuid;
                }
                else if (Pin->Direction == EGPD_Input && PinName.EndsWith(TEXT(" B")))
                {
                    const FString BaseName = PinName.LeftChop(2);
                    for (int32 Index = 0; Index < IfNode->OutputVars.Num(); ++Index)
                        if (IfNode->OutputVars[Index].GetName().ToString() == BaseName) IfNode->PathAssociatedPinGuids[Index].InputBPinGuid = Pin->PersistentGuid;
                }
                else if (Pin->Direction == EGPD_Output && Pin->PinType.PinSubCategory != TEXT("DynamicAddPin"))
                {
                    for (int32 Index = 0; Index < IfNode->OutputVars.Num(); ++Index)
                        if (IfNode->OutputVars[Index].GetName() == Pin->PinName) IfNode->PathAssociatedPinGuids[Index].OutputPinGuid = Pin->PersistentGuid;
                    ++OutputIndex;
                }
            }
        }
    }
    auto WouldCreateGraphCycle = [](UEdGraphPin* First, UEdGraphPin* Second)
    {
        UEdGraphPin* OutputPin = First && First->Direction == EGPD_Output ? First : Second;
        UEdGraphPin* InputPin = First && First->Direction == EGPD_Input ? First : Second;
        if (!OutputPin || !InputPin || OutputPin->Direction != EGPD_Output || InputPin->Direction != EGPD_Input) return true;
        UEdGraphNode* Upstream = OutputPin->GetOwningNode();
        UEdGraphNode* Downstream = InputPin->GetOwningNode();
        if (!Upstream || !Downstream || Upstream == Downstream) return true;
        TArray<UEdGraphNode*> Pending;
        TSet<UEdGraphNode*> Visited;
        Pending.Add(Downstream);
        while (Pending.Num())
        {
            UEdGraphNode* Current = Pending.Pop(false);
            if (!Current || Visited.Contains(Current)) continue;
            if (Current == Upstream) return true;
            Visited.Add(Current);
            for (UEdGraphPin* CurrentPin : Current->Pins)
                if (CurrentPin && CurrentPin->Direction == EGPD_Output)
                    for (UEdGraphPin* Linked : CurrentPin->LinkedTo)
                        if (Linked && Linked->Direction == EGPD_Input) Pending.Add(Linked->GetOwningNode());
        }
        return false;
    };
    for (const auto& Pair : Objects)
    {
        UEdGraphNode* Node = Cast<UEdGraphNode>(Pair.Value);
        if (!Node) continue;
        if (MissingNodePlaceholders.Contains(Pair.Key)) continue;
        const TArray<TSharedPtr<FJsonValue>>* JsonPins = nullptr;
        if (!Exports[Pair.Key]->AsObject()->TryGetArrayField(TEXT("Pins"), JsonPins)) continue;
        for (int32 PinIndex = 0; PinIndex < JsonPins->Num(); ++PinIndex)
        {
            FString ThisPinId;
            (*JsonPins)[PinIndex]->AsObject()->TryGetStringField(TEXT("PinId"), ThisPinId);
            UEdGraphPin** ThisPin = Pins.Find(FString::Printf(TEXT("%d|%s"), Pair.Key, *ThisPinId));
            if (!ThisPin || !*ThisPin) continue;
            const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
            if (!(*JsonPins)[PinIndex]->AsObject()->TryGetArrayField(TEXT("LinkedTo"), Links)) continue;
            for (const TSharedPtr<FJsonValue>& LinkValue : *Links)
            {
                const TSharedPtr<FJsonObject> Link = LinkValue->AsObject();
                const TSharedPtr<FJsonObject>* Owner = nullptr;
                FString PinId;
                int32 OwnerIndex = INDEX_NONE;
                if (!Link.IsValid() || !Link->TryGetObjectField(TEXT("OwningNode"), Owner) || !ExportIndex(*Owner, OwnerIndex) || !Link->TryGetStringField(TEXT("PinId"), PinId)) continue;
                if (UEdGraphPin** Other = Pins.Find(FString::Printf(TEXT("%d|%s"), OwnerIndex, *PinId)))
                    if (!(*ThisPin)->LinkedTo.Contains(*Other))
                    {
                        if (WouldCreateGraphCycle(*ThisPin, *Other))
                        {
                            Report.Warnings.AddUnique(FString::Printf(TEXT("Skipped cyclic or direction-invalid link: %s.%s <-> %s.%s"),
                                *Node->GetName(), *(*ThisPin)->PinName.ToString(), *(*Other)->GetOwningNode()->GetName(), *(*Other)->PinName.ToString()));
                            continue;
                        }
                        (*ThisPin)->MakeLinkTo(*Other);
                        ++Report.Links;
                    }
            }
        }
    }

    TArray<UNiagaraScriptSource*> Sources;
    for (const auto& Pair : Objects) if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Pair.Value)) Sources.Add(Source);
    Sources.Sort([](const UNiagaraScriptSource& A, const UNiagaraScriptSource& B) { return A.GetName() < B.GetName(); });
    UNiagaraScriptSource* ActiveSource = nullptr;
    int32 ActiveSourceScore = MIN_int32;
    for (UNiagaraScriptSource* Candidate : Sources)
    {
        if (!Candidate || !Candidate->NodeGraph) continue;
        int32 Score = Candidate->NodeGraph->Nodes.Num() * 10;
        for (UEdGraphNode* GraphNode : Candidate->NodeGraph->Nodes)
            if (UNiagaraNodeStaticSwitch* StaticSwitch = Cast<UNiagaraNodeStaticSwitch>(GraphNode))
                if (StaticSwitch->InputParameterName.IsNone() && StaticSwitch->SwitchTypeData.SwitchConstant.IsNone())
                    Score -= 1000;
        if (!ActiveSource || Score > ActiveSourceScore)
        {
            ActiveSource = Candidate;
            ActiveSourceScore = Score;
        }
    }
    if (ActiveSource) Script->SetSource(ActiveSource);
    if (ActiveSource)
    {
        for (const auto& Pair : Objects)
        {
            UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Pair.Value);
            if (Output && Output->GetTypedOuter<UNiagaraScriptSource>() == ActiveSource)
            {
                Script->SetUsage(Output->GetUsage());
                if (Output->GetUsageId().IsValid()) Script->SetUsageId(Output->GetUsageId());
                break;
            }
        }
    }
    if (Script->GetUsage() == ENiagaraScriptUsage::Module || Script->GetUsage() == ENiagaraScriptUsage::Function ||
        Script->GetUsage() == ENiagaraScriptUsage::DynamicInput)
        Script->SetUsageId(FGuid());
    // Usage is editor-only/versioned in newer assets and absent from the
    // top-level export. The active graph output is the authoritative fallback.
    if (ScriptIndex != INDEX_NONE)
    {
        const TSharedPtr<FJsonObject>* ScriptProperties = nullptr;
        if (Exports[ScriptIndex]->AsObject()->TryGetObjectField(TEXT("Properties"), ScriptProperties))
            for (const auto& Field : (*ScriptProperties)->Values)
                if (CleanName(Field.Key) == TEXT("UsageId") && Field.Value->Type == EJson::String)
                {
                    FGuid UsageId;
                    if (FGuid::Parse(Field.Value->AsString(), UsageId)) Script->SetUsageId(UsageId);
                }
    }
    if (ActiveSource && ActiveSource->NodeGraph)
    {
        // Callable graphs in 17.XX are looked up with the default (zero)
        // usage id.  Do this after the top-level script property pass, which
        // may contain a newer per-version UsageId.
        if (Script->GetUsage() == ENiagaraScriptUsage::Module || Script->GetUsage() == ENiagaraScriptUsage::Function ||
            Script->GetUsage() == ENiagaraScriptUsage::DynamicInput)
            Script->SetUsageId(FGuid());
        UNiagaraNodeOutput* ActiveOutput = FindMatchingOutput(ActiveSource->NodeGraph, Script->GetUsage());
        if (ActiveOutput)
        {
            ActiveOutput->SetUsageId(Script->GetUsageId());
        }
        else
        {
            Report.Errors.Add(FString::Printf(TEXT("Active graph has no output node for script usage %d"), (int32)Script->GetUsage()));
        }
    }
    if (Sources.Num() > 1) Report.Warnings.Add(FString::Printf(TEXT("Imported all %d versioned sources as subobjects; selected %s as the most complete 17.XX-compatible source."), Sources.Num(), ActiveSource ? *ActiveSource->GetName() : TEXT("none")));

    // Pin allocation can make Niagara register canonical script-variable
    // objects which are not the same exported UObject instances. Normalize
    // every package-owned canonical variable after graph construction so the
    // static-switch flag survives save/load without PostLoad repairing it.
    TSet<FName> StaticSwitchParameterNames;
    for (const auto& Pair : Objects)
        if (UNiagaraNodeStaticSwitch* StaticSwitch = Cast<UNiagaraNodeStaticSwitch>(Pair.Value))
            if (!StaticSwitch->InputParameterName.IsNone())
                StaticSwitchParameterNames.Add(StaticSwitch->InputParameterName);
    TArray<UObject*> PackageObjects;
    GetObjectsWithOuter(Package, PackageObjects, true);
    for (UObject* PackageObject : PackageObjects)
        if (PackageObject && PackageObject->GetClass()->GetName() == TEXT("NiagaraScriptVariable"))
        {
            UNiagaraScriptVariable* Variable = reinterpret_cast<UNiagaraScriptVariable*>(PackageObject);
            if (StaticSwitchParameterNames.Contains(Variable->Variable.GetName()))
                Variable->Metadata.SetIsStaticSwitch(true);
        }

    Script->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Script);
    Package->SetDirtyFlag(true);
    const FString AssetFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package, Script, RF_Public | RF_Standalone, *AssetFilename, GError, nullptr, false, true, SAVE_NoError))
        Report.Errors.Add(FString::Printf(TEXT("Could not save %s"), *AssetFilename));
    return Script;
}
}

void FScriptImporterModule::StartupModule()
{
    FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    TSharedRef<FExtender> Extender = MakeShared<FExtender>();
    Extender->AddMenuExtension(TEXT("FileProject"), EExtensionHook::After, nullptr, FMenuExtensionDelegate::CreateRaw(this, &FScriptImporterModule::AddMenuEntry));
    LevelEditor.GetMenuExtensibilityManager()->AddExtender(Extender);

    FString AutomatedImport;
    if (FParse::Value(FCommandLine::Get(), TEXT("NiagaraJsonImport="), AutomatedImport))
    {
        FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([AutomatedImport]()
        {
            ScriptImporter::FImportReport Report;
            const FString AssetName = FPaths::GetBaseFilename(AutomatedImport).Replace(TEXT(".o"), TEXT(""));
            ScriptImporter::Import(AutomatedImport, TEXT("/Game/ImportedNiagara/") + AssetName, Report);
            ScriptImporter::WriteReportToOutputLog(AutomatedImport, Report);
            UE_LOG(LogTemp, Display, TEXT("NIAGARA_JSON_AUTOMATION_IMPORT: %s"), *Report.Format());
            FPlatformMisc::RequestExit(false);
        });
    }

    FString AutomatedValidate;
    if (FParse::Value(FCommandLine::Get(), TEXT("NiagaraJsonValidate="), AutomatedValidate))
    {
        FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([AutomatedValidate]()
        {
            UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *AutomatedValidate);
            if (!Script)
            {
                UE_LOG(LogTemp, Error, TEXT("NIAGARA_JSON_AUTOMATION_VALIDATE: load failed"));
                FPlatformMisc::RequestExit(true);
                return;
            }
            Script->ConditionalPostLoad();
            UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetSource());
            if (!Source || !Source->NodeGraph || !ScriptImporter::FindMatchingOutput(Source->NodeGraph, Script->GetUsage(), Script->GetUsageId()))
            {
                UE_LOG(LogTemp, Error, TEXT("NIAGARA_JSON_AUTOMATION_VALIDATE: matching output lookup failed for usage %d"), (int32)Script->GetUsage());
                FPlatformMisc::RequestExit(true);
                return;
            }
            UObject* Duplicate = StaticDuplicateObject(Script, GetTransientPackage());
            Duplicate->ConditionalPostLoad();
            UE_LOG(LogTemp, Display, TEXT("NIAGARA_JSON_AUTOMATION_VALIDATE: success"));
            FPlatformMisc::RequestExit(false);
        });
    }
}

void FScriptImporterModule::ShutdownModule() {}

void FScriptImporterModule::AddMenuEntry(FMenuBuilder& MenuBuilder)
{
    MenuBuilder.AddMenuEntry(LOCTEXT("Import", "Import FModel Niagara JSON..."), LOCTEXT("ImportTip", "Recreate an editable Niagara script from an enhanced FModel JSON export."), FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FScriptImporterModule::OpenImportDialog)));
}

void FScriptImporterModule::OpenImportDialog()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop) return;
    TArray<FString> Files;
    const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    if (!Desktop->OpenFileDialog(Parent, TEXT("Import FModel Niagara JSON"), TEXT(""), TEXT(""), TEXT("JSON (*.json)|*.json"), EFileDialogFlags::None, Files) || !Files.Num()) return;
    const FString AssetName = FPaths::GetBaseFilename(Files[0]).Replace(TEXT(".o"), TEXT(""));
    ScriptImporter::FImportReport Report;
    UNiagaraScript* Script = ScriptImporter::Import(Files[0], TEXT("/Game/ImportedNiagara/") + AssetName, Report);
    ScriptImporter::WriteReportToOutputLog(Files[0], Report);
    if (Script)
    {
        TArray<UObject*> AssetsToSync;
        AssetsToSync.Add(Script);
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(AssetsToSync, false, true);
    }
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report.Format()));
}

IMPLEMENT_MODULE(FScriptImporterModule, ScriptImporter)

#undef LOCTEXT_NAMESPACE
