#pragma once
#include <set>

#include "Logger.h"
#include "Mod/APBridge.h"
#include "Mod/Archipelago.h"
#include "Mod/GameManager.h"
#include "NotifyObject.h"
#include "SDK.hpp"
#include "ThreadQueue.h"

extern "C" {
#include "MinHook.h"
}

class HookManager {
   public:
    static HookManager& Instance() {
        static HookManager instance;
        return instance;
    }

    bool Init();
    bool PostInit();

    void NotifyOnClassFunction(const std::string& className, const std::string& funcName,
                               NotifyObject::Callback callback) {
        notifyObject.Register(className, funcName, callback);
    }

    void NotifyOnClass(const std::string& className, NotifyObject::Callback callback) {
        notifyObject.Register(className, callback);
    }

    void NotifyOnMatchingClass(const std::string& partialName, NotifyObject::Callback callback) {
        notifyObject.RegisterPartial(partialName, callback);
    }

    void NotifyOnMatchingClass(const std::string& partialName, NotifyObject::CallbackWithFunc callback) {
        notifyObject.RegisterPartial(partialName, callback);
    }

    void NotifyOnClassFunctionWithParams(const std::string& className, const std::string& funcName,
                                         NotifyObject::CallBackWithFuncAndParams callback) {
        notifyObject.RegisterWithFuncNameAndParams(className, funcName, callback);
    }

    static bool playerDetected;
    static bool shuttingDown;
    static void ApplyShardDropPolicy();
    static void ResetShardDropPolicy();

   private:
    HookManager() = default;
    ~HookManager() = default;
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    void ProcessEvent(void* obj, SDK::UFunction* func, void* params) {
        if (!obj || !func) return;

        auto* uobj = static_cast<SDK::UObject*>(obj);
        std::string className = uobj->Class->Name.ToString();
        std::string funcName = func->Name.GetRawString();
        const bool isTick = funcName == "ReceiveTick" || funcName == "Tick";
        if (isTick) {
            ThreadQueue::Instance().Flush();
        }

        notifyObject.OnProcessEvent(obj, className, funcName, params);
    }

    static void HOOKED_ProcessEvent(SDK::UObject* obj, SDK::UFunction* func, void* params) {
        ProcessEventBefore(obj, func, params);
        originalProcessEvent(obj, func, params);
        if (obj && func) {
            HookManager::Instance().ProcessEvent(obj, func, params);
        }
    }

    static void HOOKED_ProcessLocalScriptFunction(SDK::UObject* obj, SDK::UFunction* func, void* params) {
        Logger::Log("Called PLSF");
        //     originalProcessLocalScriptFunction(obj, func, params);
        //     if (obj && func) {
        //         HookManager::Instance().ProcessEvent(obj, func, params);
        //     }
    }

    NotifyObject notifyObject;
    static std::set<std::string> pendingWidgets;
    static std::set<std::string> processedWidgets;
    static void (*originalProcessEvent)(SDK::UObject*, SDK::UFunction*, void*);
    static void (*originalProcessLocalScriptFunction)(SDK::UObject*, SDK::UFunction*, void*);
    using UseConsumableNative = bool (*)(SDK::UPBCharacterInventoryComponent*, SDK::FName, bool, bool);
    using RoomTransitionNative = bool (*)(SDK::UPBRoomManager*, SDK::FName, bool, SDK::FName,
                                          const SDK::FLinearColor*);
    static UseConsumableNative originalUseConsumable;
    static RoomTransitionNative originalRoomTransition;
    static bool pendingPreTownWaystone;
    static bool HOOKED_UseConsumable(SDK::UPBCharacterInventoryComponent* inventory, SDK::FName itemId,
                                     bool noRemove, bool byFamilia);
    static bool HOOKED_RoomTransition(SDK::UPBRoomManager* roomManager, SDK::FName roomId,
                                      bool transitionFlag, SDK::FName preferredSpawnPointName,
                                      const SDK::FLinearColor* fadeColor);
    static void ProcessEventBefore(SDK::UObject* obj, SDK::UFunction* func, void* params);
};
