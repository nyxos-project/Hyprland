#pragma once

#include "VirtualRegions.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/memory/Memory.hpp"
#include "../helpers/signal/Signal.hpp"
#include "../helpers/math/Math.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"

#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CEventLoopTimer;

namespace Config {
    class CWorkspaceRule;
}

namespace Monitor {
    // What hl.virtual_monitor asks for one physical output.
    struct SVirtualMonitorConfig {
        std::string         output; // a monitor selector: connector name or desc:
        std::vector<double> ratios; // one entry per region; fewer than two means no split
        std::string         layout     = "equal";
        int                 workspaces = 10; // persistent workspaces on every region but the first; 0 leaves numbering to the config
        std::string         profile;
    };

    // One physical output split into regions.
    struct SVirtualGroup {
        PHLMONITORREF                           host;
        std::string                             hostName;
        SVirtualMonitorConfig                   config;
        std::vector<Virtual::SRegionSpan>       spans;
        std::vector<PHLMONITORREF>              regions;
        std::vector<SP<Config::CWorkspaceRule>> rules;
        size_t                                  firstSlot = 1;
        bool                                    merged    = false;
        PHLWINDOWREF                            physicalFullscreen;
        CHyprSignalListener                     hostModeChanged;

        // what the regions showed when they were merged, so restore can put it back
        std::vector<std::optional<uint32_t>> savedActive;
        size_t                               savedFocus = 0;
        // where the pointer was: the mode changes move it to the middle of the screen
        std::optional<Vector2D>              savedPointer;
        // which region each workspace was on, by its numbered ID (regions' names repeat)
        std::unordered_map<uint32_t, size_t> savedHome;
    };

    class CVirtualMonitorManager {
      public:
        CVirtualMonitorManager();
        ~CVirtualMonitorManager();

        // false unless experimental:virtual_monitors is on and HYPRLAND_NO_VIRTUAL_MONITORS is unset
        bool                                      available() const;

        void                                      beginConfigReload();
        std::expected<void, std::string>          declare(SVirtualMonitorConfig&& config, std::optional<int> confirmSeconds);
        void                                      scheduleReconcile();

        std::expected<void, std::string>          merge(PHLMONITOR monitor);
        std::expected<void, std::string>          restore(PHLMONITOR monitor);
        std::expected<void, std::string>          toggleMerge(PHLMONITOR monitor);
        std::expected<void, std::string>          togglePhysicalFullscreen(PHLWINDOW window);
        std::expected<void, std::string>          confirm();
        void                                      disableAll(const std::string& reason);

        std::optional<Config::CMonitorRule>       ruleFor(const PHLMONITOR& monitor) const;
        void                                      arrangeRegions(bool xwaylandForceZeroScaling);
        void                                      retargetForWorkspaceAction();
        uint32_t                                  workspaceIDFor(const PHLMONITOR& monitor, uint32_t number) const;
        bool                                      isWorkspaceOwnedBy(const PHLWORKSPACE& workspace, const PHLMONITOR& region) const;

        SVirtualGroup*                            groupFor(const PHLMONITOR& monitor) const;
        // a split host by its connector name; hidden hosts are left out of monitor queries
        PHLMONITOR                                hostNamed(const std::string& name) const;
        const std::vector<UP<SVirtualGroup>>&     groups() const;
        const std::vector<SVirtualMonitorConfig>& configs() const;
        std::optional<int>                        pendingConfirmSeconds() const;
        bool                                      killed() const;

        std::string                               toJSON() const;
        std::string                               toText() const;

      private:
        struct SPendingRegion {
            SVirtualGroup* group = nullptr;
            size_t         index = 0;
        };

        struct SPendingConfirm {
            std::string                          output;
            std::optional<SVirtualMonitorConfig> previous;
            SP<CEventLoopTimer>                  timer;
        };

        void                                            reconcile();
        std::expected<void, std::string>                activate(SVirtualGroup& group);
        void                                            dissolve(SVirtualGroup& group, bool forMerge);
        void                                            updateInPlace(SVirtualGroup& group);
        void                                            addWorkspaceRules(SVirtualGroup& group);
        void                                            removeWorkspaceRules(SVirtualGroup& group);
        void                                            onHostGone(const PHLMONITOR& host);
        // after a restore, until the monitor rule reload it set off has run
        CHyprSignalListener m_putBackAfterReload;
        // put remembered workspaces, active workspaces and focus back on a group's regions
        void putBack(SVirtualGroup& group, const std::unordered_map<uint32_t, size_t>& home, const std::vector<std::optional<uint32_t>>& active, size_t focus,
                     std::optional<Vector2D> pointer);
        void                                            onWindowFullscreen(const PHLWINDOW& window);
        void                                            onWindowGone(const PHLWINDOW& window);
        void                                            postEvent(const SVirtualGroup& group, const char* state);
        void                                            cancelPendingConfirm();
        PHLMONITOR                                      findHost(const SVirtualMonitorConfig& config) const;
        size_t                                          regionForWorkspace(const SVirtualGroup& group, const PHLWORKSPACE& workspace) const;
        uint32_t                                        workspaceBase(const SVirtualGroup& group, size_t index) const;

        std::vector<SVirtualMonitorConfig>              m_configs;
        std::vector<UP<SVirtualGroup>>                  m_groups;
        std::unordered_map<std::string, SPendingRegion> m_pendingRegions;
        std::optional<SPendingConfirm>                  m_pendingConfirm;
        bool                                            m_reconcileScheduled = false;
        bool                                            m_killed             = false;
        bool                                            m_reconciling        = false;

        struct {
            CHyprSignalListener newMonitor;
            CHyprSignalListener monitorAdded;
            CHyprSignalListener monitorPreRemoved;
            CHyprSignalListener configReloaded;
            CHyprSignalListener windowFullscreen;
            CHyprSignalListener windowClose;
        } m_listeners;
    };

    UP<CVirtualMonitorManager>& virtualMonitors();
}
