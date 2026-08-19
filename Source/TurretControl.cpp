#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <API/ARK/Ark.h>
#include <API/UE/Math/ColorList.h>

#if defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS
#include <ArkPermissions.h>
#pragma comment(lib, "Permissions.lib")
#endif

#include "MiniJson.h"

#pragma comment(lib, "ArkApi.lib")

namespace TurretControl {

constexpr const char* kPluginName = "TurretControl";

struct Config {
    std::string fill_command = "/fill";
    std::string turrets_command = "/turrets";
    float fill_radius = 12500.0f;
    int heavy_ammo_limit = 1000;
    int tek_ammo_limit = 5000;
    int auto_ammo_limit = 1000;
    bool require_same_tribe = true;
    bool allow_during_pvp_cooldown = false;
    bool show_messages = true;

    bool use_permissions = false;
    std::string permission = "TurretControl.Default";

    bool vanilla_heavy = true;
    bool vanilla_tek = true;
    bool vanilla_auto = true;
    std::vector<std::string> custom_heavy_classes;
    std::vector<std::string> custom_tek_classes;
    std::vector<std::string> custom_auto_classes;

    int range_low = 0;
    int range_medium = 1;
    int range_high = 2;

    // Deliberately disabled until the server owner supplies verified values.
    // The 3.56 header exposes AISettingField(), but does not document its numeric mapping.
    int targeting_players_only = -1;
    int targeting_players_and_tames = -1;

    std::string sender = "TurretControl";
    std::string no_permission = "You do not have permission.";
    std::string no_turrets = "No valid turrets found.";
    std::string no_ammo = "No suitable ammunition found.";
    std::string already_full = "All valid turrets are already at their configured ammo limit.";
    std::string fill_success = "Filled {0} turrets | ARB used: {1} | Shards used: {2}";
    std::string turret_success = "Updated {0} turrets.";
    std::string targeting_unconfigured = "Targeting command is disabled until TargetingValues are configured.";
    std::string pvp_blocked = "You cannot use /fill during PvP cooldown.";
    std::string reload_ok = "TurretControl config reloaded.";
    std::string reload_failed = "TurretControl config reload failed.";
};

Config g_config;
std::string g_registered_fill_command;
std::string g_registered_turrets_command;
std::vector<UClass*> g_custom_heavy;
std::vector<UClass*> g_custom_tek;
std::vector<UClass*> g_custom_auto;

using PvpCooldownChecker = bool(__fastcall*)(AShooterPlayerController*);
PvpCooldownChecker g_pvp_checker = nullptr;

std::string ConfigPath() {
    return ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/TurretControl/config.json";
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ReplaceToken(std::string text, const std::string& token, const std::string& value) {
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
    return text;
}

FString F(const std::string& utf8) {
    return FString(ArkApi::Tools::Utf8Decode(utf8).c_str());
}

void Send(AShooterPlayerController* pc, const std::string& text) {
    if (!pc || !g_config.show_messages) return;
    const FString sender = F(g_config.sender);
    const FString msg = F(text);
    ArkApi::GetApiUtils().SendChatMessage(pc, sender, *msg);
}

std::string GetClassFullName(UClass* cls) {
    if (!cls) return {};
    UObject* cdo = cls->GetDefaultObject(true);
    if (!cdo) return {};
    FString name;
    cdo->GetFullName(&name, nullptr);
    return name.ToString();
}

std::string GetClassFullName(UObject* obj) {
    return obj ? GetClassFullName(obj->ClassField()) : std::string{};
}

bool IsValidTurret(APrimalStructureTurret* turret) {
    if (!turret) return false;
    if (turret->IsDead()) return false;
    if (!turret->RootComponentField()) return false;
    if (!turret->MyInventoryComponentField()) return false;
    return true;
}

bool ClassMatches(APrimalStructureTurret* turret, const std::vector<UClass*>& classes) {
    if (!turret) return false;
    for (UClass* cls : classes) {
        if (cls && turret->IsA(cls)) return true;
    }
    return false;
}

enum class TurretKind { Unsupported, Heavy, Tek, Auto };

TurretKind DetectTurretKind(APrimalStructureTurret* turret) {
    if (!turret) return TurretKind::Unsupported;

    if (ClassMatches(turret, g_custom_heavy)) return TurretKind::Heavy;
    if (ClassMatches(turret, g_custom_tek)) return TurretKind::Tek;
    if (ClassMatches(turret, g_custom_auto)) return TurretKind::Auto;

    const std::string class_name = ToLower(GetClassFullName(turret));
    const std::string ammo_name = ToLower(GetClassFullName(turret->AmmoItemTemplateField().uClass));

    // Tek is safest to identify by the turret ammo template; this also covers S+/SS derivatives.
    if (g_config.vanilla_tek &&
        (ammo_name.find("elementshard") != std::string::npos ||
         class_name.find("turrettek") != std::string::npos ||
         class_name.find("autoturrettek") != std::string::npos ||
         class_name.find("tek_turret") != std::string::npos)) {
        return TurretKind::Tek;
    }

    // Heavy and normal auto both use ARB, so class identity is used to separate them.
    if (g_config.vanilla_heavy &&
        (class_name.find("heavy") != std::string::npos) &&
        (class_name.find("turret") != std::string::npos)) {
        return TurretKind::Heavy;
    }

    if (g_config.vanilla_auto &&
        class_name.find("turret") != std::string::npos &&
        class_name.find("rocket") == std::string::npos &&
        class_name.find("minigun") == std::string::npos &&
        (ammo_name.find("advancedriflebullet") != std::string::npos ||
         class_name.find("autoturret") != std::string::npos ||
         class_name.find("turret_character") != std::string::npos)) {
        return TurretKind::Auto;
    }

    return TurretKind::Unsupported;
}

int LimitFor(TurretKind kind) {
    switch (kind) {
    case TurretKind::Heavy: return std::max(0, g_config.heavy_ammo_limit);
    case TurretKind::Tek: return std::max(0, g_config.tek_ammo_limit);
    case TurretKind::Auto: return std::max(0, g_config.auto_ammo_limit);
    default: return 0;
    }
}

struct TurretRef {
    APrimalStructureTurret* turret = nullptr;
    TurretKind kind = TurretKind::Unsupported;
    TSubclassOf<UPrimalItem> ammo;
    int current = 0;
    int capacity = 0;
    float distance = 0.0f;
    int planned = 0;
};

std::vector<TurretRef> FindTurrets(AShooterPlayerController* pc, bool fill_only) {
    std::vector<TurretRef> result;
    if (!pc) return result;

    AShooterCharacter* character = pc->GetPlayerCharacter();
    UWorld* world = ArkApi::GetApiUtils().GetWorld();
    if (!character || !world || !character->RootComponentField()) return result;

    const FVector player_pos = character->RootComponentField()->RelativeLocationField();
    const int player_team = ArkApi::GetApiUtils().GetTribeID(pc);

    // ArkApi 3.56 exposes the server spatial octree. Query only structure actors of the
    // turret base class inside the requested radius instead of scanning every actor/turret on the map.
    TArray<AActor*> actors;
    TSubclassOf<AActor> turret_class(APrimalStructureTurret::GetPrivateStaticClass());
    UVictoryCore::ServerOctreeOverlapActorsClass(&actors, world, player_pos, g_config.fill_radius,
        EServerOctreeGroup::STRUCTURES, turret_class, true);

    result.reserve(static_cast<size_t>(actors.Num()));
    for (AActor* actor : actors) {
        if (!actor || !actor->IsA(APrimalStructureTurret::GetPrivateStaticClass())) continue;
        auto* turret = static_cast<APrimalStructureTurret*>(actor);
        if (!IsValidTurret(turret)) continue;

        if (g_config.require_same_tribe && turret->TargetingTeamField() != player_team) continue;

        const FVector turret_pos = turret->RootComponentField()->RelativeLocationField();
        const float distance = FVector::Distance(player_pos, turret_pos);
        if (distance > g_config.fill_radius) continue;

        const TurretKind kind = DetectTurretKind(turret);
        // Filling needs a known ammo/limit profile. Mass power/range control can safely work
        // on any APrimalStructureTurret-derived structure in range, including additional mod turrets.
        if (fill_only && kind == TurretKind::Unsupported) continue;

        TurretRef ref;
        ref.turret = turret;
        ref.kind = kind;
        ref.ammo = turret->AmmoItemTemplateField();
        ref.distance = distance;

        if (fill_only) {
            UPrimalInventoryComponent* inventory = turret->MyInventoryComponentField();
            if (!inventory || !ref.ammo.uClass) continue;
            ref.current = inventory->GetItemTemplateQuantity(ref.ammo, nullptr, true, false, true, true);
            ref.capacity = std::max(0, LimitFor(kind) - ref.current);
        }
        result.emplace_back(ref);
    }

    std::sort(result.begin(), result.end(), [](const TurretRef& a, const TurretRef& b) {
        return a.distance < b.distance;
    });
    return result;
}

int InventoryQuantity(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class) {
    if (!inventory || !item_class.uClass) return 0;
    return inventory->GetItemTemplateQuantity(item_class, nullptr, true, false, true, true);
}

int RemoveFromInventory(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class, int requested) {
    if (!inventory || !item_class.uClass || requested <= 0) return 0;

    // Work on a snapshot, matching the exact ammo class. The return value is based on the
    // measured before/after inventory quantity, so a failed RemoveItem cannot be counted as
    // successfully removed and therefore cannot be turned into duplicated ammo.
    const int before_total = InventoryQuantity(inventory, item_class);
    if (before_total <= 0) return 0;
    const int target = std::min(requested, before_total);

    TArray<UPrimalItem*> matches = inventory->InventoryItemsField();
    int attempted = 0;
    for (UPrimalItem* item : matches) {
        if (attempted >= target) break;
        if (!item || item->ClassField() != item_class.uClass) continue;
        if (!item->bAllowRemovalFromInventory()() || item->bIsEngram()() || item->bIsBlueprint()()) continue;

        const int qty = item->GetItemQuantity();
        if (qty <= 0) continue;
        const int take = std::min(qty, target - attempted);

        if (take < qty) {
            item->SetQuantity(qty - take, true);
            inventory->NotifyClientsItemStatus(item, false, false, true, false, false, nullptr, nullptr, false, false, true);
            attempted += take;
        } else if (inventory->RemoveItem(&item->ItemIDField(), false, false, true, true)) {
            attempted += take;
        }
    }

    const int after_total = InventoryQuantity(inventory, item_class);
    return std::clamp(before_total - after_total, 0, target);
}

int AddToInventory(UPrimalInventoryComponent* inventory, TSubclassOf<UPrimalItem> item_class, int requested) {
    if (!inventory || !item_class.uClass || requested <= 0) return 0;
    const int before = InventoryQuantity(inventory, item_class);
    UPrimalItem* incremented = nullptr;
    inventory->IncrementItemTemplateQuantity(item_class, requested, true, false, nullptr, &incremented,
        true, false, false, false, false, false, true);
    const int after = InventoryQuantity(inventory, item_class);
    return std::max(0, after - before);
}

// Allocates an ammo pool approximately evenly among turrets while respecting each deficit.
void PlanPool(std::vector<TurretRef*>& refs, int available) {
    for (TurretRef* ref : refs) if (ref) ref->planned = 0;
    if (available <= 0 || refs.empty()) return;

    std::vector<TurretRef*> active;
    for (TurretRef* ref : refs) if (ref && ref->capacity > 0) active.push_back(ref);

    while (available > 0 && !active.empty()) {
        const int share = std::max(1, available / static_cast<int>(active.size()));
        bool progress = false;
        for (auto it = active.begin(); it != active.end() && available > 0;) {
            TurretRef* ref = *it;
            const int remaining = std::max(0, ref->capacity - ref->planned);
            if (remaining == 0) {
                it = active.erase(it);
                continue;
            }
            const int give = std::min({remaining, share, available});
            ref->planned += give;
            available -= give;
            progress = progress || give > 0;
            if (ref->planned >= ref->capacity) it = active.erase(it);
            else ++it;
        }
        if (!progress) break;
    }
}

bool HasPermission(AShooterPlayerController* pc) {
    if (!g_config.use_permissions) return true;
#if defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS
    if (!pc) return false;
    const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(pc);
    return Permissions::IsPlayerHasPermission(steam_id, F(g_config.permission));
#else
    // Fail closed when config requests Permissions but the binary was built without it.
    return false;
#endif
}

bool IsPvpBlocked(AShooterPlayerController* pc) {
    if (g_config.allow_during_pvp_cooldown) return false;
    if (!g_pvp_checker) return false; // Core plugin remains independent of PvPCooldowns.
    return g_pvp_checker(pc);
}

void FillCommandImpl(AShooterPlayerController* pc, FString*, EChatSendMode::Type) {
    if (!pc || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    if (!HasPermission(pc)) { Send(pc, g_config.no_permission); return; }
    if (IsPvpBlocked(pc)) { Send(pc, g_config.pvp_blocked); return; }

    AShooterCharacter* character = pc->GetPlayerCharacter();
    if (!character) return;
    UPrimalInventoryComponent* player_inventory = character->MyInventoryComponentField();
    if (!player_inventory) return;

    std::vector<TurretRef> turrets = FindTurrets(pc, true);
    if (turrets.empty()) { Send(pc, g_config.no_turrets); return; }

    bool has_deficit = false;
    for (const auto& t : turrets) has_deficit = has_deficit || t.capacity > 0;
    if (!has_deficit) { Send(pc, g_config.already_full); return; }

    // Separate pools by actual ammo class, not by assumed PrimalItem blueprint path.
    // Heavy/Auto normally share ARB; Tek uses Element Shards. Modded derivatives remain safe.
    struct Pool { TSubclassOf<UPrimalItem> ammo; std::vector<TurretRef*> refs; int available = 0; };
    std::vector<Pool> pools;
    for (auto& ref : turrets) {
        if (ref.capacity <= 0 || !ref.ammo.uClass) continue;
        auto it = std::find_if(pools.begin(), pools.end(), [&](const Pool& p) { return p.ammo.uClass == ref.ammo.uClass; });
        if (it == pools.end()) {
            pools.push_back(Pool{ref.ammo, {}, 0});
            it = pools.end() - 1;
        }
        it->refs.push_back(&ref);
    }

    int total_available = 0;
    for (auto& pool : pools) {
        pool.available = InventoryQuantity(player_inventory, pool.ammo);
        total_available += pool.available;
        PlanPool(pool.refs, pool.available);
    }
    if (total_available <= 0) { Send(pc, g_config.no_ammo); return; }

    int filled_turrets = 0;
    int arb_used = 0;
    int shards_used = 0;

    for (auto& ref : turrets) {
        if (ref.planned <= 0 || !IsValidTurret(ref.turret)) continue;
        UPrimalInventoryComponent* turret_inventory = ref.turret->MyInventoryComponentField();
        if (!turret_inventory || !ref.ammo.uClass) continue;

        // Re-check the live deficit immediately before mutation.
        const int live_before = InventoryQuantity(turret_inventory, ref.ammo);
        const int live_deficit = std::max(0, LimitFor(ref.kind) - live_before);
        const int want = std::min(ref.planned, live_deficit);
        if (want <= 0) continue;

        // Remove first: a failure can at worst lose ammo, never mint extra ammo.
        const int removed = RemoveFromInventory(player_inventory, ref.ammo, want);
        if (removed <= 0) continue;

        const int added = AddToInventory(turret_inventory, ref.ammo, removed);
        if (added < removed) {
            const int refund = removed - added;
            const int refunded = AddToInventory(player_inventory, ref.ammo, refund);
            if (refunded != refund) {
                Log::GetLog()->error("TurretControl: failed to refund {} of {} ammo after partial turret add", refund - refunded, refund);
            }
        }

        // If an unexpected inventory behavior added past our cap, take back only the portion
        // attributable to this command. Never confiscate ammo that was already in the turret.
        int net_added = std::min(removed, added);
        const int live_after = InventoryQuantity(turret_inventory, ref.ammo);
        const int limit = LimitFor(ref.kind);
        if (live_after > limit && net_added > 0) {
            const int overflow_from_this_add = std::min(net_added, live_after - limit);
            const int taken_back = RemoveFromInventory(turret_inventory, ref.ammo, overflow_from_this_add);
            if (taken_back > 0) {
                const int refunded = AddToInventory(player_inventory, ref.ammo, taken_back);
                if (refunded != taken_back) {
                    Log::GetLog()->error("TurretControl: failed to refund {} overflow ammo after safety clamp", taken_back - refunded);
                }
                net_added -= taken_back;
            }
        }

        if (net_added > 0) {
            ++filled_turrets;
            if (ref.kind == TurretKind::Tek) shards_used += net_added;
            else arb_used += net_added;
            ref.turret->UpdateNumBullets();
        }
    }

    std::string message = g_config.fill_success;
    message = ReplaceToken(message, "{0}", std::to_string(filled_turrets));
    message = ReplaceToken(message, "{1}", std::to_string(arb_used));
    message = ReplaceToken(message, "{2}", std::to_string(shards_used));
    Send(pc, message);
}

void TurretsCommandImpl(AShooterPlayerController* pc, FString* message, EChatSendMode::Type) {
    if (!pc || !message || ArkApi::IApiUtils::IsPlayerDead(pc)) return;
    if (!HasPermission(pc)) { Send(pc, g_config.no_permission); return; }

    std::istringstream command_stream(message->ToString());
    std::string command_word;
    std::string action;
    command_stream >> command_word >> action;
    if (action.empty()) {
        Send(pc, "Usage: " + g_config.turrets_command + " on|off|low|medium|high|players|tames");
        return;
    }
    action = ToLower(action);
    std::vector<TurretRef> turrets = FindTurrets(pc, false);
    if (turrets.empty()) { Send(pc, g_config.no_turrets); return; }

    int changed = 0;
    for (auto& ref : turrets) {
        APrimalStructureTurret* turret = ref.turret;
        if (!IsValidTurret(turret)) continue;

        if (action == "on") {
            turret->SetContainerActive(true);
            ++changed;
        } else if (action == "off") {
            turret->SetContainerActive(false);
            ++changed;
        } else if (action == "low" || action == "medium" || action == "high") {
            const int value = action == "low" ? g_config.range_low : (action == "medium" ? g_config.range_medium : g_config.range_high);
            turret->RangeSettingField() = static_cast<char>(value);
            turret->UpdatedTargeting();
            ++changed;
        } else if (action == "players" || action == "tames") {
            const int value = action == "players" ? g_config.targeting_players_only : g_config.targeting_players_and_tames;
            if (value < 0 || value > 255) {
                Send(pc, g_config.targeting_unconfigured);
                return;
            }
            turret->AISettingField() = static_cast<char>(value);
            turret->UpdatedTargeting();
            ++changed;
        } else {
            Send(pc, "Usage: " + g_config.turrets_command + " on|off|low|medium|high|players|tames");
            return;
        }
    }

    std::string msg = ReplaceToken(g_config.turret_success, "{0}", std::to_string(changed));
    Send(pc, msg);
}

void FillCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode) noexcept {
    try {
        FillCommandImpl(pc, message, mode);
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl /fill exception: {}", e.what());
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    } catch (...) {
        Log::GetLog()->error("TurretControl /fill unknown exception");
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    }
}

void TurretsCommand(AShooterPlayerController* pc, FString* message, EChatSendMode::Type mode) noexcept {
    try {
        TurretsCommandImpl(pc, message, mode);
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl /turrets exception: {}", e.what());
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    } catch (...) {
        Log::GetLog()->error("TurretControl /turrets unknown exception");
        Send(pc, "TurretControl encountered an internal error. Check the server log.");
    }
}

UClass* LoadTurretStructureClass(const std::string& path) {
    if (path.empty()) return nullptr;
    FString fpath(path.c_str());
    UClass* cls = UVictoryCore::BPLoadClass(&fpath);
    if (!cls) {
        Log::GetLog()->warn("TurretControl: custom turret class did not load: {}", path);
        return nullptr;
    }
    UObject* cdo = cls->GetDefaultObject(true);
    if (!cdo || !cdo->IsA(APrimalStructureTurret::GetPrivateStaticClass())) {
        Log::GetLog()->warn("TurretControl: ignored custom class because it is not an APrimalStructureTurret structure class: {}", path);
        return nullptr;
    }
    return cls;
}

void LoadCustomClasses() {
    g_custom_heavy.clear(); g_custom_tek.clear(); g_custom_auto.clear();
    for (const auto& p : g_config.custom_heavy_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_heavy.push_back(c);
    for (const auto& p : g_config.custom_tek_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_tek.push_back(c);
    for (const auto& p : g_config.custom_auto_classes) if (UClass* c = LoadTurretStructureClass(p)) g_custom_auto.push_back(c);
}

Config ParseConfig(const minijson::Value& root) {
    Config c;
    c.fill_command = minijson::str(root, "General", "FillCommand", c.fill_command);
    c.turrets_command = minijson::str(root, "General", "TurretsCommand", c.turrets_command);
    c.fill_radius = minijson::number(root, "General", "FillRadius", c.fill_radius);
    c.heavy_ammo_limit = minijson::integer(root, "General", "HeavyAmmoLimit", c.heavy_ammo_limit);
    c.tek_ammo_limit = minijson::integer(root, "General", "TekAmmoLimit", c.tek_ammo_limit);
    c.auto_ammo_limit = minijson::integer(root, "General", "AutoTurretAmmoLimit", c.auto_ammo_limit);
    c.require_same_tribe = minijson::boolean(root, "General", "RequireSameTribe", c.require_same_tribe);
    c.allow_during_pvp_cooldown = minijson::boolean(root, "General", "AllowDuringPvpCooldown", c.allow_during_pvp_cooldown);
    c.show_messages = minijson::boolean(root, "General", "ShowMessages", c.show_messages);

    c.use_permissions = minijson::boolean(root, "Permissions", "UsePermissions", c.use_permissions);
    c.permission = minijson::str(root, "Permissions", "DefaultPermission", c.permission);

    c.vanilla_heavy = minijson::boolean(root, "Turrets", "VanillaHeavy", c.vanilla_heavy);
    c.vanilla_tek = minijson::boolean(root, "Turrets", "VanillaTek", c.vanilla_tek);
    c.vanilla_auto = minijson::boolean(root, "Turrets", "VanillaAuto", c.vanilla_auto);
    c.custom_heavy_classes = minijson::strings(root, "Turrets", "CustomHeavyClasses");
    c.custom_tek_classes = minijson::strings(root, "Turrets", "CustomTekClasses");
    c.custom_auto_classes = minijson::strings(root, "Turrets", "CustomAutoClasses");

    c.range_low = minijson::integer(root, "RangeValues", "Low", c.range_low);
    c.range_medium = minijson::integer(root, "RangeValues", "Medium", c.range_medium);
    c.range_high = minijson::integer(root, "RangeValues", "High", c.range_high);
    c.targeting_players_only = minijson::integer(root, "TargetingValues", "PlayersOnly", c.targeting_players_only);
    c.targeting_players_and_tames = minijson::integer(root, "TargetingValues", "PlayersAndTames", c.targeting_players_and_tames);

    c.sender = minijson::str(root, "Messages", "Sender", c.sender);
    c.no_permission = minijson::str(root, "Messages", "NoPermission", c.no_permission);
    c.no_turrets = minijson::str(root, "Messages", "NoTurrets", c.no_turrets);
    c.no_ammo = minijson::str(root, "Messages", "NoAmmo", c.no_ammo);
    c.already_full = minijson::str(root, "Messages", "AlreadyFull", c.already_full);
    c.fill_success = minijson::str(root, "Messages", "FillSuccess", c.fill_success);
    c.turret_success = minijson::str(root, "Messages", "TurretSuccess", c.turret_success);
    c.targeting_unconfigured = minijson::str(root, "Messages", "TargetingUnconfigured", c.targeting_unconfigured);
    c.pvp_blocked = minijson::str(root, "Messages", "PvpBlocked", c.pvp_blocked);
    c.reload_ok = minijson::str(root, "Messages", "ReloadOk", c.reload_ok);
    c.reload_failed = minijson::str(root, "Messages", "ReloadFailed", c.reload_failed);

    c.fill_radius = std::max(100.0f, c.fill_radius);
    c.heavy_ammo_limit = std::max(0, c.heavy_ammo_limit);
    c.tek_ammo_limit = std::max(0, c.tek_ammo_limit);
    c.auto_ammo_limit = std::max(0, c.auto_ammo_limit);
    return c;
}

void ReadConfig() {
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Can't open " + ConfigPath());
    std::ostringstream ss;
    ss << file.rdbuf();
    const minijson::Value root = minijson::parse(ss.str());
    if (!root.is_object()) throw std::runtime_error("config root must be a JSON object");
    g_config = ParseConfig(root);

#if !(defined(TURRETCONTROL_WITH_PERMISSIONS) && TURRETCONTROL_WITH_PERMISSIONS)
    if (g_config.use_permissions) {
        Log::GetLog()->warn("TurretControl: UsePermissions=true but this DLL was built without Permissions support; commands will fail closed.");
    }
#endif
}

void RegisterChatCommands();

void UnregisterChatCommands() {
    if (!g_registered_fill_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(F(g_registered_fill_command));
        g_registered_fill_command.clear();
    }
    if (!g_registered_turrets_command.empty()) {
        ArkApi::GetCommands().RemoveChatCommand(F(g_registered_turrets_command));
        g_registered_turrets_command.clear();
    }
}

void RegisterChatCommands() {
    g_registered_fill_command = g_config.fill_command;
    g_registered_turrets_command = g_config.turrets_command;
    ArkApi::GetCommands().AddChatCommand(F(g_registered_fill_command), &FillCommand);
    ArkApi::GetCommands().AddChatCommand(F(g_registered_turrets_command), &TurretsCommand);
}

void ReloadCommand(APlayerController* player_controller, FString*, bool) {
    auto* pc = player_controller && player_controller->IsA(AShooterPlayerController::GetPrivateStaticClass())
        ? static_cast<AShooterPlayerController*>(player_controller) : nullptr;
    try {
        const std::string old_fill = g_registered_fill_command;
        const std::string old_turrets = g_registered_turrets_command;
        ReadConfig();
        LoadCustomClasses();
        if (g_config.fill_command != old_fill || g_config.turrets_command != old_turrets) {
            UnregisterChatCommands();
            RegisterChatCommands();
        }
        if (pc) ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Green, g_config.reload_ok.c_str());
        Log::GetLog()->info("TurretControl config reloaded");
    } catch (const std::exception& e) {
        if (pc) ArkApi::GetApiUtils().SendServerMessage(pc, FColorList::Red, e.what());
        Log::GetLog()->error("TurretControl reload failed: {}", e.what());
    }
}

void Load() {
    Log::Get().Init(kPluginName);
    ReadConfig();
    LoadCustomClasses();
    RegisterChatCommands();
    ArkApi::GetCommands().AddConsoleCommand("TurretControl.Reload", &ReloadCommand);
    Log::GetLog()->info("Loaded plugin - TurretControl");
}

void Unload() {
    UnregisterChatCommands();
    ArkApi::GetCommands().RemoveConsoleCommand("TurretControl.Reload");
    g_pvp_checker = nullptr;
    g_custom_heavy.clear(); g_custom_tek.clear(); g_custom_auto.clear();
}

} // namespace TurretControl

extern "C" __declspec(dllexport) void __fastcall Plugin_Init() noexcept {
    try {
        TurretControl::Load();
    } catch (const std::exception& e) {
        Log::Get().Init("TurretControl");
        Log::GetLog()->error("TurretControl failed to initialize: {}", e.what());
    } catch (...) {
        Log::Get().Init("TurretControl");
        Log::GetLog()->error("TurretControl failed to initialize with an unknown exception");
    }
}

extern "C" __declspec(dllexport) void __fastcall Plugin_Unload() noexcept {
    try {
        TurretControl::Unload();
    } catch (const std::exception& e) {
        Log::GetLog()->error("TurretControl unload exception: {}", e.what());
    } catch (...) {
        Log::GetLog()->error("TurretControl unload unknown exception");
    }
}

// Optional bridge for a separate PvPCooldowns adapter. No PvPCooldowns symbols are invented or linked here.
extern "C" __declspec(dllexport) void __fastcall TurretControl_SetPvpCooldownChecker(TurretControl::PvpCooldownChecker checker) {
    TurretControl::g_pvp_checker = checker;
}
