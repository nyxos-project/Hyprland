#pragma once

#include "../../helpers/memory/Memory.hpp"
#include "../../desktop/DesktopTypes.hpp"
#include "../../SharedDefs.hpp"
#include "../../macros.hpp"

#include <cstdint>
#include <tuple>
#include <unordered_map>
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

        // Sort key for navigation order: workspaces by ID, each fullscreen workspace right after its origin.
        std::tuple<WORKSPACEID, int, uint64_t> navigationKey(WORKSPACEID id) const;

        // Whether a comes before b in navigation order. For workspaces that aren't fullscreen
        // workspaces, the same as a < b.
        bool navigationBefore(WORKSPACEID a, WORKSPACEID b) const;

        bool isFullscreenWorkspace(WORKSPACEID id) const;

      private:
        struct SSpace {
            WORKSPACEID origin = WORKSPACE_INVALID;
            uint64_t    serial = 0;
        };

        struct SRecord {
            PHLWINDOWREF    window;
            PHLWORKSPACEREF workspace;
            WORKSPACEID     workspaceID = WORKSPACE_INVALID;
            WORKSPACEID     originID    = WORKSPACE_INVALID;
            std::string     originName;
            PHLMONITORREF   monitor;
        };

        void                                     scheduleCheck(PHLWINDOW window);
        void                                     check(PHLWINDOW window);
        void                                     enter(PHLWINDOW window);
        void                                     leave(PHLWINDOW window, SRecord& record);
        void                                     onClose(PHLWINDOW window);
        void                                     onOpen(PHLWINDOW window);
        bool                                     skip(PHLWINDOW window) const;
        PHLWORKSPACE                             originOf(const SRecord& record) const;
        SRecord*                                 recordOf(PHLWINDOW window);

        std::vector<SRecord>                     m_records;
        std::unordered_map<WORKSPACEID, SSpace>  m_spaces;
        std::vector<PHLWINDOWREF>                m_inPlace;
        std::vector<PHLWINDOWREF>                m_pending;
        uint64_t                                 m_serial = 0;
        bool                                     m_busy   = false;
    };

    UP<CFullscreenSpaces>& spaces();
}
