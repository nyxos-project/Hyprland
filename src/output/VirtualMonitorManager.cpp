#include "VirtualMonitorManager.hpp"
#include "Monitor.hpp"

#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/workspace/WorkspaceRuleManager.hpp"
#include "../debug/log/Logger.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../event/EventBus.hpp"
#include "../ipc/s2/S2.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/eventLoop/EventLoopTimer.hpp"
#include "../managers/fullscreen/FullscreenController.hpp"
#include "../notification/NotificationOverlay.hpp"
#include "../pointer/PointerManager.hpp"
#include "../pointer/PointerController.hpp"
#include "../render/Renderer.hpp"
#include "../state/MonitorLayoutController.hpp"
#include "../state/MonitorState.hpp"
#include "../state/workspace/PlacementController.hpp"
#include "../state/workspace/State.hpp"

#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numeric>
#include <ranges>

using namespace Monitor;
using namespace Monitor::Virtual;
using namespace Hyprutils::Utils;

UP<CVirtualMonitorManager>& Monitor::virtualMonitors() {
    static UP<CVirtualMonitorManager> p = makeUnique<CVirtualMonitorManager>();
    return p;
}

static std::string regionName(const std::string& host, size_t index) {
    return std::format("{}.v{}", host, index + 1);
}

static SP<Aquamarine::IBackendImplementation> headlessBackend() {
    if (!g_pCompositor || !g_pCompositor->m_aqBackend)
        return nullptr;

    for (const auto& impl : g_pCompositor->m_aqBackend->getImplementations()) {
        if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS)
            return impl;
    }

    return nullptr;
}

static void notify(const std::string& text, bool error) {
    LOG(error ? Log::ERR : Log::DEBUG, "virtual monitors: {}", text);
    if (Notification::overlay())
        Notification::overlay()->addNotification(std::format("Virtual monitors: {}", text), error ? CHyprColor(1.0, 0.2, 0.2, 1.0) : CHyprColor(0), 5000,
                                                 error ? ICON_WARNING : ICON_INFO);
}

CVirtualMonitorManager::CVirtualMonitorManager() {
    m_listeners.newMonitor = Event::bus()->m_events.monitor.newMon.listen([this](PHLMONITOR mon) {
        const auto PENDING = m_pendingRegions.find(mon->m_name);
        if (PENDING == m_pendingRegions.end())
            return;

        auto&      group = *PENDING->second.group;
        const auto INDEX = PENDING->second.index;
        const auto HOST  = group.host.lock();
        if (!HOST)
            return;

        mon->m_virtual.host          = HOST;
        mon->m_virtual.index         = INDEX + 1;
        mon->m_virtual.count         = group.spans.size();
        mon->m_virtual.pixelOffset   = {sc<double>(group.spans[INDEX].x), 0.0};
        mon->m_virtual.workspaceBase = workspaceBase(group, INDEX);

        // the region describes itself as part of the host, never as a connector of its own
        const auto& HOSTOUT         = HOST->m_output;
        mon->m_output->make         = HOSTOUT->make;
        mon->m_output->model        = HOSTOUT->model;
        mon->m_output->serial       = std::format("{}#v{}", HOSTOUT->serial, INDEX + 1);
        mon->m_output->description  = std::format("Virtual {} of {} on {} ({})", INDEX + 1, group.spans.size(), HOST->m_shortDescription, mon->m_name);
        mon->m_output->physicalSize = {HOSTOUT->physicalSize.x * sc<double>(group.spans[INDEX].width) / std::max(1.0, HOST->m_pixelSize.x), HOSTOUT->physicalSize.y};
    });

    m_listeners.monitorAdded = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR mon) {
        if (!mon->isVirtualRegion() && !m_configs.empty())
            scheduleReconcile();
    });

    m_listeners.monitorPreRemoved = Event::bus()->m_events.monitor.preRemoved.listen([this](PHLMONITOR mon) {
        if (m_reconciling)
            return;

        if (std::ranges::any_of(m_groups, [&mon](const auto& g) { return g->host == mon; })) {
            onHostGone(mon);
            return;
        }

        if (mon->isVirtualRegion()) {
            LOG(Log::WARN, "virtual monitors: region {} went away on its own, splitting its host back together", mon->m_name);
            scheduleReconcile();
        }
    });

    m_listeners.configReloaded   = Event::bus()->m_events.config.reloaded.listen([this] { scheduleReconcile(); });
    m_listeners.windowFullscreen = Event::bus()->m_events.window.fullscreen.listen([this](PHLWINDOW w) { onWindowFullscreen(w); });
    m_listeners.windowClose      = Event::bus()->m_events.window.close.listen([this](PHLWINDOW w) { onWindowGone(w); });
}

CVirtualMonitorManager::~CVirtualMonitorManager() {
    cancelPendingConfirm();
}

bool CVirtualMonitorManager::available() const {
    static auto       PENABLED = CConfigValue<Config::INTEGER>("experimental:virtual_monitors");
    static const bool ENV_OFF  = [] {
        const auto ENV = getenv("HYPRLAND_NO_VIRTUAL_MONITORS");
        return ENV && std::string_view{ENV} != "0" && std::string_view{ENV} != "";
    }();

    return *PENABLED && !ENV_OFF && !m_killed;
}

bool CVirtualMonitorManager::killed() const {
    return m_killed;
}

void CVirtualMonitorManager::beginConfigReload() {
    // the workspace rules were just cleared with every other rule
    for (auto& g : m_groups) {
        g->rules.clear();
    }

    m_configs.clear();
    m_killed = false;
    cancelPendingConfirm();
}

std::expected<void, std::string> CVirtualMonitorManager::declare(SVirtualMonitorConfig&& config, std::optional<int> confirmSeconds) {
    if (config.output.empty())
        return std::unexpected("'output' is required");

    if (config.ratios.size() > MAX_REGIONS)
        return std::unexpected(std::format("at most {} regions are supported", MAX_REGIONS));

    if (std::ranges::any_of(config.ratios, [](double r) { return !std::isfinite(r) || r <= 0; }))
        return std::unexpected("region proportions must be positive");

    config.workspaces = std::clamp(config.workspaces, 0, 10);

    const auto EXISTING = std::ranges::find_if(m_configs, [&config](const auto& c) { return c.output == config.output; });

    if (confirmSeconds && *confirmSeconds > 0) {
        cancelPendingConfirm();

        std::optional<SVirtualMonitorConfig> previous;
        if (EXISTING != m_configs.end())
            previous = *EXISTING;

        auto timer = makeShared<CEventLoopTimer>(
            std::chrono::seconds(*confirmSeconds),
            [this](SP<CEventLoopTimer>, void*) {
                if (!m_pendingConfirm)
                    return;

                auto pending = std::move(*m_pendingConfirm);
                m_pendingConfirm.reset();

                std::erase_if(m_configs, [&pending](const auto& c) { return c.output == pending.output; });
                if (pending.previous)
                    m_configs.emplace_back(std::move(*pending.previous));

                notify(std::format("the new layout for {} wasn't confirmed, so the previous one is back", pending.output), false);
                scheduleReconcile();
            },
            nullptr);

        m_pendingConfirm = SPendingConfirm{.output = config.output, .previous = std::move(previous), .timer = timer};
        g_pEventLoopManager->addTimer(timer);
    }

    if (EXISTING != m_configs.end())
        *EXISTING = std::move(config);
    else
        m_configs.emplace_back(std::move(config));

    m_killed = false;
    scheduleReconcile();
    return {};
}

std::expected<void, std::string> CVirtualMonitorManager::confirm() {
    if (!m_pendingConfirm)
        return std::unexpected("there is no layout change waiting to be confirmed");

    cancelPendingConfirm();
    return {};
}

void CVirtualMonitorManager::cancelPendingConfirm() {
    if (!m_pendingConfirm)
        return;

    if (m_pendingConfirm->timer) {
        m_pendingConfirm->timer->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(m_pendingConfirm->timer);
    }

    m_pendingConfirm.reset();
}

std::optional<int> CVirtualMonitorManager::pendingConfirmSeconds() const {
    if (!m_pendingConfirm || !m_pendingConfirm->timer)
        return std::nullopt;

    return sc<int>(std::ceil(m_pendingConfirm->timer->leftUs() / 1000000.F));
}

void CVirtualMonitorManager::disableAll(const std::string& reason) {
    LOG(Log::WARN, "virtual monitors: disabling all until the next config reload: {}", reason);
    m_killed = true;
    cancelPendingConfirm();
    reconcile();
}

void CVirtualMonitorManager::scheduleReconcile() {
    if (m_reconcileScheduled || !g_pEventLoopManager)
        return;

    m_reconcileScheduled = true;
    g_pEventLoopManager->doLater([this] {
        m_reconcileScheduled = false;
        reconcile();
    });
}

PHLMONITOR CVirtualMonitorManager::findHost(const SVirtualMonitorConfig& config) const {
    for (const auto& m : State::monitorState()->allMonitors()) {
        if (!m || !m->m_output || !m->m_enabled || m->isVirtualRegion() || m->isMirror())
            continue;

        if (m->matchesStaticSelector(config.output))
            return m;
    }

    return nullptr;
}

void CVirtualMonitorManager::reconcile() {
    if (m_reconciling)
        return;

    m_reconciling = true;
    CScopeGuard x([this] { m_reconciling = false; });

    if (!available()) {
        for (auto& g : m_groups) {
            if (!g->regions.empty())
                dissolve(*g, false);
        }
        m_groups.clear();
        return;
    }

    for (size_t i = 0; i < m_configs.size(); ++i) {
        const auto& CONFIG = m_configs[i];
        const auto  HOST   = findHost(CONFIG);

        auto        it    = std::ranges::find_if(m_groups, [&CONFIG](const auto& g) { return g->config.output == CONFIG.output; });
        auto*       group = it == m_groups.end() ? nullptr : it->get();

        if (group && group->host && HOST && group->host != HOST) {
            if (!group->regions.empty())
                dissolve(*group, false);
            m_groups.erase(it);
            group = nullptr;
        }

        if (!HOST)
            continue;

        if (CONFIG.ratios.size() < 2) {
            if (group) {
                if (!group->regions.empty())
                    dissolve(*group, false);
                std::erase_if(m_groups, [group](const auto& g) { return g.get() == group; });
            }
            continue;
        }

        if (!group) {
            auto& created      = m_groups.emplace_back(makeUnique<SVirtualGroup>());
            created->host      = HOST;
            created->hostName  = HOST->m_name;
            created->config    = CONFIG;
            created->firstSlot = i * MAX_REGIONS + 1;

            if (auto res = activate(*created); !res) {
                notify(std::format("couldn't split {}: {}", HOST->m_name, res.error()), true);
                m_groups.pop_back();
            }
            continue;
        }

        const bool COUNT_CHANGED = group->config.ratios.size() != CONFIG.ratios.size();
        group->config            = CONFIG;
        group->firstSlot         = i * MAX_REGIONS + 1;

        if (group->merged)
            continue;

        if (group->regions.empty() || COUNT_CHANGED) {
            if (!group->regions.empty())
                dissolve(*group, false);
            if (auto res = activate(*group); !res) {
                notify(std::format("couldn't split {}: {}", HOST->m_name, res.error()), true);
                std::erase_if(m_groups, [group](const auto& g) { return g.get() == group; });
            }
            continue;
        }

        updateInPlace(*group);
    }

    // groups nothing asks for any more
    std::erase_if(m_groups, [this](auto& g) {
        if (std::ranges::any_of(m_configs, [&g](const auto& c) { return c.output == g->config.output; }))
            return false;

        if (!g->regions.empty())
            dissolve(*g, false);
        return true;
    });
}

uint32_t CVirtualMonitorManager::workspaceBase(const SVirtualGroup& group, size_t index) const {
    // 0: the config numbers each monitor's workspaces itself (Omarchy does), so regions own none
    if (group.config.workspaces == 0)
        return 0;

    const auto SLOT = group.firstSlot + index;
    // the first region of the first split output keeps the ordinary workspaces, so splitting moves nothing
    return SLOT == 1 ? 0 : sc<uint32_t>(SLOT * 100);
}

size_t CVirtualMonitorManager::regionForWorkspace(const SVirtualGroup& group, const PHLWORKSPACE& workspace) const {
    const auto ID = workspace->numberedID();
    if (!ID)
        return 0;

    for (size_t i = 0; i < group.spans.size(); ++i) {
        const auto BASE = workspaceBase(group, i);
        if (BASE != 0 && *ID > BASE && *ID < BASE + 100)
            return i;
    }

    return 0;
}

void CVirtualMonitorManager::addWorkspaceRules(SVirtualGroup& group) {
    removeWorkspaceRules(group);

    for (size_t i = 0; i < group.spans.size(); ++i) {
        const auto BASE = workspaceBase(group, i);
        if (BASE == 0)
            continue;

        for (int n = 1; n <= group.config.workspaces; ++n) {
            Config::CWorkspaceRule rule;
            rule.m_workspaceString = std::to_string(BASE + n);
            rule.m_monitor         = regionName(group.hostName, i);
            rule.m_isPersistent    = true;
            rule.m_defaultName     = std::to_string(n);
            if (n == 1)
                rule.m_isDefault = true;

            group.rules.emplace_back(Config::workspaceRuleMgr()->add(std::move(rule)));
        }
    }
}

void CVirtualMonitorManager::removeWorkspaceRules(SVirtualGroup& group) {
    for (const auto& r : group.rules) {
        Config::workspaceRuleMgr()->remove(r);
    }

    group.rules.clear();
}

// Only rules bound to a monitor: Hyprland moves a persistent workspace whose rule names
// none to the focused monitor, which after a split or restore would pull the first
// region's workspaces onto whichever region has focus.
// The lowest-numbered workspace on a monitor, for a region with nothing to show yet.
static PHLWORKSPACE lowestWorkspaceOn(const PHLMONITOR& mon) {
    PHLWORKSPACE best;
    for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
        const auto ID = ws && ws->m_monitor == mon ? ws->numberedID() : std::nullopt;
        if (ID && (!best || *ID < *best->numberedID()))
            best = ws;
    }
    return best;
}

// Moving workspaces between monitors without replacing leaves their visibility as it was,
// so a region could draw a workspace it doesn't show. Only each monitor's active one is.
static void syncVisibility(const std::vector<PHLMONITOR>& monitors) {
    for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
        if (!ws || ws->type() == ::Workspace::eWorkspaceType::SPECIAL)
            continue;
        const auto ON = ws->m_monitor.lock();
        if (ON && std::ranges::find(monitors, ON) != monitors.end())
            ws->setVisible(ON->m_activeWorkspace == ws);
    }
}

static void ensurePersistentWorkspaces() {
    std::vector<SP<Config::CWorkspaceRule>> bound;
    for (const auto& r : Config::workspaceRuleMgr()->getAllWorkspaceRules()) {
        if (!r->m_monitor.empty())
            bound.emplace_back(r);
    }

    State::Workspace::placementController()->ensurePersistentWorkspacesPresent(
        bound, nullptr, [](PHLWORKSPACE ws, PHLMONITOR mon, bool noWarp) { State::Workspace::placementController()->moveWorkspaceToMonitor(ws, mon, noWarp); });
}

std::expected<void, std::string> CVirtualMonitorManager::activate(SVirtualGroup& group) {
    static auto PMINWIDTH = CConfigValue<Config::INTEGER>("virtual_monitors:min_region_width");

    const auto  HOST = group.host.lock();
    if (!HOST || !HOST->m_enabled || !HOST->m_output)
        return std::unexpected("the output isn't enabled");
    if (HOST->isMirror())
        return std::unexpected("the output mirrors another one");
    if (HOST->isVirtualRegion() || HOST->isVirtualHost())
        return std::unexpected("the output is already virtual");
    if (HOST->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        return std::unexpected("rotated outputs can't be split yet");

    auto spans = partition(sc<int64_t>(HOST->m_pixelSize.x), HOST->m_scale, group.config.ratios, *PMINWIDTH);
    if (!spans)
        return std::unexpected(spans.error());

    const auto HEADLESS = headlessBackend();
    if (!HEADLESS)
        return std::unexpected("no headless backend to create regions with");

    group.hostName = HOST->m_name;
    group.spans    = std::move(*spans);
    group.merged   = false;

    addWorkspaceRules(group);

    std::vector<PHLMONITOR> regions;
    const auto              FAIL = [&](std::string why) -> std::expected<void, std::string> {
        for (const auto& r : regions) {
            r->m_virtual.host.reset();
            if (r->m_output)
                r->m_output->destroy();
        }
        removeWorkspaceRules(group);
        group.regions.clear();
        return std::unexpected(std::move(why));
    };

    for (size_t i = 0; i < group.spans.size(); ++i) {
        const auto NAME = regionName(group.hostName, i);
        if (std::ranges::any_of(State::monitorState()->allMonitors(), [&NAME](const auto& m) { return m->m_name == NAME; }))
            return FAIL(std::format("a monitor named {} already exists", NAME));

        m_pendingRegions[NAME] = {.group = &group, .index = i};
        HEADLESS->createOutput(NAME);
        m_pendingRegions.erase(NAME);

        const auto REGION = std::ranges::find_if(State::monitorState()->allMonitors(), [&NAME](const auto& m) { return m->m_name == NAME; });
        if (REGION == State::monitorState()->allMonitors().end() || !(*REGION)->isVirtualRegion())
            return FAIL(std::format("couldn't create {}", NAME));

        regions.emplace_back(*REGION);
        if (!(*REGION)->m_enabled)
            return FAIL(std::format("{} didn't come up", NAME));
    }

    // from here on the host only shows its regions
    const auto HOST_ACTIVE  = HOST->m_activeWorkspace;
    const auto HOST_SPECIAL = HOST->m_activeSpecialWorkspace;
    const bool HOST_FOCUSED = Desktop::focusState()->monitor() == HOST;

    Pointer::mgr()->lockSoftwareForMonitor(HOST);

    if (HOST_SPECIAL)
        HOST->setSpecialWorkspace(nullptr);

    group.regions.clear();
    for (const auto& r : regions) {
        group.regions.emplace_back(r);
    }
    HOST->m_virtual.regions = group.regions;
    HOST->m_virtual.count   = regions.size();

    // What was on a region before a merge goes back there, wherever creating the regions
    // left it (Hyprland moves some workspaces to each new monitor); the rest of the host's
    // goes by ID range.
    for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
        if (!ws)
            continue;

        const auto ID     = ws->numberedID();
        const auto HOME   = ID ? group.savedHome.find(*ID) : group.savedHome.end();
        const bool SAVED  = HOME != group.savedHome.end() && HOME->second < regions.size();
        const auto ON     = ws->m_monitor.lock();
        const bool OURS   = ON == HOST || std::ranges::find(regions, ON) != regions.end();
        if (!OURS || (!SAVED && ON != HOST))
            continue;

        const auto TARGET = regions[SAVED ? HOME->second : regionForWorkspace(group, ws)];
        if (ON != TARGET)
            State::Workspace::placementController()->moveWorkspaceToMonitor(ws, TARGET, true, false, false);
    }
    HOST->m_activeWorkspace.reset();
    const auto HOME_COPY = std::move(group.savedHome);
    group.savedHome.clear();

    for (size_t i = 0; i < regions.size(); ++i) {
        PHLWORKSPACE target;

        if (i < group.savedActive.size() && group.savedActive[i]) {
            const ::Workspace::SWorkspaceNumberedID ID{*group.savedActive[i]};
            target = State::Workspace::state()->query().numbered(ID).run();
            // an empty workspace may have been cleaned up while merged; bring it back where it was
            if (!target)
                target = State::Workspace::state()->createNumbered(ID, regions[i]);
        } else if (HOST_ACTIVE && HOST_ACTIVE->m_monitor == regions[i])
            target = HOST_ACTIVE;
        else if (!regions[i]->m_activeWorkspace || regions[i]->m_activeWorkspace->m_monitor != regions[i])
            target = lowestWorkspaceOn(regions[i]);

        if (target && target->m_monitor == regions[i] && regions[i]->m_activeWorkspace != target)
            regions[i]->changeWorkspace(target, false, true, true);
    }
    syncVisibility(regions);

    if (HOST_SPECIAL && HOST_SPECIAL->m_monitor == regions.front())
        regions.front()->setSpecialWorkspace(HOST_SPECIAL, true);

    if (HOST_FOCUSED || !Desktop::focusState()->monitor()) {
        // after a merge, back to the region that had focus; on a fresh split, the one under the pointer
        PHLMONITOR focus;
        if (!group.savedActive.empty())
            focus = regions[std::min(group.savedFocus, regions.size() - 1)];
        else {
            focus = State::monitorState()->query().vec(Pointer::mgr()->position()).run();
            if (!focus || !focus->isVirtualRegion() || focus->m_virtual.host != HOST)
                focus = regions.front();
        }
        Desktop::focusState()->rawMonitorFocus(focus);
    }

    // Creating the regions schedules a monitor rule reload (run before the next frame),
    // and its persistent workspace pass moves workspaces whose rule names no monitor to
    // the focused region. After a restore, put everything back once more when it has run.
    if (!group.savedActive.empty()) {
        const auto PUT = [this, host = group.hostName, home = HOME_COPY, active = group.savedActive, focus = group.savedFocus, pointer = group.savedPointer] {
            const auto IT = std::ranges::find_if(m_groups, [&host](const auto& g) { return g->hostName == host; });
            if (IT != m_groups.end() && !(*IT)->merged && !(*IT)->regions.empty())
                putBack(**IT, home, active, focus, pointer);
        };
        m_putBackAfterReload = Config::monitorRuleMgr()->m_events.stateReloaded.listen([this, PUT] {
            m_putBackAfterReload.reset();
            PUT();
        });
        // and soon, in case no reload comes
        if (g_pEventLoopManager)
            g_pEventLoopManager->doLater(PUT);
    }

    group.savedActive.clear();
    group.savedFocus = 0;
    group.savedPointer.reset();

    HOST->m_events.modeChanged.emit();
    group.hostModeChanged = HOST->m_events.modeChanged.listen([this] { scheduleReconcile(); });
    Event::bus()->m_events.monitor.layoutChanged.emit();
    State::monitorLayoutController()->arrange();
    ensurePersistentWorkspaces();

    HOST->m_forceFullFrames = 2;
    HOST->addDamage(CBox{0, 0, INT16_MAX, INT16_MAX});
    for (const auto& r : regions) {
        g_pHyprRenderer->damageMonitor(r);
    }

    LOG(Log::DEBUG, "virtual monitors: split {} into {} regions", HOST->m_name, regions.size());
    postEvent(group, "split");
    return {};
}

void CVirtualMonitorManager::dissolve(SVirtualGroup& group, bool forMerge) {
    const bool WAS_RECONCILING = m_reconciling;
    m_reconciling              = true;
    CScopeGuard             x([this, WAS_RECONCILING] { m_reconciling = WAS_RECONCILING; });

    const auto              HOST = group.host.lock();
    std::vector<PHLMONITOR> regions;
    for (const auto& r : group.regions) {
        if (const auto R = r.lock())
            regions.emplace_back(R);
    }

    const auto FOCUSED      = Desktop::focusState()->monitor();
    const auto FOCUSED_IT   = std::ranges::find(regions, FOCUSED);
    const bool FOCUS_INSIDE = FOCUSED_IT != regions.end();
    const auto SHOW_FROM    = FOCUS_INSIDE ? *FOCUSED_IT : (regions.empty() ? nullptr : regions.front());
    const auto SHOW         = SHOW_FROM ? SHOW_FROM->m_activeWorkspace : nullptr;

    if (forMerge) {
        group.savedActive.clear();
        for (const auto& r : regions) {
            group.savedActive.emplace_back(r->m_activeWorkspace ? r->m_activeWorkspace->numberedID() : std::nullopt);
        }
        group.savedFocus   = FOCUS_INSIDE ? sc<size_t>(std::ranges::distance(regions.begin(), FOCUSED_IT)) : 0;
        group.savedPointer = Pointer::mgr()->position();

        group.savedHome.clear();
        for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
            const auto ON = ws ? std::ranges::find(regions, ws->m_monitor.lock()) : regions.end();
            if (const auto ID = ON != regions.end() ? ws->numberedID() : std::nullopt)
                group.savedHome[*ID] = sc<size_t>(std::ranges::distance(regions.begin(), ON));
        }
    }

    group.hostModeChanged.reset();

    if (HOST) {
        HOST->m_virtual.regions.clear();
        HOST->m_virtual.count = 0;
        HOST->m_events.modeChanged.emit();
        Event::bus()->m_events.monitor.layoutChanged.emit();
        Pointer::mgr()->unlockSoftwareForMonitor(HOST);

        for (const auto& r : regions) {
            if (r->m_activeSpecialWorkspace)
                r->setSpecialWorkspace(nullptr);

            for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
                if (ws && ws->m_monitor == r)
                    State::Workspace::placementController()->moveWorkspaceToMonitor(ws, HOST, true, false, false);
            }

            r->m_activeWorkspace.reset();
        }

        if (SHOW && SHOW->m_monitor == HOST)
            HOST->changeWorkspace(SHOW, false, true, true);
        syncVisibility({HOST});

        if (FOCUS_INSIDE || !Desktop::focusState()->monitor())
            Desktop::focusState()->rawMonitorFocus(HOST);
    }

    removeWorkspaceRules(group);

    for (const auto& r : regions) {
        r->m_virtual.host.reset();
        if (r->m_output)
            r->m_output->destroy();
    }

    group.regions.clear();
    group.merged = forMerge;

    State::monitorLayoutController()->arrange();
    ensurePersistentWorkspaces();

    if (HOST)
        g_pHyprRenderer->damageMonitor(HOST);

    LOG(Log::DEBUG, "virtual monitors: {} {}", forMerge ? "merged" : "dissolved", group.hostName);
    postEvent(group, forMerge ? "merged" : "disabled");
}

void CVirtualMonitorManager::updateInPlace(SVirtualGroup& group) {
    static auto PMINWIDTH = CConfigValue<Config::INTEGER>("virtual_monitors:min_region_width");

    const auto  HOST = group.host.lock();
    if (!HOST)
        return;

    auto spans = partition(sc<int64_t>(HOST->m_pixelSize.x), HOST->m_scale, group.config.ratios, *PMINWIDTH);
    if (!spans) {
        notify(std::format("couldn't resize the regions on {}: {}", HOST->m_name, spans.error()), true);
        return;
    }

    const bool SAME = std::ranges::equal(*spans, group.spans, [](const auto& a, const auto& b) { return a.x == b.x && a.width == b.width; });

    if (group.rules.empty() || !SAME) {
        group.spans = std::move(*spans);
        addWorkspaceRules(group);
    }

    if (SAME && !group.regions.empty() && group.regions.front()->m_scale == HOST->m_scale && group.regions.front()->m_pixelSize.y == HOST->m_pixelSize.y) {
        ensurePersistentWorkspaces();
        return;
    }

    for (size_t i = 0; i < group.regions.size(); ++i) {
        if (const auto R = group.regions[i].lock()) {
            R->m_virtual.pixelOffset = {sc<double>(group.spans[i].x), 0.0};
            R->m_virtual.count       = group.spans.size();
        }
    }

    Config::monitorRuleMgr()->scheduleReload();
    Config::monitorRuleMgr()->ensureMonitorStatus();
    State::monitorLayoutController()->arrange();
    ensurePersistentWorkspaces();

    HOST->m_forceFullFrames = 2;
    HOST->addDamage(CBox{0, 0, INT16_MAX, INT16_MAX});

    postEvent(group, "split");
}

void CVirtualMonitorManager::onHostGone(const PHLMONITOR& host) {
    for (auto& g : m_groups) {
        if (g->host != host)
            continue;

        const bool WAS_RECONCILING = m_reconciling;
        m_reconciling              = true;

        g->hostModeChanged.reset();
        host->m_virtual.regions.clear();
        host->m_virtual.count = 0;
        Pointer::mgr()->unlockSoftwareForMonitor(host);
        removeWorkspaceRules(*g);

        // the regions go too; their workspaces follow the usual path for a lost monitor, and the
        // workspace policy brings them back to the same region names when the output returns
        for (const auto& r : g->regions) {
            const auto R = r.lock();
            if (!R)
                continue;
            R->m_virtual.host.reset();
            if (R->m_output)
                R->m_output->destroy();
        }

        g->regions.clear();
        m_reconciling = WAS_RECONCILING;
        postEvent(*g, "disabled");
    }

    std::erase_if(m_groups, [&host](const auto& g) { return g->host == host; });
}

std::expected<void, std::string> CVirtualMonitorManager::merge(PHLMONITOR monitor) {
    auto* group = groupFor(monitor);
    if (!group)
        return std::unexpected("that monitor isn't split");
    if (group->merged)
        return {};

    dissolve(*group, true);
    return {};
}

std::expected<void, std::string> CVirtualMonitorManager::restore(PHLMONITOR monitor) {
    auto* group = groupFor(monitor);
    if (!group)
        return std::unexpected("that monitor isn't split");
    if (!group->merged)
        return {};

    if (const auto W = group->physicalFullscreen.lock(); W && Fullscreen::controller()->isFullscreen(W))
        Fullscreen::controller()->setFullscreenMode(W, Fullscreen::FSMODE_NONE, std::nullopt);
    group->physicalFullscreen.reset();

    if (auto res = activate(*group); !res) {
        group->merged = true;
        return std::unexpected(res.error());
    }

    return {};
}

std::expected<void, std::string> CVirtualMonitorManager::toggleMerge(PHLMONITOR monitor) {
    auto* group = groupFor(monitor);
    if (!group)
        return std::unexpected("that monitor isn't split");

    return group->merged ? restore(monitor) : merge(monitor);
}

std::expected<void, std::string> CVirtualMonitorManager::togglePhysicalFullscreen(PHLWINDOW window) {
    if (!window)
        return std::unexpected("no window");

    auto* group = groupFor(window->m_monitor.lock());
    if (!group)
        return std::unexpected("the window isn't on a virtual monitor");

    if (group->merged && group->physicalFullscreen == window)
        return restore(group->host.lock());

    if (!group->merged) {
        dissolve(*group, true);
        const auto HOST = group->host.lock();
        if (HOST && window->m_workspace && window->m_workspace->m_monitor == HOST)
            HOST->changeWorkspace(window->m_workspace, false, true, true);
    }

    group->physicalFullscreen = window;
    Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_FULLSCREEN, std::nullopt);
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
    return {};
}

void CVirtualMonitorManager::onWindowFullscreen(const PHLWINDOW& window) {
    static auto PFULLSCREEN = CConfigValue<Config::INTEGER>("virtual_monitors:fullscreen");

    if (!window)
        return;

    const bool FULLSCREEN = Fullscreen::controller()->isFullscreen(window, Fullscreen::FSMODE_FULLSCREEN);

    for (auto& g : m_groups) {
        if (g->physicalFullscreen == window && !FULLSCREEN) {
            g_pEventLoopManager->doLater([this, host = g->host] {
                if (const auto H = host.lock())
                    restore(H);
            });
            return;
        }
    }

    if (*PFULLSCREEN != 1 || !FULLSCREEN)
        return;

    const auto MONITOR = window->m_monitor.lock();
    if (!MONITOR || !MONITOR->isVirtualRegion())
        return;

    g_pEventLoopManager->doLater([this, w = PHLWINDOWREF{window}] {
        const auto W = w.lock();
        if (!W || !Fullscreen::controller()->isFullscreen(W, Fullscreen::FSMODE_FULLSCREEN))
            return;
        const auto M = W->m_monitor.lock();
        if (M && M->isVirtualRegion())
            togglePhysicalFullscreen(W);
    });
}

void CVirtualMonitorManager::onWindowGone(const PHLWINDOW& window) {
    for (auto& g : m_groups) {
        if (g->physicalFullscreen != window)
            continue;

        g->physicalFullscreen.reset();
        g_pEventLoopManager->doLater([this, host = g->host] {
            if (const auto H = host.lock())
                restore(H);
        });
    }
}

SVirtualGroup* CVirtualMonitorManager::groupFor(const PHLMONITOR& monitor) const {
    if (!monitor)
        return nullptr;

    for (const auto& g : m_groups) {
        if (g->host == monitor || std::ranges::any_of(g->regions, [&monitor](const auto& r) { return r == monitor; }))
            return g.get();
    }

    return nullptr;
}

void CVirtualMonitorManager::putBack(SVirtualGroup& group, const std::unordered_map<uint32_t, size_t>& home, const std::vector<std::optional<uint32_t>>& active,
                                     size_t focus, std::optional<Vector2D> pointer) {
    std::vector<PHLMONITOR> regions;
    for (const auto& r : group.regions) {
        const auto R = r.lock();
        if (!R)
            return;
        regions.emplace_back(R);
    }

    for (const auto& ws : State::Workspace::state()->workspacesCopy()) {
        const auto ID   = ws ? ws->numberedID() : std::nullopt;
        const auto HOME = ID ? home.find(*ID) : home.end();
        if (HOME == home.end() || HOME->second >= regions.size())
            continue;

        const auto ON = ws->m_monitor.lock();
        if (std::ranges::find(regions, ON) != regions.end() && ON != regions[HOME->second])
            State::Workspace::placementController()->moveWorkspaceToMonitor(ws, regions[HOME->second], true, false, false);
    }

    for (size_t i = 0; i < regions.size() && i < active.size(); ++i) {
        if (!active[i])
            continue;
        const auto TARGET = State::Workspace::state()->query().numbered(::Workspace::SWorkspaceNumberedID{*active[i]}).run();
        if (TARGET && TARGET->m_monitor == regions[i] && regions[i]->m_activeWorkspace != TARGET)
            regions[i]->changeWorkspace(TARGET, false, true, true);
    }
    syncVisibility(regions);

    const auto FOCUS = regions[std::min(focus, regions.size() - 1)];
    // the pointer goes back where it was if that is still on the focused region, else to its middle
    if (pointer && State::monitorState()->query().vec(*pointer).run() == FOCUS)
        Pointer::pointerController()->warpTo(*pointer, true);
    else
        Pointer::pointerController()->warpTo(FOCUS->middle(), true);
    Desktop::focusState()->rawMonitorFocus(FOCUS);
}

PHLMONITOR CVirtualMonitorManager::hostNamed(const std::string& name) const {
    for (const auto& g : m_groups) {
        if (g->hostName == name)
            return g->host.lock();
    }

    return nullptr;
}

const std::vector<UP<SVirtualGroup>>& CVirtualMonitorManager::groups() const {
    return m_groups;
}

const std::vector<SVirtualMonitorConfig>& CVirtualMonitorManager::configs() const {
    return m_configs;
}

std::optional<Config::CMonitorRule> CVirtualMonitorManager::ruleFor(const PHLMONITOR& monitor) const {
    if (!monitor || !monitor->isVirtualRegion())
        return std::nullopt;

    const auto HOST  = monitor->m_virtual.host.lock();
    const auto INDEX = monitor->m_virtual.index;
    auto*      group = groupFor(HOST);
    if (!HOST || !group || INDEX == 0 || INDEX > group->spans.size())
        return std::nullopt;

    const auto&          SPAN = group->spans[INDEX - 1];

    Config::CMonitorRule rule = HOST->m_activeMonitorRule;
    rule.m_name               = monitor->m_name;
    rule.m_resolution         = {sc<double>(SPAN.width), HOST->m_pixelSize.y};
    rule.m_refreshRate        = HOST->m_refreshRate;
    rule.m_scale              = HOST->m_scale;
    rule.m_offset             = HOST->m_position + Vector2D{sc<double>(SPAN.x) / HOST->m_scale, 0.0};
    rule.m_autoDir            = Config::DIR_AUTO_NONE;
    rule.m_disabled           = false;
    rule.m_mirrorOf           = "";
    rule.m_transform          = WL_OUTPUT_TRANSFORM_NORMAL;
    rule.m_drmMode            = {};
    rule.m_vrr                = 0;
    rule.m_reservedArea       = {};
    return rule;
}

void CVirtualMonitorManager::arrangeRegions(bool xwaylandForceZeroScaling) {
    for (const auto& g : m_groups) {
        const auto HOST = g->host.lock();
        if (!HOST || !HOST->isVirtualHost())
            continue;

        for (const auto& r : g->regions) {
            const auto R = r.lock();
            if (!R)
                continue;

            const auto OFFSET = R->m_virtual.pixelOffset;
            R->moveTo(HOST->m_position + OFFSET / HOST->m_scale);
            R->setXWaylandPosition(HOST->m_xwaylandPosition + (xwaylandForceZeroScaling ? OFFSET : OFFSET / HOST->m_scale));
            R->setXWaylandScale(HOST->m_xwaylandScale);
        }
    }
}

void CVirtualMonitorManager::retargetForWorkspaceAction() {
    static auto PTARGET = CConfigValue<Config::INTEGER>("virtual_monitors:workspace_target");

    if (*PTARGET != 1 || m_groups.empty())
        return;

    const auto FOCUSED = Desktop::focusState()->monitor();
    const auto POINTED = State::monitorState()->query().vec(Pointer::mgr()->position()).run();

    if (!POINTED || POINTED == FOCUSED)
        return;

    // only steer between regions, so ordinary monitors keep their usual behaviour
    if (!POINTED->isVirtualRegion() && !(FOCUSED && FOCUSED->isVirtualRegion()))
        return;

    Desktop::focusState()->rawMonitorFocus(POINTED);
}

uint32_t CVirtualMonitorManager::workspaceIDFor(const PHLMONITOR& monitor, uint32_t number) const {
    if (!monitor || !monitor->isVirtualRegion())
        return number;

    return monitor->m_virtual.workspaceBase + number;
}

bool CVirtualMonitorManager::isWorkspaceOwnedBy(const PHLWORKSPACE& workspace, const PHLMONITOR& region) const {
    if (!workspace || !region || !region->isVirtualRegion())
        return false;

    const auto* GROUP = groupFor(region);
    if (!GROUP)
        return false;

    return regionForWorkspace(*GROUP, workspace) + 1 == region->m_virtual.index;
}

void CVirtualMonitorManager::postEvent(const SVirtualGroup& group, const char* state) {
    if (!IPC::Socket2::sock())
        return;

    IPC::Socket2::sock()->postEvent({"virtualmonitor", std::format("{},{}", group.hostName, state)});
    IPC::Socket2::sock()->postEvent({"virtualmonitorv2", std::format("{},{},{}", group.hostName, state, group.merged ? 0 : group.spans.size())});
}

static std::string escapeJSON(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:
                if (sc<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", sc<int>(c));
                else
                    out += c;
        }
    }
    return out;
}

std::string CVirtualMonitorManager::toJSON() const {
    std::string groups;
    for (const auto& g : m_groups) {
        const auto  HOST = g->host.lock();

        std::string regions;
        for (size_t i = 0; i < g->spans.size(); ++i) {
            const auto R = i < g->regions.size() ? g->regions[i].lock() : nullptr;
            if (!regions.empty())
                regions += ",";
            regions += std::format(R"#({{"index": {}, "name": "{}", "id": {}, "x": {}, "width": {}, "ratio": {:.6f}, "workspaceBase": {}, "activeWorkspace": {}}})#", i + 1,
                                   escapeJSON(regionName(g->hostName, i)), R ? sc<int64_t>(R->m_id) : -1, g->spans[i].x, g->spans[i].width,
                                   g->config.ratios.empty() ? 0.0 : g->config.ratios[i] / std::max(1e-9, std::accumulate(g->config.ratios.begin(), g->config.ratios.end(), 0.0)),
                                   workspaceBase(*g, i), R && R->m_activeWorkspace ? sc<int64_t>(R->m_activeWorkspace->numberedID().value_or(0)) : -1);
        }

        if (!groups.empty())
            groups += ",";
        groups += std::format(
            R"#({{"output": "{}", "host": "{}", "hostId": {}, "make": "{}", "model": "{}", "serial": "{}", "state": "{}", "layout": "{}", "profile": "{}", "workspaces": {}, "regions": [{}]}})#",
            escapeJSON(g->config.output), escapeJSON(g->hostName), HOST ? sc<int64_t>(HOST->m_id) : -1, HOST ? escapeJSON(HOST->m_output->make) : "",
            HOST ? escapeJSON(HOST->m_output->model) : "", HOST ? escapeJSON(HOST->m_output->serial) : "", g->merged ? "merged" : "split", escapeJSON(g->config.layout),
            escapeJSON(g->config.profile), g->config.workspaces, regions);
    }

    std::string configs;
    for (const auto& c : m_configs) {
        std::string ratios;
        for (const auto r : c.ratios) {
            if (!ratios.empty())
                ratios += ",";
            ratios += std::format("{:.6f}", r);
        }

        if (!configs.empty())
            configs += ",";
        configs += std::format(R"#({{"output": "{}", "ratios": [{}], "layout": "{}", "workspaces": {}, "profile": "{}"}})#", escapeJSON(c.output), ratios, escapeJSON(c.layout),
                               c.workspaces, escapeJSON(c.profile));
    }

    const auto PENDING = pendingConfirmSeconds();
    return std::format(R"#({{"available": {}, "killed": {}, "pendingConfirm": {}, "groups": [{}], "configs": [{}]}})#", available() ? "true" : "false", m_killed ? "true" : "false",
                       PENDING ? std::to_string(*PENDING) : "null", groups, configs);
}

std::string CVirtualMonitorManager::toText() const {
    std::string out = std::format("virtual monitors: {}{}\n", available() ? "available" : "unavailable", m_killed ? " (disabled until reload)" : "");
    if (const auto PENDING = pendingConfirmSeconds())
        out += std::format("waiting for confirmation: {}s left\n", *PENDING);

    for (const auto& g : m_groups) {
        out += std::format("\n{} ({}): {}, {} regions\n", g->hostName, g->config.output, g->merged ? "merged" : "split", g->spans.size());
        for (size_t i = 0; i < g->spans.size(); ++i) {
            const auto R = i < g->regions.size() ? g->regions[i].lock() : nullptr;
            out += std::format("\t{}: x {} width {} px, workspaces from {}{}\n", regionName(g->hostName, i), g->spans[i].x, g->spans[i].width, workspaceBase(*g, i) + 1,
                               R && R->m_activeWorkspace ? std::format(", showing {}", R->m_activeWorkspace->addressableName()) : "");
        }
    }

    return out;
}
