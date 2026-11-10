#include "FullscreenSpaces.hpp"

#include "FullscreenController.hpp"

#include "../../config/ConfigValue.hpp"
#include "../../debug/log/Logger.hpp"
#include "../../desktop/rule/windowRule/WindowRuleApplicator.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/GlobalWindowController.hpp"
#include "../../desktop/view/window/Window.hpp"
#include "../../desktop/view/window/WindowPresentation.hpp"
#include "../../event/EventBus.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/supplementary/DragController.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../output/Monitor.hpp"
#include "../../output/VirtualMonitorManager.hpp"
#include "../../state/workspace/State.hpp"
#include "../../workspace/HLWorkspace.hpp"

#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <format>

using namespace Fullscreen;

UP<CFullscreenSpaces>& Fullscreen::spaces() {
    static UP<CFullscreenSpaces> p = makeUnique<CFullscreenSpaces>();
    return p;
}

static State::Workspace::STarget targetOf(const PHLWORKSPACE& workspace) {
    return {.id = workspace->id(), .address = workspace->addressableName(), .displayName = workspace->displayName(), .type = workspace->type()};
}

static std::optional<uint32_t> numberOf(const State::Workspace::STarget& target) {
    const auto NUMBERED = target.id ? std::get_if<::Workspace::SWorkspaceNumberedID>(&*target.id) : nullptr;
    return NUMBERED ? std::optional<uint32_t>{NUMBERED->value} : std::nullopt;
}

static bool sameTarget(const State::Workspace::STarget& a, const State::Workspace::STarget& b) {
    return a.address == b.address && a.type == b.type;
}

CFullscreenSpaces::CFullscreenSpaces() {
    static auto P1 = Event::bus()->m_events.window.fullscreen.listen([this](PHLWINDOW window) { scheduleCheck(window); });
    static auto P2 = Event::bus()->m_events.window.close.listen([this](PHLWINDOW window) { onClose(window); });
    static auto P3 = Event::bus()->m_events.window.open.listen([this](PHLWINDOW window) { onOpen(window); });
    static auto P4 = Event::bus()->m_events.window.destroy.listen([this](PHLWINDOWREF window) {
        std::erase_if(m_records, [&](const auto& r) { return !r.window || r.window == window; });
        std::erase_if(m_inPlace, [&](const auto& w) { return !w || w == window; });
    });
    static auto P5 = Event::bus()->m_events.workspace.removed.listen([this](PHLWORKSPACEREF) {
        // A fullscreen workspace keeps its place in the order only while it exists.
        std::erase_if(m_spaces, [](const auto& s) { return !s.workspace; });
    });
}

const CFullscreenSpaces::SSpace* CFullscreenSpaces::spaceOf(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return nullptr;
    const auto IT = std::ranges::find_if(m_spaces, [&](const auto& s) { return s.workspace == workspace; });
    return IT == m_spaces.end() ? nullptr : &*IT;
}

bool CFullscreenSpaces::isFullscreenWorkspace(const PHLWORKSPACE& workspace) const {
    return spaceOf(workspace);
}

PHLWORKSPACE CFullscreenSpaces::originWorkspace(const PHLWORKSPACE& workspace) const {
    const auto* SPACE = spaceOf(workspace);
    return SPACE ? State::Workspace::state()->find(SPACE->origin) : nullptr;
}

void CFullscreenSpaces::arrange(std::vector<PHLWORKSPACE>& order) const {
    if (m_spaces.empty())
        return;

    std::vector<PHLWORKSPACE> moved;
    std::erase_if(order, [&](const auto& workspace) {
        if (!spaceOf(workspace))
            return false;
        moved.emplace_back(workspace);
        return true;
    });
    std::ranges::sort(moved, {}, [this](const auto& workspace) { return spaceOf(workspace)->serial; });

    // The number a workspace sorts by: its own, or for a fullscreen one, its origin's.
    const auto sortNumber = [this](const PHLWORKSPACE& workspace) -> std::optional<uint32_t> {
        if (const auto* SPACE = spaceOf(workspace))
            return numberOf(SPACE->origin);
        return workspace->numberedID();
    };

    for (const auto& workspace : moved) {
        const auto* SPACE  = spaceOf(workspace);
        const auto  ORIGIN = State::Workspace::state()->find(SPACE->origin);
        const auto  NUMBER = numberOf(SPACE->origin);
        size_t      at     = order.size();

        if (const auto IT = ORIGIN ? std::ranges::find(order, ORIGIN) : order.end(); IT != order.end()) {
            // After its origin and the fullscreen workspaces already placed after it.
            at = std::distance(order.begin(), IT) + 1;
            while (at < order.size() && spaceOf(order[at]) && sameTarget(spaceOf(order[at])->origin, SPACE->origin))
                ++at;
        } else if (NUMBER) {
            // Its origin went away: where the origin's number would be.
            const auto IT2 = std::ranges::find_if(order, [&](const auto& w) { return sortNumber(w).value_or(0) > *NUMBER; });
            at             = std::distance(order.begin(), IT2);
        } else {
            // A named origin went away: after the named workspaces.
            const auto IT2 = std::ranges::find_if(order, [&](const auto& w) { return sortNumber(w).has_value(); });
            at             = std::distance(order.begin(), IT2);
        }

        order.insert(order.begin() + at, workspace);
    }
}

std::vector<PHLWORKSPACE> CFullscreenSpaces::navigationOrder(const PHLMONITOR& monitor) const {
    std::vector<PHLWORKSPACE> named, numbered;
    for (const auto& weak : State::Workspace::state()->workspaces()) {
        const auto workspace = weak.lock();
        if (workspace->type() == ::Workspace::eWorkspaceType::SPECIAL || (monitor && workspace->m_monitor != monitor))
            continue;
        (workspace->numberedID() ? numbered : named).emplace_back(workspace);
    }
    std::ranges::reverse(named);
    std::ranges::sort(numbered, {}, [](const auto& workspace) { return *workspace->numberedID(); });
    named.insert(named.end(), numbered.begin(), numbered.end());
    arrange(named);
    return named;
}

std::optional<int> CFullscreenSpaces::compare(const PHLWORKSPACE& a, const State::Workspace::STarget& b) const {
    if (!a || !b.valid())
        return std::nullopt;

    const auto B = State::Workspace::state()->find(b);

    if (spaceOf(a) || spaceOf(B)) {
        if (!B)
            return std::nullopt;
        const auto ORDER = navigationOrder(nullptr);
        const auto IA    = std::ranges::find(ORDER, a);
        const auto IB    = std::ranges::find(ORDER, B);
        if (IA == ORDER.end() || IB == ORDER.end())
            return std::nullopt;
        return IA < IB ? -1 : (IA > IB ? 1 : 0);
    }

    const auto A_ID = a->numberedID();
    const auto B_ID = numberOf(b);
    if (!A_ID || !B_ID)
        return std::nullopt;
    return *A_ID < *B_ID ? -1 : (*A_ID > *B_ID ? 1 : 0);
}

std::optional<bool> CFullscreenSpaces::slidesForward(const PHLWORKSPACE& from, const PHLWORKSPACE& to, const PHLMONITOR& monitor, bool wraparound) const {
    if (!from || !to || (!spaceOf(from) && !spaceOf(to)))
        return std::nullopt;

    // a virtual monitor wraps through its own workspaces only
    auto order = navigationOrder(monitor && monitor->isVirtualRegion() ? monitor : nullptr);

    // As for numbered workspaces alone, named ones don't take part.
    std::erase_if(order, [&](const auto& w) {
        const auto* SPACE = spaceOf(w);
        return SPACE ? !numberOf(SPACE->origin) : !w->numberedID();
    });

    const auto IFROM = std::ranges::find(order, from);
    const auto ITO   = std::ranges::find(order, to);
    if (IFROM == order.end() || ITO == order.end())
        return std::nullopt;

    bool       forward = ITO > IFROM;
    const auto LAST    = order.end() - 1;
    if (wraparound && order.size() > 1 && ((IFROM == order.begin() && ITO == LAST) || (ITO == order.begin() && IFROM == LAST)))
        forward = !forward;
    return forward;
}

CFullscreenSpaces::SRecord* CFullscreenSpaces::recordOf(PHLWINDOW window) {
    const auto IT = std::ranges::find_if(m_records, [&](const auto& r) { return r.window == window; });
    return IT == m_records.end() ? nullptr : &*IT;
}

void CFullscreenSpaces::scheduleCheck(PHLWINDOW window) {
    if (m_busy || !window)
        return;

    // Moves, floating changes and group swaps turn fullscreen off and straight back on;
    // look once things have settled.
    if (std::ranges::find(m_pending, window) != m_pending.end())
        return;

    m_pending.emplace_back(window);
    g_pEventLoopManager->doLater([this, weak = PHLWINDOWREF{window}] {
        std::erase_if(m_pending, [&](const auto& w) { return !w || w == weak; });
        if (const auto WINDOW = weak.lock())
            check(WINDOW);
    });
}

bool CFullscreenSpaces::skip(PHLWINDOW window) const {
    const auto WORKSPACE = window->m_workspace;
    if (!WORKSPACE || WORKSPACE->type() == ::Workspace::eWorkspaceType::SPECIAL || window->onSpecialWorkspace() || (window->m_state & Desktop::View::WINDOW_STATE_PINNED))
        return true;

    if (spaceOf(WORKSPACE) || std::ranges::find(m_inPlace, window) != m_inPlace.end())
        return true;

    if (window->m_ruleApplicator->m_tagKeeper.isTagged("fullscreen-in-place"))
        return true;

    if (const auto DRAGGED = g_layoutManager->dragController()->target(); DRAGGED && DRAGGED == window->layoutTarget())
        return true;

    // Virtual monitors handle fullscreen on their regions themselves.
    const auto MONITOR = WORKSPACE->m_monitor.lock();
    if (!MONITOR || MONITOR->isVirtualRegion())
        return true;

    return std::ranges::any_of(Monitor::virtualMonitors()->groups(), [&](const auto& g) { return g->physicalFullscreen == window; });
}

void CFullscreenSpaces::check(PHLWINDOW window) {
    static auto PENABLED = CConfigValue<Config::INTEGER>("misc:fullscreen_own_workspace");

    if (!window->mapped() || !window->m_workspace)
        return;

    const bool FULLSCREEN = Fullscreen::controller()->getFullscreenModes(window).internal == FSMODE_FULLSCREEN;
    auto*      record     = recordOf(window);

    if (!FULLSCREEN) {
        std::erase_if(m_inPlace, [&](const auto& w) { return !w || w == window; });
        if (record) {
            if (window->m_workspace == record->workspace.lock())
                leave(window, *record);
            std::erase_if(m_records, [&](const auto& r) { return r.window == window; });
        }
        return;
    }

    if (record) {
        // Moved off its workspace while fullscreen: it stays where it was put.
        if (window->m_workspace != record->workspace.lock()) {
            m_inPlace.emplace_back(window);
            std::erase_if(m_records, [&](const auto& r) { return r.window == window; });
        }
        return;
    }

    if (!*PENABLED || skip(window))
        return;

    enter(window);
}

void CFullscreenSpaces::enter(PHLWINDOW window) {
    const auto ORIGIN  = window->m_workspace;
    const auto MONITOR = ORIGIN->m_monitor.lock();
    if (!MONITOR)
        return;

    const auto SERIAL    = ++m_serial;
    const auto ADDRESS   = std::format("fullscreen-{}-{}", ORIGIN->addressableName(), SERIAL);
    const auto WORKSPACE = State::Workspace::state()->createNamed(ADDRESS, MONITOR, ADDRESS, false);
    if (!WORKSPACE)
        return;

    Hyprutils::Utils::CScopeGuard ready([&] { WORKSPACE->ready(); });

    LOG(Log::DEBUG, "fullscreen_own_workspace: {} goes fullscreen on {}, after {}", window->metadata().title(), ADDRESS, ORIGIN->addressableName());

    // In the order before anything moves, so the slide into it goes the right way.
    m_spaces.push_back({.workspace = WORKSPACE, .origin = targetOf(ORIGIN), .serial = SERIAL});
    m_records.push_back({.window = window, .workspace = WORKSPACE, .origin = targetOf(ORIGIN), .monitor = MONITOR});

    const bool SHOWN = MONITOR->m_activeWorkspace == ORIGIN;

    m_busy = true;
    Desktop::globalWindowController()->moveWindowToWorkspace(window, WORKSPACE);
    window->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
    m_busy = false;

    if (SHOWN) {
        MONITOR->changeWorkspace(WORKSPACE, false, true);
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);
    }
}

PHLWORKSPACE CFullscreenSpaces::originOf(const SRecord& record) const {
    if (const auto ORIGIN = State::Workspace::state()->find(record.origin))
        return ORIGIN;

    // Its only window was the fullscreen one, so it went away; bring it back as it was.
    const auto MONITOR = record.monitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto ORIGIN = State::Workspace::state()->create(record.origin, MONITOR, false);
    if (ORIGIN)
        ORIGIN->ready();
    return ORIGIN;
}

void CFullscreenSpaces::leave(PHLWINDOW window, SRecord& record) {
    const auto WORKSPACE = record.workspace.lock();
    const auto ORIGIN    = originOf(record);
    if (!ORIGIN || !WORKSPACE)
        return;

    const auto MONITOR = WORKSPACE->m_monitor.lock();
    const bool SHOWN   = MONITOR && MONITOR->m_activeWorkspace == WORKSPACE;

    m_busy = true;
    Desktop::globalWindowController()->moveWindowToWorkspace(window, ORIGIN);
    window->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
    m_busy = false;

    if (SHOWN && ORIGIN->m_monitor == MONITOR) {
        MONITOR->changeWorkspace(ORIGIN, false, true);
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);
    }
}

void CFullscreenSpaces::onClose(PHLWINDOW window) {
    const auto* record = recordOf(window);
    if (!record)
        return;

    const auto RECORD = *record;
    std::erase_if(m_records, [&](const auto& r) { return r.window == window; });

    // Once the window is gone, show where it came from instead of an empty workspace.
    g_pEventLoopManager->doLater([this, RECORD] {
        const auto WORKSPACE = RECORD.workspace.lock();
        const auto MONITOR   = RECORD.monitor.lock();
        if (!WORKSPACE || !MONITOR || MONITOR->m_activeWorkspace != WORKSPACE || WORKSPACE->getWindowCount() > 0)
            return;

        if (const auto ORIGIN = originOf(RECORD); ORIGIN && ORIGIN->m_monitor == MONITOR)
            MONITOR->changeWorkspace(ORIGIN, false, true);
    });
}

void CFullscreenSpaces::onOpen(PHLWINDOW window) {
    if (!window->m_workspace || !spaceOf(window->m_workspace))
        return;

    const auto IT = std::ranges::find_if(m_records, [&](const auto& r) { return r.workspace == window->m_workspace; });
    if (IT == m_records.end())
        return;

    // The app's own dialogs stay with it; anything else opens where the app came from.
    const auto OWNER = IT->window.lock();
    if (OWNER && OWNER->backend().pid() == window->backend().pid())
        return;

    if (const auto ORIGIN = originOf(*IT)) {
        m_busy = true;
        Desktop::globalWindowController()->moveWindowToWorkspace(window, ORIGIN);
        m_busy = false;
    }
}
