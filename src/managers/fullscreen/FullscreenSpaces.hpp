#pragma once

#include "../../helpers/memory/Memory.hpp"
#include "../../desktop/DesktopTypes.hpp"
#include "../../state/workspace/Target.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace Fullscreen {

    /*
        misc:fullscreen_own_workspace: a window that goes truly fullscreen (FSMODE_FULLSCREEN) moves
        to a workspace of its own, which sits right after the workspace it came from in navigation
        order (m+1/m-1, swipes, the slide animation). Leaving fullscreen moves it back; closing it
        returns to the origin and the workspace goes away.
    */
    class CFullscreenSpaces {
      public:
        CFullscreenSpaces();
        ~CFullscreenSpaces() = default;

        bool isFullscreenWorkspace(const PHLWORKSPACE& workspace) const;

        // The workspace a fullscreen workspace came from, if it still exists.
        PHLWORKSPACE originWorkspace(const PHLWORKSPACE& workspace) const;

        // Moves each fullscreen workspace in a navigation-ordered list to right after the workspace it came from.
        void arrange(std::vector<PHLWORKSPACE>& order) const;

        // -1, 0 or 1 for a before, at or after b in navigation order. In navigation order when a fullscreen
        // workspace is involved; otherwise by number for numbered workspaces, as before, and nothing for named ones.
        std::optional<int> compare(const PHLWORKSPACE& a, const State::Workspace::STarget& b) const;

        // Whether going from one workspace to the other slides forward, when a fullscreen workspace is involved.
        std::optional<bool> slidesForward(const PHLWORKSPACE& from, const PHLWORKSPACE& to, const PHLMONITOR& monitor, bool wraparound) const;

      private:
        struct SSpace {
            PHLWORKSPACEREF           workspace;
            State::Workspace::STarget origin;
            uint64_t                  serial = 0;
        };

        struct SRecord {
            PHLWINDOWREF              window;
            PHLWORKSPACEREF           workspace;
            State::Workspace::STarget origin;
            PHLMONITORREF             monitor;
        };

        void                      scheduleCheck(PHLWINDOW window);
        void                      check(PHLWINDOW window);
        void                      enter(PHLWINDOW window);
        void                      leave(PHLWINDOW window, SRecord& record);
        void                      onClose(PHLWINDOW window);
        void                      onOpen(PHLWINDOW window);
        bool                      skip(PHLWINDOW window) const;
        PHLWORKSPACE              originOf(const SRecord& record) const;
        SRecord*                  recordOf(PHLWINDOW window);
        const SSpace*             spaceOf(const PHLWORKSPACE& workspace) const;
        std::vector<PHLWORKSPACE> navigationOrder(const PHLMONITOR& monitor) const;

        std::vector<SRecord>      m_records;
        std::vector<SSpace>       m_spaces;
        std::vector<PHLWINDOWREF> m_inPlace;
        std::vector<PHLWINDOWREF> m_pending;
        uint64_t                  m_serial = 0;
        bool                      m_busy   = false;
    };

    UP<CFullscreenSpaces>& spaces();
}
