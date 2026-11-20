#pragma once
// [lcs-menu] The pause menu's display page as the video settings (src/lcs/lcs_menu.cpp; only the LCS_MENU_VIDEO build
// does anything, the others get empty functions).
#include <cstdint>

class PS2Runtime;

namespace lcs_menu
{
    // one setting of ours on the page: its label (upper case, with the colon, as the game's), its values' names, how
    // to read and set it (main thread)
    struct Row
    {
        const char *label;
        const char *const *names;
        int count;
        int (*get)();
        void (*set)(int index);
        bool bar = false; // drawn as bars like BRIGHTNESS (count steps, left / right without wrapping); names unused
    };

    void install(PS2Runtime &runtime);          // hooks (before the game runs)
    void frame(uint8_t *ram, bool paused);      // game thread, once a game frame: the page written at the first one
    void setRows(const Row *rows, int count);   // main thread, once before the game runs (the array must stay)
    bool betaPolice();                          // BETA POLICE CARS: police car / enforcer blue instead of black
    void setBetaPolice(bool on);
    void hostFrame();                           // main thread, once a host frame: the menu's changes applied
}
