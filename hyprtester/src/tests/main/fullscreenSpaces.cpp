#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include "../shared.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Utils;

// The move to and from a fullscreen workspace happens once the fullscreen change settles.
static void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

static std::string activeName() {
    const auto STR   = getFromSocket("/activeworkspace");
    const auto BEGIN = STR.find('(');
    const auto END   = STR.find(')', BEGIN);
    return BEGIN == std::string::npos || END == std::string::npos ? "" : STR.substr(BEGIN + 1, END - BEGIN - 1);
}

static std::string focusWorkspace(const std::string& ws) {
    const auto RESULT = getFromSocket(std::format("/dispatch hl.dsp.focus({{ workspace = '{}' }})", ws));
    settle();
    return RESULT;
}

static std::string fullscreen(const std::string& action) {
    const auto RESULT = getFromSocket(std::format("/dispatch hl.dsp.window.fullscreen({{ mode = 'fullscreen', action = '{}' }})", action));
    settle();
    return RESULT;
}

TEST_CASE(fullscreenOwnWorkspace) {
    CScopeGuard guard = {[&]() {
        Tests::killAllWindows();
        OK(getFromSocket("/eval hl.plugin.test.alt(0)"));
        OK(getFromSocket("/reload"));
    }};

    OK(getFromSocket("/eval hl.config({ misc = { fullscreen_own_workspace = true } })"));
    OK(getFromSocket("/eval hl.config({ gestures = { workspace_swipe_invert = 0, workspace_swipe_create_new = 0, workspace_swipe_wrap = 0 } })"));

    // Workspaces 2 and 3 each keep a window, so they stay while the fullscreen ones come and go.
    OK(focusWorkspace("3"));
    if (!Tests::spawnKitty("fs_three"))
        FAIL_TEST("Could not spawn kitty");
    OK(focusWorkspace("2"));
    if (!Tests::spawnKitty("fs_two"))
        FAIL_TEST("Could not spawn kitty");
    if (!Tests::spawnKitty("fs_a"))
        FAIL_TEST("Could not spawn kitty");

    NLog::log("{}Fullscreen moves to a workspace of its own", Colors::YELLOW);
    OK(fullscreen("set"));
    const auto FIRST = activeName();
    EXPECT(FIRST.starts_with("fullscreen-2-"), true);
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "class: fs_a");
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 2");

    NLog::log("{}It sits between 2 and 3", Colors::YELLOW);
    OK(focusWorkspace("m+1"));
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 3 (3)");
    OK(focusWorkspace("m-1"));
    EXPECT(activeName(), FIRST);

    NLog::log("{}A second one from 2 goes after the first", Colors::YELLOW);
    OK(focusWorkspace("m-1"));
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "(2)");
    if (!Tests::spawnKitty("fs_b"))
        FAIL_TEST("Could not spawn kitty");
    OK(fullscreen("set"));
    const auto SECOND = activeName();
    EXPECT(SECOND.starts_with("fullscreen-2-"), true);
    EXPECT(SECOND != FIRST, true);
    OK(focusWorkspace("m-1"));
    EXPECT(activeName(), FIRST);
    OK(focusWorkspace("m+1"));
    EXPECT(activeName(), SECOND);
    OK(focusWorkspace("m+1"));
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 3 (3)");

    NLog::log("{}Swipes go the same way", Colors::YELLOW);
    OK(getFromSocket("/eval hl.plugin.test.alt(1)"));
    OK(focusWorkspace("2"));
    OK(getFromSocket("/eval hl.plugin.test.gesture('right', 3)"));
    settle();
    EXPECT(activeName(), FIRST);
    OK(getFromSocket("/eval hl.plugin.test.gesture('right', 3)"));
    settle();
    EXPECT(activeName(), SECOND);
    OK(getFromSocket("/eval hl.plugin.test.gesture('left', 3)"));
    settle();
    EXPECT(activeName(), FIRST);
    OK(getFromSocket("/eval hl.plugin.test.alt(0)"));

    NLog::log("{}Closing one goes back to 2", Colors::YELLOW);
    OK(focusWorkspace(std::format("name:{}", SECOND)));
    OK(getFromSocket("/dispatch hl.dsp.focus({ window = 'class:fs_b' })"));
    OK(getFromSocket("/dispatch hl.dsp.window.kill()"));
    Tests::waitUntilWindowsN(3);
    settle();
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 2 (2)");
    EXPECT(getFromSocket("/workspaces").contains(SECOND), false);

    NLog::log("{}Leaving fullscreen goes back to 2", Colors::YELLOW);
    OK(focusWorkspace(std::format("name:{}", FIRST)));
    OK(getFromSocket("/dispatch hl.dsp.focus({ window = 'class:fs_a' })"));
    OK(fullscreen("unset"));
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 2 (2)");
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "class: fs_a");
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "fullscreen: 0");
    EXPECT(getFromSocket("/workspaces").contains(FIRST), false);

    NLog::log("{}A workspace whose only window went fullscreen comes back", Colors::YELLOW);
    OK(focusWorkspace("4"));
    if (!Tests::spawnKitty("fs_c"))
        FAIL_TEST("Could not spawn kitty");
    OK(fullscreen("set"));
    EXPECT(activeName().starts_with("fullscreen-4-"), true);
    EXPECT(getFromSocket("/workspaces").contains("ID 4 (4)"), false);
    OK(fullscreen("unset"));
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 4 (4)");
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "class: fs_c");
    OK(focusWorkspace("2"));

    NLog::log("{}Maximize stays put", Colors::YELLOW);
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'maximized', action = 'set' })"));
    settle();
    EXPECT_CONTAINS(getFromSocket("/activeworkspace"), "ID 2 (2)");
    OK(getFromSocket("/dispatch hl.dsp.window.fullscreen({ mode = 'maximized', action = 'unset' })"));
    settle();

    NLog::log("{}A special workspace keeps its fullscreen window", Colors::YELLOW);
    OK(focusWorkspace("special:fs_special"));
    if (!Tests::spawnKitty("fs_special"))
        FAIL_TEST("Could not spawn kitty");
    OK(fullscreen("set"));
    EXPECT_CONTAINS(getFromSocket("/activewindow"), "(special:fs_special)");
    EXPECT(getFromSocket("/workspaces").contains("fullscreen-special"), false);
    OK(getFromSocket("/dispatch hl.dsp.workspace.toggle_special('fs_special')"));
}
