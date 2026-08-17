using System.Collections;
using System.Reflection;
using UAssetAPI;
using UAssetAPI.ExportTypes;
using UAssetAPI.PropertyTypes.Objects;
using UAssetAPI.PropertyTypes.Structs;
using UAssetAPI.UnrealTypes;
using UAssetSnippet;

if (args.Length != 1)
    throw new ArgumentException("Usage: StaticPakPatcher <unpacked-pak-root>");

var root = Path.GetFullPath(args[0]);
var content = Path.Combine(root, "BloodstainedRotN", "Content");
if (!Directory.Exists(content))
    throw new DirectoryNotFoundException($"Missing unpacked content root: {content}");

static NormalExport Normal(UAsset asset, int exportNumber) =>
    (NormalExport)asset.Exports[exportNumber - 1];

static PropertyData Property(NormalExport export, string name) =>
    export.Data.Single(property => property.Name.ToString() == name);

static int ObjectExportNumber(NormalExport export, string name) =>
    ((ObjectPropertyData)Property(export, name)).Value.Index;

static void SetObject(NormalExport export, string name, int exportNumber) =>
    ((ObjectPropertyData)Property(export, name)).Value = FPackageIndex.FromRawIndex(exportNumber);

static void SetName(UAsset asset, NormalExport export, string propertyName, string value) =>
    ((NamePropertyData)Property(export, propertyName)).Value = FName.FromString(asset, value);

static void SetEnum(UAsset asset, NormalExport export, string propertyName, string enumType, string value)
{
    var property = (BytePropertyData)Property(export, propertyName);
    property.EnumType = FName.FromString(asset, enumType);
    property.EnumValue = FName.FromString(asset, value);
}

static void SetRootTransform(UAsset asset, NormalExport actor, FVector location, FVector scale)
{
    var rootExport = Normal(asset, ObjectExportNumber(actor, "RootComponent"));
    rootExport.Data.Clear();

    var locationStruct = new StructPropertyData(FName.FromString(asset, "RelativeLocation"),
                                                 FName.FromString(asset, "Vector"));
    locationStruct.Value.Add(new VectorPropertyData(FName.FromString(asset, "RelativeLocation"))
    {
        Value = location,
    });
    rootExport.Data.Add(locationStruct);

    var scaleStruct = new StructPropertyData(FName.FromString(asset, "RelativeScale3D"),
                                              FName.FromString(asset, "Vector"));
    scaleStruct.Value.Add(new VectorPropertyData(FName.FromString(asset, "RelativeScale3D"))
    {
        Value = scale,
    });
    rootExport.Data.Add(scaleStruct);
}

static NormalExport AddTreasureProxy(
    UAsset donorAsset, UAsset targetAsset, string objectName, string itemId,
    string treasureFlag, FVector location)
{
    if (targetAsset.Exports.OfType<NormalExport>()
        .Any(export => export.ObjectName.ToString() == objectName))
        throw new InvalidOperationException($"{objectName} already exists in a clean build input");

    var firstAddedExport = targetAsset.Exports.Count;
    var snippet = new UAssetSnippet.UAssetSnippet(donorAsset, 63);
    snippet.AddToUAsset(targetAsset, objectName);
    var proxy = (NormalExport)targetAsset.Exports[firstAddedExport];
    SetName(targetAsset, proxy, "DropItemID", itemId);
    SetName(targetAsset, proxy, "ItemID", itemId);
    SetEnum(targetAsset, proxy, "TreasureFlag", "EGameTreasureFlag", treasureFlag);
    SetRootTransform(targetAsset, proxy, location, new FVector(0.0f, 0.0f, 0.0f));
    return proxy;
}

static void PatchShipTreasureMarkers(string content)
{
    var levels = Path.Combine(content, "Core", "Environment", "ACT01_SIP", "Level");
    var donor = new UAsset(Path.Combine(levels, "m01SIP_002_Gimmick.umap"), EngineVersion.VER_UE4_22);

    var wallPath = Path.Combine(levels, "m01SIP_004_Gimmick.umap");
    var wall = new UAsset(wallPath, EngineVersion.VER_UE4_22);
    AddTreasureProxy(donor, wall, "AP_WallMarker_BP", "Wall_SIP004_1",
        "EGameTreasureFlag::Dummy1", new FVector(120.0f, -120.0f, 240.0f));
    wall.Write(wallPath);

    var hiddenPath = Path.Combine(levels, "m01SIP_025_Gimmick.umap");
    var hidden = new UAsset(hiddenPath, EngineVersion.VER_UE4_22);
    var randomizedChest = hidden.Exports.OfType<NormalExport>().Single(export =>
        export.ObjectName.ToString() == "TR_PBEasyTreasureBox_BP");
    SetEnum(hidden, randomizedChest, "TreasureFlag", "EGameTreasureFlag",
        "EGameTreasureFlag::Treasure_StatusUp_Test1");
    AddTreasureProxy(donor, hidden, "AP_HiddenChestMarker_BP", "Treasurebox_SIP025_2",
        "EGameTreasureFlag::Treasure_StatusUp_Test1", new FVector(1003.0f, -120.0f, 60.0f));
    hidden.Write(hiddenPath);
    Console.WriteLine("Patched ship treasure markers and hidden-chest proxy");
}

static void AddPanelSlot(UAsset asset, NormalExport panel, int slotExportNumber)
{
    var slots = (ArrayPropertyData)Property(panel, "Slots");
    var example = (ObjectPropertyData)slots.Value[0];
    slots.Value = [.. slots.Value, new ObjectPropertyData(FName.FromString(asset, example.Name.ToString()))
    {
        Value = FPackageIndex.FromRawIndex(slotExportNumber),
    }];
}

static void RemovePanelSlot(NormalExport panel, int slotExportNumber)
{
    var slots = (ArrayPropertyData)Property(panel, "Slots");
    slots.Value = slots.Value
        .Where(value => ((ObjectPropertyData)value).Value.Index != slotExportNumber)
        .ToArray();
}

static bool SetNamedFloat(object? value, string name, float replacement, HashSet<object>? visited = null)
{
    if (value is null || value is string) return false;
    visited ??= new HashSet<object>(ReferenceEqualityComparer.Instance);
    if (!value.GetType().IsValueType && !visited.Add(value)) return false;

    var type = value.GetType();
    var nameProperty = type.GetProperty("Name", BindingFlags.Public | BindingFlags.Instance);
    var nameField = type.GetField("Name", BindingFlags.Public | BindingFlags.Instance);
    var valueProperty = type.GetProperty("Value", BindingFlags.Public | BindingFlags.Instance);
    var valueField = type.GetField("Value", BindingFlags.Public | BindingFlags.Instance);
    var currentName = (nameProperty?.GetValue(value) ?? nameField?.GetValue(value))?.ToString();
    var valueType = valueProperty?.PropertyType ?? valueField?.FieldType;
    if (currentName == name && valueType == typeof(float))
    {
        if (valueProperty?.CanWrite == true) valueProperty.SetValue(value, replacement);
        else if (valueField is not null) valueField.SetValue(value, replacement);
        else return false;
        return true;
    }

    if (value is IEnumerable enumerable)
        foreach (var entry in enumerable)
            if (SetNamedFloat(entry, name, replacement, visited)) return true;
    if (valueProperty is not null && valueProperty.GetIndexParameters().Length == 0)
        return SetNamedFloat(valueProperty.GetValue(value), name, replacement, visited);
    if (valueField is not null)
        return SetNamedFloat(valueField.GetValue(value), name, replacement, visited);
    return false;
}

static void RepurposeRegistryWidget(
    UAsset asset, int rootNumber, int oldPanelNumber, int newPanelNumber,
    string treasureId, float left, float top)
{
    var root = Normal(asset, rootNumber);
    var slotNumber = ObjectExportNumber(root, "Slot");
    var slot = Normal(asset, slotNumber);
    RemovePanelSlot(Normal(asset, oldPanelNumber), slotNumber);
    AddPanelSlot(asset, Normal(asset, newPanelNumber), slotNumber);
    slot.OuterIndex = FPackageIndex.FromRawIndex(newPanelNumber);
    SetObject(slot, "Parent", newPanelNumber);
    foreach (var property in root.Data.Where(property => property.Name.ToString() == "TreasureID"))
        ((NamePropertyData)property).Value = FName.FromString(asset, treasureId);
    if (!SetNamedFloat(Property(slot, "LayoutData"), "Left", left) ||
        !SetNamedFloat(Property(slot, "LayoutData"), "Top", top))
        throw new InvalidOperationException($"Unable to position registry widget {root.ObjectName}");
}

static void PatchMapRegistry(string content)
{
    var path = Path.Combine(content, "Core", "UI", "Map", "MapManageBlueprint.uasset");
    var asset = new UAsset(path, EngineVersion.VER_UE4_22);
    var additions = new[]
    {
        (Id: "Treasurebox_SIP025_2", TemplateRoot: 4339, TemplateOldPanel: 347,
         CookedRoot: 4577, CookedOldPanel: 394, Left: -932.254f, Top: 208f),
        (Id: "Wall_SIP004_1", TemplateRoot: 4373, TemplateOldPanel: 344,
         CookedRoot: 4611, CookedOldPanel: 391, Left: -1116f, Top: 134f),
    };
    foreach (var addition in additions)
    {
        RepurposeRegistryWidget(asset, addition.TemplateRoot, addition.TemplateOldPanel, 342,
            addition.Id, addition.Left, addition.Top);
        RepurposeRegistryWidget(asset, addition.CookedRoot, addition.CookedOldPanel, 389,
            addition.Id, addition.Left, addition.Top);
    }
    asset.Write(path);
    Console.WriteLine("Patched native map treasure registry");
}

static void PatchDenChestGate(string content)
{
    var path = Path.Combine(content, "Core", "Environment", "ACT02_VIL", "Level",
        "m02VIL_005_Gimmick.umap");
    var asset = new UAsset(path, EngineVersion.VER_UE4_22);
    var chest = asset.Exports.OfType<NormalExport>().Single(export =>
        export.Data.OfType<NamePropertyData>().Any(property =>
            property.Name.ToString() == "ItemID" &&
            property.Value.ToString() == "Treasurebox_VIL005_1"));
    var unlock = chest.Data.OfType<NamePropertyData>().Single(property =>
        property.Name.ToString() == "OptionalGimmickID");
    if (unlock.Value.ToString() is not ("GND001_IsFakeMoonBreak" or "GotAllShard"))
        throw new InvalidOperationException(
            $"Unexpected Den chest gate {unlock.Value}; expected GND001_IsFakeMoonBreak or GotAllShard");
    unlock.Value = FName.FromString(asset, "GotAllShard");
    asset.Write(path);
    Console.WriteLine("Patched post-Den chest gate");
}

static object Member(object value, string name)
{
    var type = value.GetType();
    return type.GetProperty(name, BindingFlags.Public | BindingFlags.Instance)?.GetValue(value)
        ?? type.GetField(name, BindingFlags.Public | BindingFlags.Instance)?.GetValue(value)
        ?? throw new InvalidOperationException($"{type.Name} has no public {name} member");
}

static void SetIntMember(object value, string name, int replacement)
{
    var type = value.GetType();
    var property = type.GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
    if (property?.CanWrite == true)
    {
        property.SetValue(value, replacement);
        return;
    }
    var field = type.GetField(name, BindingFlags.Public | BindingFlags.Instance)
        ?? throw new InvalidOperationException($"{type.Name} has no writable public {name} member");
    field.SetValue(value, replacement);
}

static int PatchLendingReturn(FunctionExport function, int expressionIndex, int expected)
{
    var let = function.ScriptBytecode[expressionIndex];
    if (let.GetType().Name != "EX_Let")
        throw new InvalidOperationException(
            $"GetLendableBooksNum[{expressionIndex}] is {let.GetType().Name}, expected EX_Let");
    var constant = Member(let, "Expression");
    if (constant.GetType().Name != "EX_IntConst")
        throw new InvalidOperationException(
            $"GetLendableBooksNum[{expressionIndex}].Expression is {constant.GetType().Name}, expected EX_IntConst");
    var oldValue = Convert.ToInt32(Member(constant, "Value"));
    if (oldValue != expected)
        throw new InvalidOperationException(
            $"GetLendableBooksNum[{expressionIndex}] is {oldValue}, expected {expected}");
    SetIntMember(constant, "Value", 3);
    return oldValue;
}

static void PatchLendingLimit(string content)
{
    var path = Path.Combine(content, "Core", "System", "PBBookManager_BP.uasset");
    var asset = new UAsset(path, EngineVersion.VER_UE4_22);
    var function = asset.Exports.OfType<FunctionExport>().Single(export =>
        export.ObjectName.ToString() == "GetLendableBooksNum");
    PatchLendingReturn(function, 19, 1);
    PatchLendingReturn(function, 25, 2);
    asset.Write(path);

    var verify = new UAsset(path, EngineVersion.VER_UE4_22);
    var verifyFunction = verify.Exports.OfType<FunctionExport>().Single(export =>
        export.ObjectName.ToString() == "GetLendableBooksNum");
    foreach (var index in new[] { 19, 25, 27 })
    {
        var constant = Member(verifyFunction.ScriptBytecode[index], "Expression");
        if (Convert.ToInt32(Member(constant, "Value")) < 3)
            throw new InvalidOperationException($"Lending return at expression {index} is below 3");
    }
    Console.WriteLine("Patched O.D. lending returns 1/2 to minimum 3; unlimited branch preserved");
}

PatchShipTreasureMarkers(content);
PatchMapRegistry(content);
PatchDenChestGate(content);
PatchLendingLimit(content);
Console.WriteLine("Static pak asset patching complete");
