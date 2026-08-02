#pragma once
#include "HookManager.h"

#include <Chr_P0000_classes.hpp>
#include <Engine_classes.hpp>
#include <PB_Chr_PlayerRoot_classes.hpp>
#include <ProjectBlood_parameters.hpp>
#include <ProjectBlood_structs.hpp>
#include <Step_P0000_classes.hpp>
#include <UMG_classes.hpp>
#include <UnrealContainers.hpp>
#include <optional>

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "GameManager.h"
#include "Gui.h"
#include "InGameTracker.h"
#include "ItemGetPopup_classes.hpp"
#include "Logger.h"
#include "MainMenuStatus.h"
#include "Mod/Archipelago.h"
#include "ProjectBlood_classes.hpp"
#include "QualityOfLife.h"
#include "SDK.hpp"
#include "Utils.h"

std::set<std::string> HookManager::pendingWidgets;
std::set<std::string> HookManager::processedWidgets;

void (*HookManager::originalProcessEvent)(SDK::UObject*, SDK::UFunction*, void*) = nullptr;
void (*HookManager::originalProcessLocalScriptFunction)(SDK::UObject*, SDK::UFunction*, void*) = nullptr;
bool HookManager::playerDetected = false;
bool HookManager::shuttingDown = false;

struct ShardAppearance {
    SDK::EShardType type;
    SDK::EShardColor color;
};

static std::unordered_map<std::string, ShardAppearance> originalRandomizedShardAppearances;
static std::unordered_map<std::string, SDK::EDropSpecialFlag> originalRandomizedDropFlags;

namespace {

const std::unordered_map<std::string, const char*> knownVanillaShardIds = {
    // Generated from the unmodified base-game PB_DT_DropRateMaster in pakchunk0.
    // Runtime table access is insufficient because Randomizer.pak has already replaced
    // these rows with AP_* placeholder shards by the time this plugin loads.
    {"AP_AAAA_Shard", "Dummy"},
    {"AP_N1001_Shard", "SwingTentacle"},
    {"AP_N1002_Shard", "Bloodsteel"},
    {"AP_N1003_Shard", "Demoniccapture"},
    {"AP_N1006_Shard", "Reflectionray"},
    {"AP_N1008_Shard", "Dimensionshift"},
    {"AP_N2001_Shard", "Hammerknuckle"},
    {"AP_N2004_Shard", "Doublejump"},
    {"AP_N2006_Shard", "Invert"},
    {"AP_N2007_Shard", "Shadowtracer"},
    {"AP_N2012_Shard", "AccelWorld"},
    {"AP_N2013_Shard", "NeverSatisfied"},
    {"AP_N1004_Shard", "GoldBarrett"},
    {"AP_N1005_Shard", "InfernoBrace"},
    {"AP_N3006_Shard", "Ceruleansplash"},
    {"AP_N3006_OpeningDemo_Shard", "Ceruleansplash"},
    {"AP_N3005_Shard", "FireCannon"},
    {"AP_N3005_FireCannon_Shard", "FireCannon"},
    {"AP_N3007_Shard", "Raginggirl"},
    {"AP_N3013_Shard", "SummonAme"},
    {"AP_N3015_Shard", "Headfail"},
    {"AP_N3051_Shard", "SummonRat"},
    {"AP_N3004_Shard", "BoonsLore"},
    {"AP_N3029_Shard", "SummonButt"},
    {"AP_N3073_Shard", "SummonGilemund"},
    {"AP_N3105_Shard", "HelleTicalGrinder"},
    {"AP_N3080_Shard", "SwordMastery"},
    {"AP_N3066_Shard", "EntangleBind"},
    {"AP_N3042_Shard", "Releasetoade"},
    {"AP_N3065_Shard", "SummonYorkton"},
    {"AP_N3011_Shard", "StraightArrow"},
    {"AP_N3091_Shard", "FamiliaCarabos"},
    {"AP_N3034_Shard", "SummonApe"},
    {"AP_N3075_Shard", "CraftMastery"},
    {"AP_N3100_Shard", "GratefulAssist"},
    {"AP_N3104_Shard", "Acidgouache"},
    {"AP_N3053_Shard", "Petrey"},
    {"AP_N3079_Shard", "KnifeMastery"},
    {"AP_N3055_Shard", "INTEnhance"},
    {"AP_N3001_Shard", "SummonBuell"},
    {"AP_N3116_Shard", "LigaStreyma"},
    {"AP_N3082_Shard", "SummonBugs"},
    {"AP_N3090_Shard", "SummonDurahanMaHead"},
    {"AP_N3084_Shard", "SummonBuell"},
    {"AP_N2002_Shard", "Beastguard"},
    {"AP_N3064_Shard", "CircleRipper"},
    {"AP_N3010_Shard", "Shovelshooter"},
    {"AP_N3033_Shard", "ResistThunder"},
    {"AP_N3017_Shard", "GunMastery"},
    {"AP_N3063_Shard", "FamiliaDantalion"},
    {"AP_N3103_Shard", "Sacredshade"},
    {"AP_N3099_Shard", "ResistPetri"},
    {"AP_N3008_Shard", "TepesOsius"},
    {"AP_N3111_Shard", "Aimingshield"},
    {"AP_N3058_Shard", "ChangeBunny"},
    {"AP_N3019_Shard", "ShootingDagger"},
    {"AP_N2009_Shard", "DragonicRage"},
    {"AP_N3025_Shard", "Resistcurse"},
    {"AP_N3043_Shard", "ResistPoison"},
    {"AP_N3092_Shard", "Healing"},
    {"AP_N3024_Shard", "BaudRideBlast"},
    {"AP_N3070_Shard", "Moneyispower"},
    {"AP_N3114_Shard", "ResistBrow"},
    {"AP_N3072_Shard", "SummonChair"},
    {"AP_N3012_Shard", "ChaseArrow"},
    {"AP_N3052_Shard", "FamiliaBradBringer"},
    {"AP_N3085_Shard", "SpearMastery"},
    {"AP_N3109_Shard", "Upbeatheat"},
    {"AP_N3037_Shard", "FamiliaBuell"},
    {"AP_N3077_Shard", "KickMastery"},
    {"AP_N3093_Shard", "MNDEnhance"},
    {"AP_N3110_Shard", "ThrowAxe"},
    {"AP_N3121_Shard", "Zombie8Bit"},
    {"AP_N3122_Shard", "Ghost8Bit"},
    {"AP_N2015_Shard", "Nightmare8Bit"},
    {"AP_N3124_Shard", "EightBitFire"},
    {"AP_N3102_Shard", "ResistHorley"},
    {"AP_N3009_Shard", "optimizer"},
    {"AP_N3087_Shard", "Deadhowling"},
    {"AP_N3098_Shard", "Resistedge"},
    {"AP_N3078_Shard", "VaIsha"},
    {"AP_N3022_Shard", "Aquastream"},
    {"AP_N3044_Shard", "SamonSamHiggin"},
    {"AP_N3076_Shard", "Submariner"},
    {"AP_N3089_Shard", "Detectiveeye"},
    {"AP_N3056_Shard", "Wisdomwords"},
    {"AP_N3014_Shard", "CurseDray"},
    {"AP_N3002_Shard", "CONEnhance"},
    {"AP_N3112_Shard", "TissLeif"},
    {"AP_N3115_Shard", "ResistBrow"},
    {"AP_N2010_Shard", "Resistthrust"},
    {"AP_N3101_Shard", "Resistdark"},
    {"AP_N3059_Shard", "WhipMastery"},
    {"AP_N3113_Shard", "Toxicstorm"},
    {"AP_N3083_Shard", "RedDowther"},
    {"AP_N3032_Shard", "ResistFire"},
    {"AP_N3119_Shard", "FireThrower"},
    {"AP_N3123_Shard", "HellHound"},
    {"AP_N3086_Shard", "RapidSpear"},
    {"AP_N3071_Shard", "LUKEnhance"},
    {"AP_N3035_Shard", "LuckyDrop"},
    {"AP_N3054_Shard", "STREnhance"},
    {"AP_N3074_Shard", "PetraBless"},
    {"AP_N3020_Shard", "HiddenDart"},
    {"AP_N3117_Shard", "Tornadoslicer"},
    {"AP_N3067_Shard", "EntangleBind"},
    {"AP_N3030_Shard", "SummonButt"},
    {"AP_N3045_Shard", "Releasetoade"},
    {"AP_N3016_Shard", "FireCannon"},
    {"AP_N3028_Shard", "FamiliaBuell"},
    {"AP_N2003_Shard", "WildScratch"},
    {"AP_N2008_Shard", "Voidlay"},
    {"AP_N3081_Shard", "drain"},
    {"AP_N3021_Shard", "Accelerator"},
    {"AP_N3038_Shard", "GunMastery"},
    {"AP_N3031_Shard", "Resistice"},
    {"AP_N3088_Shard", "SummonTracer"},
    {"AP_N3018_Shard", "GunMastery"},
    {"AP_N3120_Shard", "AxStrike"},
    {"AP_N3057_Shard", "FoldShiu"},
    {"AP_N3106_Shard", "Chiselbalage"},
    {"AP_N3107_Shard", "RuinBeak"},
    {"AP_N3108_Shard", "Jackpot"},
    {"AP_Shortcut_Shard", "Shortcut"},
    {"AP_Deepsinker_Shard", "Deepsinker"},
    {"AP_FamiliaSilverKnight_Shard", "FamiliaSilverKnight"},
    {"AP_Aquastream_Shard", "Aquastream"},
    {"AP_FamiliaIgniculus_Shard", "FamiliaIgniculus"},
    {"AP_FamiliaArcher_Shard", "FamiliaArcher"},
};

std::optional<SDK::FName> ResolveVanillaShardId(const SDK::FName& randomizedShardId) {
    auto knownShard = knownVanillaShardIds.find(randomizedShardId.ToString());
    if (knownShard == knownVanillaShardIds.end()) return std::nullopt;
    return FNameFromString(knownShard->second);
}

ItemLookupResult GetVanillaShardItemSupport(Archipelago& archipelago, const SDK::FName& vanillaShardId) {
    SDK::FPBItemCatalogData itemData{};
    GameManager::Instance().Player()->CharacterInventory->GetItemDataById(vanillaShardId, &itemData);
    std::string displayName = itemData.Name.ToString();

    if (!displayName.empty()) {
        ItemLookupResult result = archipelago.GetItemLookupResult(displayName);
        if (result != ItemLookupResult::UnknownItem) return result;
    }
    return archipelago.GetItemLookupResult(vanillaShardId.ToString());
}

SDK::EShardColor DefaultShardColor(SDK::EShardType shardType) {
    switch (shardType) {
        case SDK::EShardType::Trigger:
            return SDK::EShardColor::Red;
        case SDK::EShardType::Effective:
            return SDK::EShardColor::Blue;
        case SDK::EShardType::Directional:
            return SDK::EShardColor::Purple;
        case SDK::EShardType::Enchant:
            return SDK::EShardColor::Yellow;
        case SDK::EShardType::Familia:
            return SDK::EShardColor::Green;
        case SDK::EShardType::Skill:
            return SDK::EShardColor::White;
        default:
            return SDK::EShardColor::None;
    }
}

SDK::FPBShardMasterData* FindShardMasterRow(SDK::UDataTable* shardMasterTable, const std::string& rowName) {
    if (!shardMasterTable) return nullptr;
    for (const auto& pair : shardMasterTable->RowMap) {
        if (pair.Key().ToString() == rowName) {
            return reinterpret_cast<SDK::FPBShardMasterData*>(pair.Value());
        }
    }
    return nullptr;
}

SDK::FPBDropRateMasterData* FindDropMasterRow(SDK::UDataTable* dropTable, const std::string& rowName) {
    if (!dropTable) return nullptr;
    for (const auto& pair : dropTable->RowMap) {
        if (pair.Key().ToString() == rowName) {
            return reinterpret_cast<SDK::FPBDropRateMasterData*>(pair.Value());
        }
    }
    return nullptr;
}

bool RestoreVanillaShardAppearance(SDK::AShardBase* shardBase, const SDK::FName& vanillaShardId) {
    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (!gameInstance) return false;

    auto* shardManager = gameInstance->GetShardManager();
    if (!shardManager || !shardManager->ShardMasterTable) return false;

    auto* shardData = FindShardMasterRow(shardManager->ShardMasterTable, vanillaShardId.ToString());
    if (!shardData) return false;

    SDK::EShardColor shardColor = shardData->ShardColorOverride;
    if (shardColor == SDK::EShardColor::None) shardColor = DefaultShardColor(shardData->ShardType);

    shardBase->ShardId = vanillaShardId;
    shardBase->ShardType = shardData->ShardType;
    shardBase->ShardColor = shardColor;
    shardBase->SetShardType(shardData->ShardType);
    return true;
}

}  // namespace

void HookManager::ResetCompatibilityShardMasterData() {
    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (gameInstance) {
        auto* shardManager = gameInstance->GetShardManager();
        if (shardManager && shardManager->ShardMasterTable) {
            for (const auto& [rowName, appearance] : originalRandomizedShardAppearances) {
                auto* row = FindShardMasterRow(shardManager->ShardMasterTable, rowName);
                if (!row) continue;
                row->ShardType = appearance.type;
                row->ShardColorOverride = appearance.color;
            }
            originalRandomizedShardAppearances.clear();
        }
    }

    auto* dropManager = SDK::UPBDropManager::GetDropManager();
    if (dropManager && dropManager->DropTable) {
        for (const auto& [rowName, dropFlag] : originalRandomizedDropFlags) {
            auto* row = FindDropMasterRow(dropManager->DropTable, rowName);
            if (row) row->DropSpecialFlags = dropFlag;
        }
        originalRandomizedDropFlags.clear();
    }
}

void HookManager::ApplyCompatibilityShardMasterData() {
    auto* archipelago = Archipelago::ConnectedInstance();
    if (!archipelago) return;

    ResetCompatibilityShardMasterData();

    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (!gameInstance) return;
    auto* shardManager = gameInstance->GetShardManager();
    if (!shardManager || !shardManager->ShardMasterTable) return;
    auto* dropManager = SDK::UPBDropManager::GetDropManager();

    const bool autoSellRepeatedShards = QualityOfLife::Instance().IsAutoSellWastedShardsEnabled();
    size_t compatibilityRows = 0;
    size_t unsuppressedDropRows = 0;
    for (const auto& [randomizedName, vanillaName] : knownVanillaShardIds) {
        SDK::FName vanillaShardId = FNameFromString(vanillaName);
        ItemLookupResult itemResult = GetVanillaShardItemSupport(*archipelago, vanillaShardId);
        if (itemResult == ItemLookupResult::NotReady) {
            ResetCompatibilityShardMasterData();
            return;
        }
        const bool needsCompatibility = itemResult == ItemLookupResult::UnknownItem;

        if (needsCompatibility) {
            auto* randomizedRow = FindShardMasterRow(shardManager->ShardMasterTable, randomizedName);
            auto* vanillaRow = FindShardMasterRow(shardManager->ShardMasterTable, vanillaName);
            if (randomizedRow && vanillaRow) {
                originalRandomizedShardAppearances[randomizedName] =
                    ShardAppearance{randomizedRow->ShardType, randomizedRow->ShardColorOverride};
                randomizedRow->ShardType = vanillaRow->ShardType;
                randomizedRow->ShardColorOverride = vanillaRow->ShardColorOverride == SDK::EShardColor::None
                                                          ? DefaultShardColor(vanillaRow->ShardType)
                                                          : vanillaRow->ShardColorOverride;
                compatibilityRows++;
            }
        }

        if ((needsCompatibility || autoSellRepeatedShards) && dropManager && dropManager->DropTable &&
            randomizedName.starts_with("AP_")) {
            std::string dropRowName = randomizedName.substr(3);
            auto* dropRow = FindDropMasterRow(dropManager->DropTable, dropRowName);
            if (dropRow) {
                originalRandomizedDropFlags[dropRowName] = dropRow->DropSpecialFlags;
                dropRow->DropSpecialFlags = SDK::EDropSpecialFlag::None;
                unsuppressedDropRows++;
            }
        }
    }

    Logger::Log(LogLevel::File, "[Shard] Applied shard table patches; compatibility rows:",
                compatibilityRows, "unsuppressed drop rows:", unsuppressedDropRows,
                "auto-sell repeats:", autoSellRepeatedShards);
}

bool HookManager::Init() {
    MH_Initialize();

    constexpr SDK::int32 ProcessLocalScriptFunction = 0x0674B520;
    void* processEventPtr = (void*)(SDK::InSDKUtils::GetImageBase() + SDK::Offsets::ProcessEvent);
    void* processLocalScriptFunctionPtr = (void*)(SDK::InSDKUtils::GetImageBase() + ProcessLocalScriptFunction);
    if (!processEventPtr) {
        Logger::Log(LogLevel::Error, "Failed to get ProcessEvent address");
        return false;
    }
    if (!processLocalScriptFunctionPtr) {
        Logger::Log(LogLevel::Error, "Failed to get processLocalScriptFunction address");
        return false;
    }

    MH_STATUS peStatus = MH_CreateHook(processEventPtr, &HOOKED_ProcessEvent, (void**)&originalProcessEvent);
    MH_STATUS plsfStatus = MH_CreateHook(processLocalScriptFunctionPtr, &HOOKED_ProcessLocalScriptFunction,
                                         (void**)&originalProcessLocalScriptFunction);
    if (peStatus != MH_OK) {
        Logger::Log(LogLevel::Error, "pe MH_CreateHook failed: ", (int)peStatus);
        return false;
    }
    if (plsfStatus != MH_OK) {
        Logger::Log(LogLevel::Error, "plsf MH_CreateHook failed: ", (int)plsfStatus);
        return false;
    }

    peStatus = MH_EnableHook(processEventPtr);
    if (peStatus != MH_OK) {
        Logger::Log(LogLevel::Error, "pe MH_EnableHook failed: ", (int)peStatus);
        return false;
    }
    plsfStatus = MH_EnableHook(processLocalScriptFunctionPtr);
    if (plsfStatus != MH_OK) {
        Logger::Log(LogLevel::Error, "plsf MH_EnableHook failed: ", (int)plsfStatus);
        return false;
    }

    // Register chest lifecycle observers as soon as ProcessEvent is hooked. PostInit deliberately waits for the
    // player and HUD, by which time the streamed level containing a loaded save may already have constructed and
    // begun play. ObserveTreasureActor safely defers native map registration until the HUD component exists.
    const auto observeTreasure = [](void* obj) { InGameTracker::Instance().ObserveTreasureActor(obj); };
    for (const char* className : {"PBEasyTreasureBox_BP_C", "PBPureMiriamTreasureBox_BP_C",
                                  "PBBronzeTreasureBox_BP_C", "PBGoldenTreasureBox_BP_C",
                                  "BP_ChaosTreasureBox_C"}) {
        NotifyOnClassFunction(className, "UserConstructionScript", observeTreasure);
        NotifyOnClassFunction(className, "ReceiveBeginPlay", observeTreasure);
    }

    // When game and player completely load in
    NotifyOnClassFunction("PBGameMode_Miriam_BP_C", "OnLoadGameCompletely", [](void* obj) {
        // The title world and its widgets have already been destroyed by this point.
        // Drop our stale references without invoking a method on the dead widget.
        MainMenuStatus::Instance().Forget();
        GameManager::Instance().PlayerAlive();
        InGameTracker::Instance().LoadDisplayMode();
        QualityOfLife::Instance().LoadSettings();
        InGameTracker::Instance().InvalidateReachability("save loaded");
        Archipelago::Instance().ResetLocalLocationCache();
        Archipelago::Instance().ApplySavedEnemyDropShuffle();
        Gui::Instance().TryAutoConnect();
        APBridge::Instance().EnqueueSync();
        Logger::Log("Player respawned");
    });

    NotifyOnClassFunction("TitleMainMenu_C", "Tick",
                          [](void* obj) { MainMenuStatus::Instance().Show(static_cast<SDK::UObject*>(obj)); });

    Logger::Log("HookManager initialized successfully");
    return true;
}

bool HookManager::PostInit() {
    NotifyOnClassFunction("MapManageBlueprint_C", "Event_MapStart",
                          [](void* obj) { InGameTracker::Instance().ApplyMapMarkers(obj); });
    NotifyOnClassFunction("MapManageBlueprint_C", "Tick",
                          [](void* obj) { InGameTracker::Instance().ApplyDeferredGhostMap(obj); });
    NotifyOnClassFunction("MiniMapBlueprint_C", "Tick",
                          [](void* obj) { InGameTracker::Instance().ApplyMiniMap(obj); });

    NotifyOnClassFunctionWithParams(
        "PBCharacterInventoryComponent", "GetItemWithDisplay",
        [](void* obj, const std::string& functionName, void* rawParams) {
            if (functionName != "GetItemWithDisplay" || !rawParams) return;

            auto* params = static_cast<SDK::Params::PBCharacterInventoryComponent_GetItemWithDisplay*>(rawParams);
            if (params->Quantity <= 0 || !params->ReturnValue) return;

            const std::string nativeItemId = params->newItemId.ToString();
            if (!nativeItemId.empty()) InGameTracker::Instance().ObserveNativeItem(nativeItemId);
        });

    // Whens constantly when the player is alive
    NotifyOnClassFunction("Chr_P0000_C", "GetAdditionalCameraTargetLocations", [](void* obj) {
        if (!Archipelago::ConnectedInstance()) return;
        if (!Archipelago::ConnectedInstance()->IsPendingDeathlink()) return;

        Logger::Log("Pending death link is true");
        // is pending death link is true
        if (GameManager::Instance().CanKillPlayer() && !GameManager::Instance().IsPlayerDead()) {
            Logger::Log("Killing player from deathlink");
            GameManager::Instance().KillPlayer();
            Logger::Log("Killed player");
        }
    });

    // When player dies
    NotifyOnClassFunction("Chr_P0000_C", "Kill", [](void* obj) {
        Logger::Log("Player died");
        GameManager::Instance().PlayerDied();
        Logger::Log("Pending death link", Archipelago::ConnectedInstance()->IsPendingDeathlink());
        // pending death is true
        if (!Archipelago::ConnectedInstance()->IsPendingDeathlink()) {
            Logger::Log("Sent out death link");
            Archipelago::ConnectedInstance()->InvokeDeathLink();  // send out death link
        } else {
            Archipelago::ConnectedInstance()->ResetDeathLink();
            Logger::Log("Resetting death link");
        }
        Logger::Log("End of player died");
    });

    // When player returns to title screen
    // IMPORTANT: Also when player dies and loading screen occurs, the title gets created for whatever reason
    NotifyOnClassFunction("PBTitlePlayerController_C", "ClientRestart", [](void* obj) {
        if (!GameManager::Instance().IsPlayerDead()) {
            if (Archipelago::Instance().IsConnected()) APBridge::Instance().EnqueueDisconnect();
            Gui::Instance().ResetAutoConnect();
        }
        Logger::Log("Returned to title");
    });

    // When Bael is defeated
    NotifyOnClassFunction("Chr_N1013_Dominique_C", "BP_OnKilled",
                          [](void* obj) { Archipelago::Instance().BaelDefeated(); });

    // When player changes room
    NotifyOnClassFunction("PBRoomManager", "OnSerializeGame", [](void* obj) {
        std::string currentRoomId = GameManager::Instance().RoomManager()->GetCurrentRoomId().ToString();
        SDK::UPBGameInstance in;
        Logger::Log("Changed rooms:", currentRoomId);
    });

    // When player saves
    NotifyOnClassFunction("PBGameInstanceBP_C", "OnSaveStoryDataCompletedDelegates_Event_0", [](void* obj) {
        Logger::Log("Player saved game");
    });

    // When the shard that comes out of the enemy appears
    NotifyOnClassFunction("PurpleShard_C", "UserConstructionScript", [](void* obj) {
        auto shardBase = reinterpret_cast<SDK::AShardBase*>(obj);
        auto instance = GameManager::Instance;
        auto roomManager = instance().RoomManager();
        auto roomId = roomManager->GetCurrentRoomId().ToString();

        auto shardId = shardBase->ShardId;
        std::string shardName = shardId.ToString();
        if (!shardName.starts_with("AP_")) return;
        auto* archipelago = Archipelago::ConnectedInstance();
        if (!archipelago) return;

        auto vanillaShardId = ResolveVanillaShardId(shardId);
        if (!vanillaShardId) {
            Logger::Log("[Shard] Could not resolve randomized shard actor to a vanilla shard:", shardName);
            return;
        }

        ItemLookupResult itemResult = GetVanillaShardItemSupport(*archipelago, *vanillaShardId);
        if (itemResult == ItemLookupResult::NotReady) {
            Logger::Log("[Shard] Item data is not ready; leaving shard actor intact:", shardName);
            return;
        }
        if (itemResult == ItemLookupResult::UnknownItem) {
            RestoreVanillaShardAppearance(shardBase, *vanillaShardId);
            return;
        }

        LocationCheckResult checkResult = archipelago->SendLocationChecks(shardName);
        if (checkResult == LocationCheckResult::NotReady) {
            Logger::Log("[Shard] World data is not ready; leaving vanilla shard actor intact:", shardName);
            return;
        }

        if (checkResult == LocationCheckResult::UnknownLocation) {
            Logger::Log("[Shard] Server supports shard items but this shard has no location; suppressing actor:",
                        shardName);
        } else if (checkResult == LocationCheckResult::Sent) {
            instance().GivePlayerItem(shardName, false, 0);
            Logger::Log("[Shard] Sent location check:", shardName);
        } else if (checkResult == LocationCheckResult::AlreadyChecked &&
                   QualityOfLife::Instance().IsAutoSellWastedShardsEnabled()) {
            QualityOfLife::Instance().SellRepeatedShardNow(*vanillaShardId);
            Logger::Log(LogLevel::File, "[Shard] Sold repeated AP shard and suppressed actor:", shardName,
                        "vanilla shard:", vanillaShardId->ToString());
        }

        ((SDK::AActor*)(shardBase))->K2_DestroyActor();
        if (instance().RoomIsBossRoom(roomId)) instance().CheckBossSoftlock();
    });

    // When the item popup in bottom right corner appears
    NotifyOnClassFunction("ItemGetPopup_C", "Tick", [](void* obj) {
        auto* popup = static_cast<SDK::UItemGetPopup_C*>(obj);
        std::string popupText = popup->MLTF_SIZE_23_ItemName->text.ToString();

        if (!popupText.starts_with("AP_")) return;

        if (Archipelago::Instance().SendLocationChecks(popupText) == LocationCheckResult::Sent) {
            Logger::Log("[ItemGetPopup] Sent location check:", popupText);
        }
    });

    Logger::Log("HookManager post initialized successfully");
    return true;
}

void HookManager::ProcessEventBefore(SDK::UObject* obj, SDK::UFunction* func, void* params) {
    if (!obj || !func) return;
    QualityOfLife::Instance().ProcessEventBefore(obj, func, params);
    std::string functionName = func->Name.GetRawString();

    if (functionName == "QuitGame" || functionName == "QuitGameYes") {
        shuttingDown = true;
        Archipelago::Instance().Shutdown();
    }

    // UserConstructionScript selects the shard actor's material. Restore the vanilla
    // ID before that script runs so compatibility shards use their real category color.
    if (functionName == "UserConstructionScript" && obj->Class->Name.ToString() == "PurpleShard_C") {
        auto* shardBase = static_cast<SDK::AShardBase*>(obj);
        std::string shardName = shardBase->ShardId.ToString();
        auto* archipelago = Archipelago::ConnectedInstance();
        if (!archipelago || !shardName.starts_with("AP_")) return;

        auto vanillaShardId = ResolveVanillaShardId(shardBase->ShardId);
        if (!vanillaShardId) return;

        ItemLookupResult itemResult = GetVanillaShardItemSupport(*archipelago, *vanillaShardId);
        if (itemResult != ItemLookupResult::UnknownItem) return;

        RestoreVanillaShardAppearance(shardBase, *vanillaShardId);
        return;
    }

    if (!params || functionName != "CrystallizeDead") return;

    auto* crystallizeParams = static_cast<SDK::Params::PBBaseCharacter_CrystallizeDead*>(params);
    std::string shardName = crystallizeParams->DropShardId.ToString();
    auto* archipelago = Archipelago::ConnectedInstance();
    if (!archipelago || !shardName.starts_with("AP_")) return;

    auto vanillaShardId = ResolveVanillaShardId(crystallizeParams->DropShardId);
    if (!vanillaShardId) {
        Logger::Log("[Shard] Could not resolve randomized shard before crystallization:", shardName);
        return;
    }

    ItemLookupResult itemResult = GetVanillaShardItemSupport(*archipelago, *vanillaShardId);
    if (itemResult != ItemLookupResult::UnknownItem) return;

    crystallizeParams->DropShardId = *vanillaShardId;
}
