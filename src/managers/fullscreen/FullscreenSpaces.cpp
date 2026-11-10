#include "FullscreenSpaces.hpp"

#include "FullscreenController.hpp"

#include "../../config/ConfigValue.hpp"
#include "../../debug/log/Logger.hpp"
#include "../../desktop/Workspace.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/GlobalWindowController.hpp"
#include "../../desktop/view/Window.hpp"
#include "../../desktop/rule/windowRule/WindowRuleApplicator.hpp"
#include "../../event/EventBus.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/supplementary/DragController.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/WorkspaceState.hpp"

#include <algorithm>
#include <format>

using namespace Fullscreen;

UP<CFullscreenSpaces>& Fullscreen::spaces() {
    static UP<CFullscreenSpaces> p = makeUnique<CFullscreenSpaces>();
    return p;
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
        std::erase_if(m_spaces, [](const auto& e) { return !State::workspaceState()->query().id(e.first).run(); });
    });
}

std::tuple<WORKSPACEID, int, uint64_t> CFullscreenSpaces::navigationKey(WORKSPACEID id) const {
    if (const auto IT = m_spaces.find(id); IT != m_spaces.end())
        return {IT->second.origin, 1, IT->second.serial};
    return {id, 0, 0};
}

bool CFullscreenSpaces::navigationBefore(WORKSPACEID a, WORKSPACEID b) const {
    return navigationKey(a) < navigationKey(b);
}

bool CFullscreenSpaces::isFullscreenWorkspace(WORKSPACEID id) const {
    return m_spaces.contains(id);
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
    if (!window->m_workspace || window->m_workspace->m_isSpecialWorkspace || window->onSpecialWorkspace() || window->m_pinned)
        return true;

    if (isFullscreenWorkspace(window->m_workspace->m_id))
        return true;

    if (std::ranges::find(m_inPlace, window) != m_inPlace.end())
        return true;

    if (window->m_ruleApplicator->m_tagKeeper.isTagged("fullscreen-in-place"))
        return true;

    if (const auto DRAGGED = g_layoutManager->dragController()->target(); DRAGGED && DRAGGED == window->layoutTarget())
        return true;

    return !window->m_workspace->m_monitor;
}

void CFullscreenSpaces::check(PHLWINDOW window) {
    static auto PENABLED = CConfigValue<Config::INTEGER>("misc:fullscreen_own_workspace");

    if (!window->m_isMapped || !window->m_workspace)
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

    const auto ID     = State::workspaceState()->nextAvailableNamedWorkspace();
    const auto SERIAL = ++m_serial;

    // In the order before it exists, so the slide into it goes the right way.
    m_spaces[ID] = {.origin = ORIGIN->m_id, .serial = SERIAL};

    const auto WORKSPACE = State::workspaceState()->create(ID, MONITOR->m_id, std::format("fullscreen-{}-{}", ORIGIN->m_name, SERIAL), false);
    if (!WORKSPACE) {
        m_spaces.erase(ID);
        return;
    }

    Log::logger->log(Log::DEBUG, "fullscreen_own_workspace: {} goes fullscreen on {}, after {}", window->m_title, WORKSPACE->m_name, ORIGIN->m_name);

    m_records.push_back({.window = window, .workspace = WORKSPACE, .workspaceID = ID, .originID = ORIGIN->m_id, .originName = ORIGIN->m_name, .monitor = MONITOR});

    const bool SHOWN = MONITOR->m_activeWorkspace == ORIGIN;

    m_busy = true;
    Desktop::globalWindowController()->moveWindowToWorkspace(window, WORKSPACE);
    window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
    m_busy = false;

    if (SHOWN) {
        MONITOR->changeWorkspace(WORKSPACE, false, true);
        Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_OTHER);
    }
}

PHLWORKSPACE CFullscreenSpaces::originOf(const SRecord& record) const {
    if (const auto ORIGIN = State::workspaceState()->query().id(record.originID).run())
        return ORIGIN;

    // Its only window was the fullscreen one, so it went away; bring it back as it was.
    const auto MONITOR = record.monitor.lock();
    if (!MONITOR)
        return nullptr;

    return State::workspaceState()->create(record.originID, MONITOR->m_id, record.originName, false);
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
    window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
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
    if (!window->m_workspace || !isFullscreenWorkspace(window->m_workspace->m_id))
        return;

    const auto IT = std::ranges::find_if(m_records, [&](const auto& r) { return r.workspaceID == window->m_workspace->m_id; });
    if (IT == m_records.end())
        return;

    // The app's own dialogs stay with it; anything else opens where the app came from.
    const auto OWNER = IT->window.lock();
    if (OWNER && OWNER->getPID() == window->getPID())
        return;

    if (const auto ORIGIN = originOf(*IT)) {
        m_busy = true;
        Desktop::globalWindowController()->moveWindowToWorkspace(window, ORIGIN);
        m_busy = false;
    }
}
