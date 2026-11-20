// [lcs-menu] Experimental: the pause menu's "display" page as the video settings (build/menu only, LCS_MENU_VIDEO=1;
// release and diag are built without it: everything below is compiled out there).
//
// LCS's frontend is VC-style, not the reference port's widget pages (2026-11-16, SLUS_214.23 and the generated code):
// - The screen table at 0x393A30, 0x116 bytes a screen: name[8] (GXT key), then 15 entries of 0x12 bytes
//   {u16 action, char key[8] (GXT), u8 0, u8 5, u16 x 240, u16 y, u16 align (2 label : value, 3 centred)}.
//   Screens: 0 FEH_STA, 1 FEH_LOA, 2 FEH_BRI, 3 FEH_CON, 4 FEH_AUD, 5 FEH_DIS (0x393F9E: 13 FED_BRI, 11 FED_SUB,
//   12 FED_WIS, 40 FED_RDR, 41 FED_HUD, 29 FED_POS at y 40..140), 6 FEH_LAN, 7 FEH_MAP. The page shows whatever
//   entries the table holds (a copied seventh row appeared, then an entry of ours: user tests).
// - The menu manager: +0x588 the screen shown, +0x58C the selected entry.
// - 0x33EAE0 (the menu's process) calls 0x340B80 (the button presses, a0 = the manager; its actions' jump table at
//   0x3411EC takes action - 2 < 0x30 only) and 0x339FE0 (the draw). Every jump table on the action is bounded
//   (0x3411EC, 0x33ACC4, 0x342C9C): an action of 60 does nothing in the game.
// - The draw, per entry: label = CText::Get(entry key) (0x33AC80), then the value by action (jump table 0x3CA0B0,
//   action - 5): SUBTITLES (11) takes CText::Get of its ON / OFF key (0x3D8DA8 / 0x3D8D50) and draws label : value.
// - CText::Get 0x332E68 (a0 TheText = *(0x3D89B8), a1 the key).
//
// The page (user's layout, 2026-11-16): BRIGHTNESS and SUBTITLES are the game's entries; the others are ours (Row,
// from main.cpp): drawn as action 11 entries with keys FEV_00.., whose label and value the CText::Get hook gives
// (UTF-16 in guestMalloc'd buffers), and turned into action 60 while the game handles the buttons (so it does not
// switch the subtitles); left / right / Cross / Enter on them step the value. The changes are applied on the main
// thread (hostFrame), as the F2 video page applies them. LCS_MENU_PAGE=0: the game's page as it is.
#include "lcs/lcs_menu.h"
#include "lcs/lcs_diag.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "platform/platform.h"
#include "runtime/gs/gs_hw.h"
#include "render/presenter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>

namespace lcs_input
{
    int mouseSensitivity();
    void setMouseSensitivity(int step);
    bool rawInput();
    void setRawInput(bool raw);
    bool invertMouse();
    void setInvertMouse(bool on);
    int bindingLines(bool vehicle, const char **labels, const char **values, int max);
}

namespace lcs_menu
{
#if LCS_MENU_VIDEO
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kScreens = 0x00393A30u, kScreenSize = 0x116u, kEntrySize = 0x12u, kEntries = 15u;
        constexpr int kScreenCount = 8;
        constexpr uint32_t kDisplay = 5u;
        constexpr uint32_t kDisplayEntries = kScreens + kDisplay * kScreenSize + 8u;
        constexpr uint32_t kManagerScreen = 0x588u, kManagerEntry = 0x58Cu;
        constexpr uint32_t kManagerFocus = 0x590u; // u8: the page's entries have the focus (0: the tabs)
        // the menu manager's fields around the screen / entry / focus, kept when a page of ours is opened (Esc)
        constexpr uint32_t kStateFrom = 0x580u, kStateBytes = 0x40u;
        uint8_t g_enterState[64][kStateBytes] = {};
        bool g_haveEnterState[64] = {};
        std::string hexBytes(const uint8_t *p, uint32_t n)
        {
            std::string out;
            char b[4];
            for (uint32_t i = 0; i < n; ++i)
            {
                std::snprintf(b, sizeof(b), "%02X", p[i]);
                out += b;
                if ((i & 3u) == 3u)
                    out += ' ';
            }
            return out;
        }
        constexpr uint32_t kTextGet = 0x00332E68u, kButtons = 0x00340B80u;
        // the menu's bars (f12 x, f13 y - fixed per action in the draw: BRIGHTNESS 39 -, f17 the filled fraction): a bar
        // row of ours is drawn as a BRIGHTNESS entry, its y and fraction put in here
        constexpr uint32_t kDrawBars = 0x003423A0u;
        constexpr uint16_t kBarAction = 13u;
        constexpr uint32_t kOnKey = 0x003D8DA8u, kOffKey = 0x003D8D50u; // SUBTITLES' value keys in the draw
        constexpr uint16_t kValueAction = 11u, kInertAction = 60u, kAlignValue = 2u, kAlignCentre = 3u;
        constexpr uint16_t kFirstY = 40u, kStepY = 16u; // BRIGHTNESS's bars stay at the game's y (39)
        constexpr int kMaxRows = 48;
        constexpr int kTableScreens = 64; // the screen table's length (0x393A30 .. 0x3980C6; FEH_MP 14..27 unused on the PS2)

        // [lcs-menu] CONTROLS split in two (user's layout 2026-11-16): screen 3 holds CONTROLLER SETUP and MOUSE AND
        // KEYBOARD SETUP, each opening a screen of its own (action 3 opens the entry's target screen, byte +11, as the
        // game's NO entries do); the controller picture is not drawn (the draw calls 0x33E190 and 0x3460A0 for screen 3,
        // returning to 0x33CAD8 / 0x33CAE8). Screens of the multiplayer menus nothing opens on the PS2:
        constexpr uint32_t kControls = 3u, kScrController = 15u, kScrMouse = 16u, kScrChange = 18u, kScrFoot = 19u, kScrVehicle = 20u;
        // [lcs-menu] DISPLAY split in two (the user's ask 2026-11-18: too little room on one page): DISPLAY OPTIONS (the
        // picture: resolution, widescreen, brightness, VSync, frame rate) and VIDEO OPTIONS (quality: MSAA, texture
        // filtering, draw distance, trails, beta police cars), each with RESTORE DEFAULTS - two more unused screens
        constexpr uint32_t kScrDisplayOpts = 21u, kScrVideoOpts = 22u;
        uint32_t g_brightnessScreen = 5u; // where the game's BRIGHTNESS entry is (its bars follow it)
        bool g_displaySplit = false;
        constexpr uint16_t kOpenAction = 3u;
        constexpr uint32_t kCtrlPicA = 0x0033E190u, kCtrlPicB = 0x003460A0u, kCtrlPicRetA = 0x0033CAD8u, kCtrlPicRetB = 0x0033CAE8u;
        // the pages of ours and the page each goes back to (Esc: lcs_input -> requestBack)
        std::atomic<int> g_screenNow{-1};
        std::atomic<bool> g_backWanted{false};
        int parentOf(uint32_t screen)
        {
            switch (screen)
            {
            case kScrController: case kScrMouse: return static_cast<int>(kControls);
            case kScrChange: return static_cast<int>(kScrMouse);
            case kScrFoot: case kScrVehicle: return static_cast<int>(kScrChange);
            case kScrDisplayOpts: case kScrVideoOpts: return g_displaySplit ? 5 : -1;
            default: return -1;
            }
        }

        // [lcs-menu] EXIT GAME on the game page (screen 1: LOAD GAME at x 240 y 70, NEW GAME and DELETE GAME at x / y 0 -
        // stacked under it by the game; the byte at +11 is the screen an entry opens: 58, 9, 59). Ours is the fourth,
        // inert (action 60), centred, x / y 0; Cross arms it ("EXIT GAME? PRESS AGAIN"), a second Cross within 3 s quits
        // as the window's close does (platform::requestQuit on the main thread). LCS_MENU_EXIT=0: not added.
        constexpr uint32_t kGame = 1u, kExitSlot = 3u;
        constexpr uint32_t kGameEntries = kScreens + kGame * kScreenSize + 8u;
        constexpr uint32_t kExitKey = kGameEntries + kExitSlot * kEntrySize + 2u;
        bool g_exitBuilt = false;
        uint32_t g_exitText = 0; // guest: the label (kTextChars UTF-16 characters)
        std::chrono::steady_clock::time_point g_exitArmed{};
        std::atomic<bool> g_quitWanted{false};
        bool exitArmed() { return std::chrono::steady_clock::now() - g_exitArmed < std::chrono::seconds(3); }
        constexpr uint32_t kTextChars = 32u; // a buffer's UTF-16 characters, the terminator included

        // the page: the game's entries by their index in the original display page, ours (-1) in the order given
        // (2026-11-17, the user's order: picture and frame rate, then quality, then extras; SUBTITLES moved to the audio
        // page, half a row's gap before RESTORE DEFAULTS)
        constexpr int kGameBrightness = 0, kGameSubtitles = 1, kOurs = -1, kGap = -2;
        constexpr int kLayout[] = {kOurs, kOurs, kOurs, kOurs, kGameBrightness, kOurs, kOurs, kOurs, kOurs, kOurs, kGap, kOurs};
        constexpr uint32_t kAudio = 4u;
        constexpr uint16_t kAudioSubtitlesY = 120u; // below FEA_RSS (100), the audio page's 20 a row
        uint16_t g_brightnessY = 40u; // the game's BRIGHTNESS entry's y on the page (its bars are drawn at a fixed 39)
        std::atomic<int> g_menuScreen{-1}; // the menu's screen now (the buttons' pass)
        uint32_t g_tabText[8] = {};        // the text CText::Get gave for screen n's name (the tabs, see print)

        PS2Runtime::RecompiledFunction g_origTextGet = nullptr, g_origButtons = nullptr, g_origDrawBars = nullptr;
        PS2Runtime::RecompiledFunction g_origCtrlPicA = nullptr, g_origCtrlPicB = nullptr;

        Row g_all[kMaxRows];          // main.cpp's rows (the display page), then ours (the controls pages)
        const Row *g_rows = g_all;
        int g_rowCount = 0, g_mainRows = 0;
        int g_restoreFrom = 0, g_restoreTo = 0; // the rows RESTORE DEFAULTS resets (its own page's)
        int g_defaults[kMaxRows] = {};
        std::atomic<int> g_value[kMaxRows];   // main thread -> game thread: each row's value now
        std::atomic<int> g_pending[kMaxRows]; // game thread -> main thread: the value to set (-1 none)
        std::atomic<bool> g_restore{false};

        bool g_pageBuilt = false;          // game thread
        int g_slotRow[kTableScreens][kEntries]; // screen, slot -> our row, -1 the game's, a link or empty
        uint32_t g_textPool = 0;           // guest buffers: two (label, value) a row
        int g_valueFor = -1;               // the row whose label was just fetched: its value comes next
        int g_drawing = -1;                // the row of ours being drawn (its label fetched last), -1 the game's
        uint16_t g_rowY[kMaxRows] = {};
        bool g_controlsBuilt = false;
        struct Fixed
        {
            char key[8];
            const char *text;
            uint32_t at; // guest buffer
        };
        Fixed g_fixed[] = {
            {{'F', 'E', 'V', '_', 'L', '0', 0, 0}, "CONTROLLER SETUP", 0},
            {{'F', 'E', 'V', '_', 'L', '1', 0, 0}, "MOUSE AND KEYBOARD SETUP", 0},
            {{'F', 'E', 'V', '_', 'L', '2', 0, 0}, "CHANGE CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'L', '3', 0, 0}, "FOOT CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'L', '4', 0, 0}, "VEHICLE CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'T', '0', 0, 0}, "CONTROLLER SETUP", 0},
            {{'F', 'E', 'V', '_', 'T', '1', 0, 0}, "MOUSE AND KEYBOARD", 0},
            {{'F', 'E', 'V', '_', 'T', '2', 0, 0}, "CHANGE CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'T', '3', 0, 0}, "FOOT CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'T', '4', 0, 0}, "VEHICLE CONTROLS", 0},
            {{'F', 'E', 'V', '_', 'L', '5', 0, 0}, "DISPLAY OPTIONS", 0},
            {{'F', 'E', 'V', '_', 'L', '6', 0, 0}, "VIDEO OPTIONS", 0},
            {{'F', 'E', 'V', '_', 'T', '5', 0, 0}, "DISPLAY OPTIONS", 0},
            {{'F', 'E', 'V', '_', 'T', '6', 0, 0}, "VIDEO OPTIONS", 0},
        };
        constexpr int kFixedDisplayLink = 10, kFixedVideoLink = 11, kFixedDisplayTitle = 12, kFixedVideoTitle = 13;
        uint32_t fixedText(const uint8_t *ram, uint32_t key)
        {
            if (!g_controlsBuilt || key + 8u > PS2_RAM_SIZE)
                return 0;
            for (const Fixed &f : g_fixed)
                if (f.at && std::memcmp(ram + key, f.key, 8) == 0)
                    return f.at;
            return 0;
        }

        uint32_t rowText(uint8_t *ram, int row, bool value, const char *text)
        {
            const uint32_t buffer = g_textPool + (static_cast<uint32_t>(row) * 2u + (value ? 1u : 0u)) * kTextChars * 2u;
            uint32_t i = 0;
            for (; text[i] && i + 1u < kTextChars; ++i)
            {
                const uint16_t c = static_cast<uint8_t>(text[i]);
                std::memcpy(ram + buffer + i * 2u, &c, 2);
            }
            const uint16_t zero = 0;
            std::memcpy(ram + buffer + i * 2u, &zero, 2);
            return buffer;
        }

        // our key "FEV_nn" -> the row, or -1
        int keyRow(const uint8_t *ram, uint32_t key)
        {
            if (key < kScreens || key >= kScreens + kTableScreens * kScreenSize)
                return -1;
            const uint8_t *k = ram + key;
            if (std::memcmp(k, "FEV_", 4) != 0 || k[4] < '0' || k[4] > '9' || k[5] < '0' || k[5] > '9')
                return -1;
            const int row = (k[4] - '0') * 10 + (k[5] - '0');
            return row < g_rowCount ? row : -1;
        }

        // [lcs-menu] The PS2's memory card texts (TEXT/ENGLISH.GXT, the main table) in PC words: the saves are a folder
        // (saves\mc0) here, no card, slot or console to mention. Every key's first use is logged (which one the boot's
        // loading screen shows). LCS_PC_TEXTS=0: the game's texts.
        struct Replacement
        {
            const char *key, *text;
        };
        constexpr Replacement kReplacements[] = {
            {"MCDNCK", "Checking save data. Please do not close the game."},
            {"AUTOLD", "Loading. Please do not close the game."},
            {"FELD_WR", "Loading data. Please do not close the game."},
            {"FESZ_WR", "Saving data. Please do not close the game."},
            {"FEDL_WR", "Deleting data. Please do not close the game."},
            {"FES_CFM", "Creating save data. Please do not close the game."},
            {"FEM_NTH", "No save data available."},
            {"FES_NOC", "No save data available."},
            {"MCDNSP", "No save data available. Your progress cannot be saved. Do you wish to start the game anyway?"},
            {"FEM_NTF", "The save data is not set up."},
            {"FESZ_FO", "The save data is not set up. Do you wish to set it up now?"},
            {"SLONNF", "The save data is not set up. Do you wish to set it up now?"},
            {"FESZ_FF", "Setting up the save data failed! Please try again."},
            {"SLONFM", "Setting up the save data failed! Please try again."},
            {"MCGNDP", "There is not enough free space to save this game. Do you wish to start the game anyway?"},
            {"MCGNSP", "There is not enough free space to save this game. Do you wish to start the game anyway?"},
            {"SLNSP", "There is not enough free space to save this game."},
            {"SLONDR", "There is not enough free space to save this game."},
            {"FES_CMP", "Save failed! Please try again."},
            {"FES_DEE", "Delete failed! Please try again."},
            {"FES_LCF", "Load failed! Please try again."},
            {"FES_LOE", "Load failed! Please try again. Now restarting game."},
            {"FES_LOQ", "Load failed! This save game is not compatible with the game. Please try loading a different save game. "
                        "Now restarting game."},
        };
        constexpr uint32_t kReplacementChars = 160u; // a buffer's UTF-16 characters, the terminator included
        constexpr size_t kReplacementCount = sizeof(kReplacements) / sizeof(kReplacements[0]);
        const bool g_pcTexts = !(std::getenv("LCS_PC_TEXTS") && std::getenv("LCS_PC_TEXTS")[0] == '0');
        uint32_t g_replacementPool = 0; // guest: the texts, written once
        bool g_replacementUsed[kReplacementCount] = {};

        // the key's replacement in guest memory (0: none)
        uint32_t replacement(uint8_t *ram, PS2Runtime *runtime, uint32_t key)
        {
            if (!g_pcTexts || key + 8u > PS2_RAM_SIZE)
                return 0;
            char k[9] = {};
            std::memcpy(k, ram + key, 8);
            for (size_t i = 0; i < kReplacementCount; ++i)
            {
                if (std::strcmp(k, kReplacements[i].key) != 0)
                    continue;
                if (!g_replacementPool)
                {
                    g_replacementPool = runtime->guestMalloc(static_cast<uint32_t>(kReplacementCount) * kReplacementChars * 2u, 16u);
                    if (!g_replacementPool)
                        return 0;
                    for (size_t r = 0; r < kReplacementCount; ++r)
                    {
                        const uint32_t buffer = g_replacementPool + static_cast<uint32_t>(r) * kReplacementChars * 2u;
                        const char *t = kReplacements[r].text;
                        uint32_t c = 0;
                        for (; t[c] && c + 1u < kReplacementChars; ++c)
                        {
                            const uint16_t ch = static_cast<uint8_t>(t[c]);
                            std::memcpy(ram + buffer + c * 2u, &ch, 2);
                        }
                        const uint16_t zero = 0;
                        std::memcpy(ram + buffer + c * 2u, &zero, 2);
                    }
                }
                if (!g_replacementUsed[i])
                {
                    g_replacementUsed[i] = true;
                    std::fprintf(stderr, "[lcs-text] %s shown as PC text: \"%s\"\n", kReplacements[i].key, kReplacements[i].text);
                }
                return g_replacementPool + static_cast<uint32_t>(i) * kReplacementChars * 2u;
            }
            return 0;
        }

        PS2_GUEST_CC void textGet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kTextGet)
                return g_origTextGet(rdram, ctx, runtime);
            const uint32_t key = GPR_U32(ctx, 5) & kRamMask;
            if (const uint32_t pc = replacement(rdram, runtime, key))
            {
                SET_GPR_U32(ctx, 2, pc);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (g_exitBuilt && key == kExitKey)
            {
                const char *label = exitArmed() ? "EXIT GAME? PRESS AGAIN" : "EXIT GAME";
                uint32_t i = 0;
                for (; label[i] && i + 1u < kTextChars; ++i)
                {
                    const uint16_t c = static_cast<uint8_t>(label[i]);
                    std::memcpy(rdram + g_exitText + i * 2u, &c, 2);
                }
                const uint16_t zero = 0;
                std::memcpy(rdram + g_exitText + i * 2u, &zero, 2);
                SET_GPR_U32(ctx, 2, g_exitText);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            { // diagnosis for the key prompts (2026-11-18): the C<configuration><ACTION> lookups ~k~ makes, with who asks
                static int told = 0;
                static const bool promptLog = lcs_diag::wanted("LCS_PROMPT_LOG"); // release: only with LCS_PROMPT_LOG=1
                const uint8_t *k = rdram + key;
                if (promptLog && told < 80 && key + 8u < PS2_RAM_SIZE && k[0] == 'C' && k[1] >= '0' && k[1] <= '3' && k[2] >= 'A' && k[2] <= 'Z')
                {
                    char name[9] = {};
                    std::memcpy(name, k, 8);
                    ++told;
                    g_origTextGet(rdram, ctx, runtime);
                    const uint32_t text = GPR_U32(ctx, 2) & kRamMask;
                    std::string s;
                    for (int i = 0; i < 24 && text; ++i)
                    {
                        uint16_t c;
                        std::memcpy(&c, rdram + text + i * 2u, 2);
                        if (!c)
                            break;
                        char b[8];
                        std::snprintf(b, sizeof(b), c >= 32u && c < 127u ? "%c" : "<%X>", c);
                        s += b;
                    }
                    std::fprintf(stderr, "[lcs-prompt] %s = \"%s\" (text 0x%06X, ra 0x%06X)\n", name, s.c_str(), text, GPR_U32(ctx, 31));
                    return;
                }
            }
            // a tab's name (the screen table's name of screens 0 - 7): remembered for the tabs' places (print)
            if (key >= kScreens && key < kScreens + 8u * kScreenSize && (key - kScreens) % kScreenSize == 0u)
            {
                g_valueFor = g_drawing = -1;
                g_origTextGet(rdram, ctx, runtime);
                g_tabText[(key - kScreens) / kScreenSize] = GPR_U32(ctx, 2) & kRamMask;
                return;
            }
            if (const uint32_t fixed = fixedText(rdram, key))
            {
                g_valueFor = g_drawing = -1;
                SET_GPR_U32(ctx, 2, fixed);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (!g_pageBuilt)
                return g_origTextGet(rdram, ctx, runtime);
            const int row = keyRow(rdram, key);
            uint32_t text = 0;
            if (row >= 0)
            {
                g_drawing = row;
                g_valueFor = g_rows[row].count > 0 && !g_rows[row].bar ? row : -1;
                text = rowText(rdram, row, false, g_rows[row].label);
            }
            else if (g_valueFor >= 0 && (key == kOnKey || key == kOffKey))
            {
                const Row &r = g_rows[g_valueFor];
                int v = g_value[g_valueFor].load(std::memory_order_relaxed);
                v = v < 0 ? 0 : v >= r.count ? r.count - 1 : v;
                text = rowText(rdram, g_valueFor, true, r.names[v]);
                g_valueFor = -1;
            }
            else if (key >= kScreens && key < kScreens + kTableScreens * kScreenSize)
                g_valueFor = g_drawing = -1; // another entry's label
            if (!text)
                return g_origTextGet(rdram, ctx, runtime);
            SET_GPR_U32(ctx, 2, text);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // [lcs-menu] BETA POLICE CARS (user's ask 2026-11-17): CVehicleModelInfo::ChooseVehicleColour (0x2B9338: a0 the
        // model info, a1 / a2 the colours' bytes; +0x170 / +0x178 its pairs, +0x180 their count, +0x181 the last one;
        // the vehicle keeps them at +0x1F0 / +0x1F1) gives the police car and the enforcer 0 / 1, black and white
        // (GAME.DTZ, both). With the switch on their black is the palette's 2, "police car blue" (42, 119, 161; VC's
        // index too - the palette has 101 colours). The models by their name key at model info +8 (CKeyGen of
        // "POLICE" / "ENFORCER"). Cars made from then on; the ones on the street keep theirs.
        constexpr uint32_t kChooseColour = 0x002B9338u;
        constexpr uint32_t kKeyPolice = 0xEB7B6120u, kKeyEnforcer = 0xF785BBFAu;
        constexpr uint8_t kBlack = 0u, kPoliceBlue = 2u;
        std::atomic<bool> g_betaPolice{false};
        PS2Runtime::RecompiledFunction g_origChooseColour = nullptr;
        PS2_GUEST_CC void chooseColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kChooseColour)
                return g_origChooseColour(rdram, ctx, runtime);
            const uint32_t info = GPR_U32(ctx, 4) & kRamMask, c1 = GPR_U32(ctx, 5) & kRamMask, c2 = GPR_U32(ctx, 6) & kRamMask;
            g_origChooseColour(rdram, ctx, runtime);
            if (!g_betaPolice.load(std::memory_order_relaxed))
                return;
            uint32_t key;
            std::memcpy(&key, rdram + info + 8u, 4);
            if (key != kKeyPolice && key != kKeyEnforcer)
                return;
            for (uint32_t at : {c1, c2})
                if (rdram[at] == kBlack)
                    rdram[at] = kPoliceBlue;
            static uint32_t told = 0;
            if (told++ < 4u)
                std::fprintf(stderr, "[lcs-menu] beta %s: colours %u / %u\n", key == kKeyPolice ? "police car" : "enforcer",
                             rdram[c1], rdram[c2]);
        }

        // CVehicleModelInfo::SetVehicleColour (0x2B9248: a0 the model info, a1 / a2 the colours): the palette's RGB
        // (*(gp + 0x498) + 4 + index * 4) written into the paintable materials' colours, listed at model info +0xA8
        // (primary) and +0x10C (secondary), null-ended. The police car stayed black with 2 / 1 chosen (user's test
        // 13:35): its lists are logged here once a model to see whether the black is paintable at all.
        constexpr uint32_t kSetColour = 0x002B9248u;

        // [lcs-menu] Diagnosis for the beta police cars (2026-11-17): the police model has no paintable materials (its
        // lists empty in the model file, SetClump 0x2B96B8 copies them). To find its black ones, the material's layout
        // is learnt from a car that has some: its first primary material (colour at +4, as SetVehicleColour writes it),
        // the texture it points to, and where else near its clump (model info +0xC) that pointer is kept (the
        // geometry's material array). For the police car: what near its clump looks like a material (a pointer to
        // something with a pointer first and an opaque colour at +4). Logged only - nothing written.
        uint32_t rdw(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        std::string hexAt(const uint8_t *ram, uint32_t a, uint32_t n)
        {
            std::string out;
            char b[4];
            for (uint32_t i = 0; i < n; ++i)
            {
                std::snprintf(b, sizeof(b), "%02X", ram[(a + i) & kRamMask]);
                out += b;
                if ((i & 3u) == 3u)
                    out += ' ';
            }
            return out;
        }
        std::string textAt(const uint8_t *ram, uint32_t a, uint32_t n)
        {
            std::string out;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint8_t c = ram[(a + i) & kRamMask];
                out += c >= 32 && c < 127 ? static_cast<char>(c) : '.';
            }
            return out;
        }
        void materialProbe(const uint8_t *ram, uint32_t info, bool police)
        {
            // [lcs] the diag build only: it scans 384 KB and logs on the game thread (2026-11-18: 169,000 lines in one
            // release session, a hitch at every police car's colour)
            if constexpr (!lcs_diag::kEnabled)
                return;
            static int normals = 0;
            static uint32_t probedPolice[2] = {0, 0};
            const uint32_t clump = rdw(ram, info + 0xCu) & kRamMask;
            if (!clump || clump < 0x100000u || clump >= (PS2_RAM_SIZE - 0x10000u))
                return;
            const uint32_t lo = clump > 0x30000u ? clump - 0x30000u : 0u, hi = std::min(clump + 0x30000u, (PS2_RAM_MASK & ~0xFu));
            if (!police)
            {
                const uint32_t m = rdw(ram, info + 0xA8u) & kRamMask;
                if (!m || normals >= 2)
                    return;
                ++normals;
                const uint32_t tex = rdw(ram, m) & kRamMask;
                std::fprintf(stderr, "[lcs-probe] car info 0x%06X clump 0x%06X: material 0x%06X = %s| texture 0x%06X = %s| %s\n", info,
                             clump, m, hexAt(ram, m - 0x10u, 0x30u).c_str(), tex, hexAt(ram, tex, 0x30u).c_str(),
                             textAt(ram, tex, 0x40u).c_str());
                int found = 0;
                for (uint32_t at = lo; at < hi && found < 6; at += 4u)
                    if ((rdw(ram, at) & kRamMask) == m && (at < info || at >= info + 0x200u))
                    {
                        ++found;
                        std::fprintf(stderr, "[lcs-probe]   kept at 0x%06X (clump %+d): %s\n", at,
                                     static_cast<int>(at) - static_cast<int>(clump), hexAt(ram, at - 0x10u, 0x30u).c_str());
                    }
                return;
            }
            bool fresh = false;
            for (uint32_t &done : probedPolice)
            {
                if (done == clump)
                    return;
                if (done == 0u)
                {
                    done = clump;
                    fresh = true;
                    break;
                }
            }
            if (!fresh) // both slots taken: probed enough (before, every later police car was probed and logged again)
                return;
            std::fprintf(stderr, "[lcs-probe] police info 0x%06X clump 0x%06X: %s\n", info, clump, hexAt(ram, clump, 0x40u).c_str());
            int n = 0;
            uint32_t seen[64];
            for (uint32_t at = lo; at < hi && n < 48; at += 4u)
            {
                const uint32_t m = rdw(ram, at) & kRamMask;
                if (m < lo || m >= hi || (m & 3u))
                    continue;
                const uint32_t tex = rdw(ram, m) & kRamMask;
                if (ram[m + 7u] != 0xFFu || tex < 0x100000u || tex >= (PS2_RAM_SIZE - 0x10000u))
                    continue;
                bool dup = false;
                for (int i = 0; i < n; ++i)
                    dup = dup || seen[i] == m;
                if (dup)
                    continue;
                seen[n++] = m;
                std::fprintf(stderr, "[lcs-probe]   0x%06X at 0x%06X: rgb %u,%u,%u | %s| tex %s\n", m, at, ram[m + 4u], ram[m + 5u],
                             ram[m + 6u], hexAt(ram, m, 0x20u).c_str(), textAt(ram, tex, 0x40u).c_str());
            }
        }
        PS2Runtime::RecompiledFunction g_origSetColour = nullptr;
        PS2_GUEST_CC void setColour(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kSetColour)
                return g_origSetColour(rdram, ctx, runtime);
            const uint32_t info = GPR_U32(ctx, 4) & kRamMask;
            const uint32_t c1 = GPR_U32(ctx, 5) & 0xFFu, c2 = GPR_U32(ctx, 6) & 0xFFu;
            uint32_t key = 0;
            if (info)
                std::memcpy(&key, rdram + info + 8u, 4);
            g_origSetColour(rdram, ctx, runtime);
            if (info)
                materialProbe(rdram, info, key == kKeyPolice || key == kKeyEnforcer);
            static int toldPolice = 0, toldEnforcer = 0;
            int *told = key == kKeyPolice ? &toldPolice : key == kKeyEnforcer ? &toldEnforcer : nullptr;
            if (!told || *told >= 2)
                return;
            ++*told;
            std::string line;
            for (uint32_t list : {0xA8u, 0x10Cu})
            {
                int n = 0;
                char buf[64];
                line += list == 0xA8u ? " primary [" : "] secondary [";
                for (uint32_t i = 0; i < 25u; ++i)
                {
                    uint32_t m;
                    std::memcpy(&m, rdram + info + list + i * 4u, 4);
                    if (!m)
                        break;
                    m &= kRamMask;
                    std::snprintf(buf, sizeof(buf), "%s%u,%u,%u", n ? " " : "", rdram[m + 4], rdram[m + 5], rdram[m + 6]);
                    line += buf;
                    ++n;
                }
            }
            std::fprintf(stderr, "[lcs-menu] %s colours %u / %u: materials%s]\n", key == kKeyPolice ? "police car" : "enforcer", c1,
                         c2, line.c_str());
        }

        PS2_GUEST_CC void drawBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kDrawBars && g_pageBuilt && g_drawing >= 0 && g_rows[g_drawing].bar)
            {
                const Row &r = g_rows[g_drawing];
                int v = g_value[g_drawing].load(std::memory_order_relaxed);
                v = v < 0 ? 0 : v >= r.count ? r.count - 1 : v;
                ctx->f[13] = static_cast<float>(g_rowY[g_drawing]) - 1.0f;
                ctx->f[17] = r.count > 1 ? static_cast<float>(v) / static_cast<float>(r.count - 1) : 0.0f;
            }
            else if (ctx->pc == kDrawBars && g_pageBuilt && g_drawing < 0 &&
                     g_menuScreen.load(std::memory_order_relaxed) == static_cast<int>(g_brightnessScreen))
                ctx->f[13] = static_cast<float>(g_brightnessY) - 1.0f; // the game's BRIGHTNESS, moved down the page
            g_origDrawBars(rdram, ctx, runtime);
        }

        // a button's press, edge-detected here (the menu's handler may run more than once a frame)
        struct Edge
        {
            bool was = false;
            bool pressed(bool down)
            {
                const bool p = down && !was;
                was = down;
                return p;
            }
        };
        Edge g_left, g_right, g_select;

        bool pad(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        // [lcs-menu] The mouse in the menu (the user's ask 2026-11-18: entries clickable). The menu is drawn 4:3 in the
        // middle of the window (present::fitRect, displayAspect(false)) in the frontend's 480 x 272 units (x 240 is the
        // middle; an entry's y is its row's top). The entry under the cursor is selected (manager +0x58C) when the mouse
        // moves; a left click on it is Cross (lcs_input sends it while mouseOverEntry), on a setting of ours the next
        // value; the right button goes back as Esc does. Entries the game stacks itself (x / y 0: the game page's LOAD,
        // NEW, DELETE, EXIT) are not hit by the mouse. LCS_MENU_MOUSE_LOG=1: each click's position and entry.
        constexpr float kMenuW = 480.0f, kMenuH = 272.0f, kRowTop = 0.0f, kRowHeight = 14.0f;
        std::atomic<bool> g_mouseOver{false};
        Edge g_click;

        // [lcs-menu] The tabs (MAP BRIEF GAME STATS / CONTROLS AUDIO DISPLAY): manager +0 is the tab (1..7), the
        // buttons turn it into the screen through the table at 0x3CA340 (MAP 7, BRIEF 2, GAME 1, STATS 0, CONTROLS 3,
        // AUDIO 4, DISPLAY 5). Where the draw puts them is learnt as it draws: a screen's name fetched by CText::Get
        // (its key is the screen table's name) and then printed by CFont::PrintString 0x1658F8 (f12 x, f13 y, a0 the
        // text) is that tab - the page's own title, printed at y 10, is not one. A click on a tab sets the tab and its
        // screen (the entry back to the first). Logged once a tab.
        constexpr uint32_t kPrint = 0x001658F8u;
        constexpr int kTabScreen[7] = {7, 2, 1, 0, 3, 4, 5};
        // (found by the log 17:38: the tabs are printed by the routine returning to 0x345FD0, at the menu's foot - y 232
        // MAP BRIEF GAME STATS, y 249 CONTROLS AUDIO DISPLAY - in that order, left-aligned, every frame)
        constexpr uint32_t kTabPrintReturn = 0x00345FD0u;
        int g_tabPrinted = 0; // the tabs printed this frame (reset by the buttons' pass)
        PS2Runtime::RecompiledFunction g_origPrint = nullptr;
        float g_tabX[8] = {}, g_tabY[8] = {}, g_tabW[8] = {};
        bool g_tabSeen[8] = {};
        PS2_GUEST_CC void print(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kPrint)
            {
                static const bool traceFonts = lcs_diag::wanted("LCS_TRACE_FONT_ATLAS");
                static std::vector<uint64_t> seen;
                if (traceFonts && seen.size() < 256)
                {
                    // CFont::RenderFontBuffer binds slot *(int16_t*)0x408444.
                    constexpr const char *atlases[] = {"font2/font2m", "font1/font1m", "font3/font3m"};
                    int16_t slot;
                    std::memcpy(&slot, rdram + 0x408444u, sizeof(slot));
                    const uint32_t text = GPR_U32(ctx, 4) & kRamMask;
                    if (slot >= 0 && slot < 3 && text && text <= PS2_RAM_SIZE - 96u)
                    {
                        char sample[48]{};
                        uint32_t hash = 2166136261u;
                        for (unsigned i = 0; i < sizeof(sample) - 1; ++i)
                        {
                            uint16_t c;
                            std::memcpy(&c, rdram + text + i * 2u, sizeof(c));
                            if (!c)
                                break;
                            sample[i] = c >= 32 && c < 127 ? char(c) : '?';
                            hash = (hash ^ c) * 16777619u;
                        }
                        const uint32_t caller = GPR_U32(ctx, 31);
                        const uint64_t key = (uint64_t(caller | (uint32_t(slot) << 24)) << 32) | hash;
                        if (sample[0] && std::find(seen.begin(), seen.end(), key) == seen.end())
                        {
                            seen.push_back(key);
                            std::fprintf(stderr, "[lcs-font-atlas] %s, caller %06X, text %s\n", atlases[slot], caller, sample);
                        }
                    }
                }
            }
            if (ctx->pc == kPrint)
            { // diagnosis for the key prompts: printed texts with a button token (~X~ ~O~ ~T~ ~S~ ~K~ ...), in or out of menus
                static int told = 0;
                static const bool promptLog = lcs_diag::wanted("LCS_PROMPT_LOG");
                const uint32_t text = GPR_U32(ctx, 4) & kRamMask;
                if (promptLog && told < 40 && text && text + 512u < PS2_RAM_SIZE)
                {
                    std::string s;
                    bool token = false;
                    for (int i = 0; i < 160; ++i)
                    {
                        uint16_t c;
                        std::memcpy(&c, rdram + text + i * 2u, 2);
                        if (!c)
                            break;
                        char b[8];
                        std::snprintf(b, sizeof(b), c >= 32u && c < 127u ? "%c" : "<%X>", c);
                        s += b;
                        if (c >= 0x80u) // a special character: a button glyph, likely (the width code: 0xE0 and up wider)
                            token = true;
                        if (c == '~' && i + 2 < 160)
                        {
                            uint16_t n, e;
                            std::memcpy(&n, rdram + text + (i + 1) * 2u, 2);
                            std::memcpy(&e, rdram + text + (i + 2) * 2u, 2);
                            if (e == '~' && (n == 'X' || n == 'O' || n == 'T' || n == 'S' || n == 'K' || n == 'U' || n == 'D' ||
                                             n == 'L' || n == 'R' || n == 'A' || n == 'B' || n == 'C'))
                                token = true;
                        }
                    }
                    if (token)
                    {
                        ++told;
                        std::fprintf(stderr, "[lcs-prompt] print \"%s\" at x %.1f y %.1f (ra 0x%06X)\n", s.c_str(), ctx->f[12],
                                     ctx->f[13], GPR_U32(ctx, 31));
                    }
                }
            }
            if (ctx->pc == kPrint && g_menuScreen.load(std::memory_order_relaxed) >= 0)
            {
                const uint32_t text = GPR_U32(ctx, 4) & kRamMask;
                { // diagnosis (2026-11-18: no tab was found this way): the first 40 prints in the menu, with their text
                    static int told = 0;
                    if (told < 40 && text && text + 64u < PS2_RAM_SIZE)
                    {
                        ++told;
                        char s[24] = {};
                        for (int i = 0; i < 23; ++i)
                        {
                            uint16_t c;
                            std::memcpy(&c, rdram + text + i * 2u, 2);
                            if (!c)
                                break;
                            s[i] = c < 128u ? static_cast<char>(c) : '?';
                        }
                        std::fprintf(stderr, "[lcs-menu] print \"%s\" at x %.1f y %.1f (ra 0x%06X, screen %d)\n", s, ctx->f[12],
                                     ctx->f[13], GPR_U32(ctx, 31), g_menuScreen.load(std::memory_order_relaxed));
                    }
                }
                if ((GPR_U32(ctx, 31) & kRamMask) == kTabPrintReturn && g_tabPrinted < 7)
                {
                    const int sc = kTabScreen[g_tabPrinted++];
                    int chars = 0;
                    while (chars < 32 && text + chars * 2u + 2u < PS2_RAM_SIZE)
                    {
                        uint16_t c;
                        std::memcpy(&c, rdram + text + chars * 2u, 2);
                        if (!c)
                            break;
                        ++chars;
                    }
                    g_tabX[sc] = ctx->f[12];
                    g_tabY[sc] = ctx->f[13];
                    g_tabW[sc] = static_cast<float>(chars) * 9.0f;
                    if (!g_tabSeen[sc])
                    {
                        g_tabSeen[sc] = true;
                        std::fprintf(stderr, "[lcs-menu] tab of screen %d at x %.1f y %.1f\n", sc, ctx->f[12], ctx->f[13]);
                    }
                }
            }
            g_origPrint(rdram, ctx, runtime);
        }
        // the tab under the menu point (its screen), or -1: the tabs are left-aligned text, a tab reaching to the
        // next one in its row
        int tabAt(float mx, float my)
        {
            int best = -1;
            for (int sc = 0; sc < 8; ++sc)
            {
                if (!g_tabSeen[sc] || my < g_tabY[sc] - 3.0f || my > g_tabY[sc] + 13.0f || mx < g_tabX[sc] - 4.0f)
                    continue;
                float end = g_tabX[sc] + g_tabW[sc] + 4.0f;
                for (int o = 0; o < 8; ++o)
                    if (o != sc && g_tabSeen[o] && std::fabs(g_tabY[o] - g_tabY[sc]) < 4.0f && g_tabX[o] > g_tabX[sc])
                        end = std::min(end, g_tabX[o] - 4.0f);
                if (mx < end)
                    best = sc;
            }
            return best;
        }
        bool menuMouse(float &mx, float &my)
        {
            float x = 0, y = 0;
            int w = 0, h = 0;
            if (!platform::mousePosition(x, y))
                return false;
            platform::windowSize(w, h);
            if (w <= 0 || h <= 0)
                return false;
            float x0 = 0, y0 = 0, pw = 0, ph = 0;
            present::fitRect(present::displayAspect(false), 0.0f, static_cast<float>(w), static_cast<float>(h), x0, y0, pw, ph);
            if (pw <= 0.0f || ph <= 0.0f)
                return false;
            mx = (x - x0) / pw * kMenuW;
            my = (y - y0) / ph * kMenuH;
            return mx >= 0.0f && mx < kMenuW && my >= 0.0f && my < kMenuH;
        }
        // the entry of `screen` under the menu point, or -1
        int entryAt(const uint8_t *ram, uint32_t screen, float mx, float my)
        {
            if (screen >= static_cast<uint32_t>(kTableScreens) || mx < 24.0f || mx > kMenuW - 24.0f)
                return -1;
            const uint8_t *e = ram + kScreens + screen * kScreenSize + 8u;
            int best = -1;
            float bestDist = 1e9f;
            uint16_t lastY = 0;
            for (uint32_t s = 0; s < kEntries; ++s)
            {
                const uint8_t *en = e + s * kEntrySize;
                uint16_t action, y;
                std::memcpy(&action, en, 2);
                std::memcpy(&y, en + 14, 2);
                if (action == 0u && en[2] == 0u)
                    continue;
                // x / y 0: the game stacks it under the entry before (the game page's NEW, DELETE, EXIT: 20 a row)
                if (y == 0u)
                {
                    if (lastY == 0u)
                        continue;
                    y = static_cast<uint16_t>(lastY + 20u);
                }
                lastY = y;
                const float top = static_cast<float>(y) + kRowTop;
                if (my < top - 2.0f || my > top + kRowHeight)
                    continue;
                const float d = std::fabs(my - (top + kRowHeight * 0.5f));
                if (d < bestDist)
                {
                    bestDist = d;
                    best = static_cast<int>(s);
                }
            }
            return best;
        }
        // the buttons' pass: the hovered entry selected; true on a click on it (its press, once)
        bool mouseInMenu(uint8_t *ram, uint32_t manager, uint32_t screen, uint32_t &entry)
        {
            g_tabPrinted = 0; // the draw after this pass prints the tabs again
            static float lastX = -1.0f, lastY = -1.0f;
            float mx = 0, my = 0;
            const bool in = !platform::mouseCaptured() && menuMouse(mx, my);
            const int hit = in ? entryAt(ram, screen, mx, my) : -1;
            const bool moved = in && (std::fabs(mx - lastX) > 0.5f || std::fabs(my - lastY) > 0.5f);
            if (in)
            {
                lastX = mx;
                lastY = my;
            }
            if (hit >= 0 && moved && static_cast<uint32_t>(hit) != entry)
            {
                entry = static_cast<uint32_t>(hit);
                std::memcpy(ram + manager + kManagerEntry, &entry, 4);
            }
            g_mouseOver.store(hit >= 0 && static_cast<uint32_t>(hit) == entry, std::memory_order_relaxed);
            const bool down = g_click.pressed(platform::mouseDown(platform::MOUSE_LEFT));
            const bool click = down && hit >= 0 && static_cast<uint32_t>(hit) == entry;
            if (down && in && hit < 0)
            {
                const int tab = tabAt(mx, my);
                if (tab >= 0 && static_cast<uint32_t>(tab) != screen)
                {
                    int index = 0;
                    for (int i = 0; i < 7; ++i)
                        if (kTabScreen[i] == tab)
                            index = i + 1;
                    const uint32_t sc = static_cast<uint32_t>(tab), first = 0, idx = static_cast<uint32_t>(index);
                    if (index)
                        std::memcpy(ram + manager, &idx, 4);
                    std::memcpy(ram + manager + kManagerScreen, &sc, 4);
                    std::memcpy(ram + manager + kManagerEntry, &first, 4);
                    entry = 0;
                    std::fprintf(stderr, "[lcs-menu] mouse: tab %d (screen %d)\n", index, tab);
                }
            }
            static const bool log = std::getenv("LCS_MENU_MOUSE_LOG") != nullptr;
            if (log && g_click.was && click)
                std::fprintf(stderr, "[lcs-menu] mouse click at %.0f / %.0f (menu units): screen %u entry %d\n", mx, my, screen, hit);
            return click;
        }

        // our entries' actions: inert while the game handles the buttons, the drawn kind (value / bars) otherwise
        void setActions(uint8_t *ram, bool inert)
        {
            for (uint32_t sc = 0; sc < static_cast<uint32_t>(kTableScreens); ++sc)
                for (uint32_t s = 0; s < kEntries; ++s)
                    if (g_slotRow[sc][s] >= 0 && g_rows[g_slotRow[sc][s]].count > 0)
                    {
                        const Row &r = g_rows[g_slotRow[sc][s]];
                        const uint16_t action = inert ? kInertAction : r.bar ? kBarAction : kValueAction;
                        std::memcpy(ram + kScreens + sc * kScreenSize + 8u + s * kEntrySize, &action, 2);
                    }
        }

        PS2_GUEST_CC void buttons(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kButtons)
                return g_origButtons(rdram, ctx, runtime);
            const uint32_t manager = GPR_U32(ctx, 4) & kRamMask;
            uint32_t screen = 0, entry = 0;
            std::memcpy(&screen, rdram + manager + kManagerScreen, 4);
            std::memcpy(&entry, rdram + manager + kManagerEntry, 4);
            const bool click = mouseInMenu(rdram, manager, screen, entry);
            if (g_exitBuilt)
            {
                static Edge exitSelect;
                const bool onExit = screen == kGame && entry == kExitSlot;
                if (exitSelect.pressed(onExit && (platform::keyDown(platform::KEY_ENTER) || pad(platform::GAMEPAD_SOUTH))) ||
                    (onExit && click))
                {
                    if (exitArmed())
                    {
                        g_quitWanted.store(true, std::memory_order_release);
                        std::fprintf(stderr, "[lcs-menu] EXIT GAME: quitting\n");
                    }
                    else
                    {
                        g_exitArmed = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[lcs-menu] EXIT GAME armed (press again within 3 s)\n");
                    }
                }
            }
            g_screenNow.store(g_controlsBuilt ? static_cast<int>(screen) : -1, std::memory_order_relaxed);
            g_menuScreen.store(static_cast<int>(screen), std::memory_order_relaxed);
            if (!g_pageBuilt)
                return g_origButtons(rdram, ctx, runtime);
            // Esc on a page of ours: the selected entry opens the page above for this pass (the Cross lcs_input sent)
            const int parent = g_controlsBuilt ? parentOf(screen) : -1;
            const bool back = parent >= 0 && entry < kEntries && g_backWanted.exchange(false, std::memory_order_acq_rel);
            if (back && g_haveEnterState[screen])
            {
                // one step back as the game came here (the user's test 17:4x: after an Esc the arrows did nothing): the
                // parent's screen, entry and focus as they were when this page was opened; the game's buttons skip
                // this pass (no press of this frame acts on the parent)
                const uint8_t *st = g_enterState[screen];
                for (uint32_t off : {kManagerScreen, kManagerEntry})
                    std::memcpy(rdram + manager + off, st + (off - kStateFrom), 4);
                rdram[manager + kManagerFocus] = st[kManagerFocus - kStateFrom];
                std::fprintf(stderr, "[lcs-menu] Esc: screen %u -> %d (its state restored)\n", screen, parent);
                std::fprintf(stderr, "[lcs-menu]   state now    %s\n", hexAt(rdram, manager + kStateFrom, kStateBytes).c_str());
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (back)
            {
                uint8_t *e = rdram + kScreens + screen * kScreenSize + 8u + entry * kEntrySize;
                uint8_t saved[kEntrySize];
                std::memcpy(saved, e, kEntrySize);
                setActions(rdram, true);
                std::memcpy(e, &kOpenAction, 2);
                e[11] = static_cast<uint8_t>(parent);
                g_origButtons(rdram, ctx, runtime);
                std::memcpy(e, saved, kEntrySize);
                setActions(rdram, false);
                // one step (the user's ask 2026-11-18: from VIDEO OPTIONS back to DISPLAY, not out to the tabs): the page
                // above with its entries in focus (+0x590, the buttons handle the entries only while it is set) and the
                // entry that opened this page selected
                uint32_t now = 0;
                std::memcpy(&now, rdram + manager + kManagerScreen, 4);
                if (now == static_cast<uint32_t>(parent))
                {
                    const uint8_t *pe = rdram + kScreens + static_cast<uint32_t>(parent) * kScreenSize + 8u;
                    for (uint32_t s = 0; s < kEntries; ++s)
                    {
                        uint16_t action;
                        std::memcpy(&action, pe + s * kEntrySize, 2);
                        if (action == kOpenAction && pe[s * kEntrySize + 11] == screen)
                        {
                            std::memcpy(rdram + manager + kManagerEntry, &s, 4);
                            break;
                        }
                    }
                    rdram[manager + kManagerFocus] = 1u;
                }
                std::fprintf(stderr, "[lcs-menu] Esc: screen %u -> %d (now %u, focus %u)\n", screen, parent, now,
                             rdram[manager + kManagerFocus]);
                return;
            }
            const int row = screen < static_cast<uint32_t>(kTableScreens) && entry < kEntries ? g_slotRow[screen][entry] : -1;
            const bool on = row >= 0;
            const bool l = g_left.pressed(on && (platform::keyDown(platform::KEY_LEFT) || pad(platform::GAMEPAD_DPAD_LEFT)));
            const bool r = g_right.pressed(on && (platform::keyDown(platform::KEY_RIGHT) || pad(platform::GAMEPAD_DPAD_RIGHT)));
            const bool s = g_select.pressed(on && (platform::keyDown(platform::KEY_ENTER) || pad(platform::GAMEPAD_SOUTH))) ||
                           (on && click);
            if (on && (l || r || s))
            {
                const Row &rw = g_rows[row];
                if (rw.count <= 0)
                {
                    if (s)
                    {
                        g_restoreFrom = 0;
                        g_restoreTo = g_mainRows;
                        g_restore.store(true, std::memory_order_release);
                        std::fprintf(stderr, "[lcs-menu] %s\n", rw.label);
                    }
                }
                else
                {
                    const int now = g_value[row].load(std::memory_order_relaxed);
                    int next = ((l ? now - 1 : now + 1) % rw.count + rw.count) % rw.count;
                    if (rw.bar) // a bar: left / right, no wrapping (Cross nothing)
                        next = s && !l && !r ? now : l ? (now > 0 ? now - 1 : 0) : (now + 1 < rw.count ? now + 1 : now);
                    g_value[row].store(next, std::memory_order_relaxed); // shown at once; main thread confirms it
                    g_pending[row].store(next, std::memory_order_release);
                    if (rw.bar)
                        std::fprintf(stderr, "[lcs-menu] %s %d of %d\n", rw.label, next + 1, rw.count);
                    else
                        std::fprintf(stderr, "[lcs-menu] %s %s\n", rw.label, rw.names[next]);
                }
            }
            // the game sees our entries as inert while it handles the buttons (action 11 would switch the subtitles)
            uint8_t pre[kStateBytes];
            std::memcpy(pre, rdram + manager + kStateFrom, kStateBytes);
            setActions(rdram, true);
            g_origButtons(rdram, ctx, runtime);
            setActions(rdram, false);
            // a page of ours opened from its parent: the menu's state on the parent kept, for Esc to go back to it
            uint32_t after = 0;
            std::memcpy(&after, rdram + manager + kManagerScreen, 4);
            if (after != screen && after < static_cast<uint32_t>(kTableScreens) && parentOf(after) == static_cast<int>(screen))
            {
                std::memcpy(g_enterState[after], pre, kStateBytes);
                g_haveEnterState[after] = true;
                std::fprintf(stderr, "[lcs-menu] screen %u -> %u, the menu's state (+0x%X) kept\n", screen, after, kStateFrom);
                std::fprintf(stderr, "[lcs-menu]   state before %s\n", hexBytes(pre, kStateBytes).c_str());
                std::fprintf(stderr, "[lcs-menu]   state after  %s\n", hexAt(rdram, manager + kStateFrom, kStateBytes).c_str());
            }
        }

        std::atomic<bool> g_rowsReady{false}; // setRows done (main thread, its first host frame)

        // the page written into the display screen's entries (once; false: not yet - main.cpp's rows still to come)
        bool buildPage(uint8_t *ram, PS2Runtime *runtime)
        {
            if (std::getenv("LCS_MENU_PAGE") && std::getenv("LCS_MENU_PAGE")[0] == '0')
                return true;
            if (!g_origTextGet || !g_origButtons || !g_origDrawBars)
                return true;
            if (!g_rowsReady.load(std::memory_order_acquire))
                return false;
            if (!g_rows || g_rowCount <= 0)
            {
                std::fprintf(stderr, "[lcs-menu] no rows: no video page\n");
                return true;
            }
            g_textPool = runtime->guestMalloc(static_cast<uint32_t>(g_rowCount) * 2u * kTextChars * 2u, 16u);
            if (!g_textPool)
            {
                std::fprintf(stderr, "[lcs-menu] no guest memory for the texts: no video page\n");
                return true;
            }
            uint8_t original[kEntries * kEntrySize];
            std::memcpy(original, ram + kDisplayEntries, sizeof(original));
            uint8_t page[kEntries * kEntrySize] = {};
            const uint8_t *valueTemplate = original + 1u * kEntrySize; // SUBTITLES: flags, x, label : value
            const uint8_t *barTemplate = original + 0u * kEntrySize;   // BRIGHTNESS
            int next = 0;
            uint32_t slot = 0;
            uint16_t gap = 0;
            for (int item : kLayout)
            {
                if (slot >= kEntries)
                    break;
                if (item == kGap)
                {
                    gap = static_cast<uint16_t>(gap + kStepY / 2u);
                    continue;
                }
                uint8_t *e = page + slot * kEntrySize;
                g_slotRow[kDisplay][slot] = -1;
                if (item >= 0)
                    std::memcpy(e, original + static_cast<uint32_t>(item) * kEntrySize, kEntrySize);
                else
                {
                    if (next >= g_mainRows)
                        continue;
                    const Row &r = g_rows[next];
                    std::memcpy(e, r.bar ? barTemplate : valueTemplate, kEntrySize);
                    const uint16_t action = r.count <= 0 ? kInertAction : r.bar ? kBarAction : kValueAction;
                    const uint16_t align = r.count > 0 ? kAlignValue : kAlignCentre;
                    char key[8] = {'F', 'E', 'V', '_', static_cast<char>('0' + next / 10), static_cast<char>('0' + next % 10), 0, 0};
                    std::memcpy(e, &action, 2);
                    std::memcpy(e + 2, key, 8);
                    std::memcpy(e + 16, &align, 2);
                    g_slotRow[kDisplay][slot] = next++;
                }
                const uint16_t y = static_cast<uint16_t>(kFirstY + slot * kStepY + gap);
                if (g_slotRow[kDisplay][slot] >= 0)
                    g_rowY[g_slotRow[kDisplay][slot]] = y;
                if (item == kGameBrightness)
                    g_brightnessY = y;
                std::memcpy(e + 14, &y, 2);
                ++slot;
            }
            for (uint32_t s = slot; s < kEntries; ++s)
                g_slotRow[kDisplay][s] = -1;
            std::memcpy(ram + kDisplayEntries, page, sizeof(page));
            // SUBTITLES (the game's entry, its action does the toggle) on the audio page's first free slot
            {
                uint8_t *audio = ram + kScreens + kAudio * kScreenSize + 8u;
                for (uint32_t s = 0; s < kEntries; ++s)
                {
                    uint8_t *e = audio + s * kEntrySize;
                    uint16_t action;
                    std::memcpy(&action, e, 2);
                    if (action != 0u || e[2] != 0u)
                        continue;
                    std::memcpy(e, original + static_cast<uint32_t>(kGameSubtitles) * kEntrySize, kEntrySize);
                    std::memcpy(e + 14, &kAudioSubtitlesY, 2);
                    std::fprintf(stderr, "[lcs-menu] SUBTITLES on the audio page, slot %u\n", s);
                    break;
                }
            }
            g_pageBuilt = true;
            std::fprintf(stderr, "[lcs-menu] video page: %u entries (%d of ours), texts at 0x%08X\n", slot, next, g_textPool);
            return true;
        }

        void buildExit(uint8_t *ram, PS2Runtime *runtime)
        {
            if ((std::getenv("LCS_MENU_EXIT") && std::getenv("LCS_MENU_EXIT")[0] == '0') || !g_origTextGet || !g_origButtons)
                return;
            uint8_t *slot = ram + kGameEntries + kExitSlot * kEntrySize;
            uint16_t action;
            std::memcpy(&action, slot, 2);
            if (action != 0u || slot[2] != 0u)
            {
                std::fprintf(stderr, "[lcs-menu] game page slot %u is not free: no EXIT GAME\n", kExitSlot);
                return;
            }
            g_exitText = runtime->guestMalloc(kTextChars * 2u, 16u);
            if (!g_exitText)
                return;
            std::memcpy(slot, ram + kGameEntries + 2u * kEntrySize, kEntrySize); // DELETE GAME: stacked, centred
            const char key[8] = {'F', 'E', 'V', '_', 'E', 'X', 'T', 0};
            std::memcpy(slot, &kInertAction, 2);
            std::memcpy(slot + 2, key, 8);
            slot[11] = 0; // opens no screen
            g_exitBuilt = true;
            std::fprintf(stderr, "[lcs-menu] EXIT GAME in game page slot %u\n", kExitSlot);
        }

        // the MOUSE AND KEYBOARD page's rows and the bindings pages' lines (appended to main.cpp's rows by setRows)
        const char *const kOnOff[] = {"OFF", "ON"};
        const char *g_bindLabels[2][15], *g_bindValues[2][15];
        int g_bindCount[2] = {};
        int g_rowSensitivity = -1, g_rowRaw = -1, g_rowInvert = -1, g_rowBind[2] = {-1, -1};

        void addControlsRows()
        {
            auto add = [](const Row &r) {
                if (g_rowCount >= kMaxRows)
                    return -1;
                g_all[g_rowCount] = r;
                return g_rowCount++;
            };
            Row sens{"MOUSE SENSITIVITY:", nullptr, 10, [] { return lcs_input::mouseSensitivity(); },
                     [](int i) { lcs_input::setMouseSensitivity(i); }, true};
            g_rowSensitivity = add(sens);
            Row raw{"RAW INPUT:", kOnOff, 2, [] { return lcs_input::rawInput() ? 1 : 0; }, [](int i) { lcs_input::setRawInput(i != 0); }};
            g_rowRaw = add(raw);
            Row invert{"INVERT MOUSE:", kOnOff, 2, [] { return lcs_input::invertMouse() ? 1 : 0; },
                       [](int i) { lcs_input::setInvertMouse(i != 0); }};
            g_rowInvert = add(invert);
            for (int v = 0; v < 2; ++v)
            {
                g_bindCount[v] = lcs_input::bindingLines(v == 1, g_bindLabels[v], g_bindValues[v], 15);
                for (int i = 0; i < g_bindCount[v]; ++i)
                {
                    Row line{g_bindLabels[v][i], &g_bindValues[v][i], 1, [] { return 0; }, nullptr};
                    const int r = add(line);
                    if (i == 0)
                        g_rowBind[v] = r;
                }
            }
        }

        // one entry: a copy of the template, then our action, key, y, alignment (and the screen it opens)
        void entry(uint8_t *ram, uint32_t screen, uint32_t slot, const uint8_t *tmpl, uint16_t action, const char key[8], uint16_t y,
                   uint16_t align, uint8_t opens = 0)
        {
            uint8_t *e = ram + kScreens + screen * kScreenSize + 8u + slot * kEntrySize;
            std::memcpy(e, tmpl, kEntrySize);
            std::memcpy(e, &action, 2);
            std::memcpy(e + 2, key, 8);
            e[11] = opens;
            std::memcpy(e + 14, &y, 2);
            std::memcpy(e + 16, &align, 2);
        }
        void rowEntry(uint8_t *ram, uint32_t screen, uint32_t slot, const uint8_t *valueTmpl, const uint8_t *barTmpl, int row, uint16_t y)
        {
            if (row < 0)
                return;
            const Row &r = g_rows[row];
            const char key[8] = {'F', 'E', 'V', '_', static_cast<char>('0' + row / 10), static_cast<char>('0' + row % 10), 0, 0};
            entry(ram, screen, slot, r.bar ? barTmpl : valueTmpl, r.bar ? kBarAction : kValueAction, key, y, kAlignValue);
            g_slotRow[screen][slot] = row;
            g_rowY[row] = y;
        }
        void clearScreen(uint8_t *ram, uint32_t screen, const char title[8])
        {
            uint8_t *sc = ram + kScreens + screen * kScreenSize;
            std::memset(sc, 0, kScreenSize);
            std::memcpy(sc, title, 8);
            for (uint32_t s = 0; s < kEntries; ++s)
                g_slotRow[screen][s] = -1;
        }

        // CONTROLS -> CONTROLLER SETUP (the game's four entries) / MOUSE AND KEYBOARD SETUP (sensitivity, raw input,
        // CHANGE CONTROLS -> FOOT / VEHICLE CONTROLS: the bindings). LCS_MENU_CONTROLS=0: the game's page.
        void buildControls(uint8_t *ram, PS2Runtime *runtime)
        {
            if (std::getenv("LCS_MENU_CONTROLS") && std::getenv("LCS_MENU_CONTROLS")[0] == '0')
                return;
            for (uint32_t sc : {kScrController, kScrMouse, kScrChange, kScrFoot, kScrVehicle})
            {
                const uint8_t *e = ram + kScreens + sc * kScreenSize + 8u;
                for (uint32_t s = 0; s < kEntries; ++s)
                    if (e[s * kEntrySize] || e[s * kEntrySize + 1] || e[s * kEntrySize + 2])
                    {
                        std::fprintf(stderr, "[lcs-menu] screen %u is in use: no controls pages\n", sc);
                        return;
                    }
            }
            const uint32_t pool = runtime->guestMalloc(static_cast<uint32_t>(sizeof(g_fixed) / sizeof(g_fixed[0])) * kTextChars * 2u, 16u);
            if (!pool)
                return;
            uint32_t at = pool;
            for (Fixed &f : g_fixed)
            {
                uint32_t i = 0;
                for (; f.text[i] && i + 1u < kTextChars; ++i)
                {
                    const uint16_t c = static_cast<uint8_t>(f.text[i]);
                    std::memcpy(ram + at + i * 2u, &c, 2);
                }
                const uint16_t zero = 0;
                std::memcpy(ram + at + i * 2u, &zero, 2);
                f.at = at;
                at += kTextChars * 2u;
            }
            // the game's controls entries (CONFIGURATION, TYPE, INVERT LOOK, VIBRATION) and the templates
            uint8_t original[kEntries * kEntrySize];
            std::memcpy(original, ram + kScreens + kControls * kScreenSize + 8u, sizeof(original));
            const uint8_t *valueTmpl = original; // CONFIGURATION: label : value
            uint8_t barTmpl[kEntrySize];
            std::memcpy(barTmpl, ram + kDisplayEntries, kEntrySize); // BRIGHTNESS (the display page's first entry)

            // CONTROLS: two entries opening the pages
            uint8_t name[8];
            std::memcpy(name, ram + kScreens + kControls * kScreenSize, 8);
            clearScreen(ram, kControls, reinterpret_cast<const char *>(name));
            entry(ram, kControls, 0, valueTmpl, kOpenAction, g_fixed[0].key, 60, kAlignCentre, kScrController);
            entry(ram, kControls, 1, valueTmpl, kOpenAction, g_fixed[1].key, 80, kAlignCentre, kScrMouse);
            // CONTROLLER SETUP: the game's four
            clearScreen(ram, kScrController, g_fixed[5].key);
            std::memcpy(ram + kScreens + kScrController * kScreenSize + 8u, original, 4u * kEntrySize);
            // MOUSE AND KEYBOARD SETUP
            clearScreen(ram, kScrMouse, g_fixed[6].key);
            rowEntry(ram, kScrMouse, 0, valueTmpl, barTmpl, g_rowSensitivity, 40);
            rowEntry(ram, kScrMouse, 1, valueTmpl, barTmpl, g_rowInvert, 60);
            rowEntry(ram, kScrMouse, 2, valueTmpl, barTmpl, g_rowRaw, 80);
            entry(ram, kScrMouse, 3, valueTmpl, kOpenAction, g_fixed[2].key, 110, kAlignCentre, kScrChange);
            // CHANGE CONTROLS
            clearScreen(ram, kScrChange, g_fixed[7].key);
            entry(ram, kScrChange, 0, valueTmpl, kOpenAction, g_fixed[3].key, 60, kAlignCentre, kScrFoot);
            entry(ram, kScrChange, 1, valueTmpl, kOpenAction, g_fixed[4].key, 80, kAlignCentre, kScrVehicle);
            // FOOT / VEHICLE CONTROLS: the bindings (shown; changing them comes next)
            for (int v = 0; v < 2; ++v)
            {
                const uint32_t sc = v ? kScrVehicle : kScrFoot;
                clearScreen(ram, sc, g_fixed[v ? 9 : 8].key);
                for (int i = 0; i < g_bindCount[v] && i < static_cast<int>(kEntries); ++i)
                    rowEntry(ram, sc, static_cast<uint32_t>(i), valueTmpl, barTmpl, g_rowBind[v] + i, static_cast<uint16_t>(30 + 13 * i));
            }
            g_controlsBuilt = true;
            std::fprintf(stderr, "[lcs-menu] controls pages: CONTROLS -> %u / %u, CHANGE CONTROLS %u -> %u / %u (%d / %d bindings)\n",
                         kScrController, kScrMouse, kScrChange, kScrFoot, kScrVehicle, g_bindCount[0], g_bindCount[1]);
        }

        // DISPLAY -> DISPLAY OPTIONS / VIDEO OPTIONS: the entries buildPage put on the display page are moved, in their
        // order, onto the two pages (the quality ones to VIDEO OPTIONS), RESTORE DEFAULTS on both, half a row below;
        // the display page keeps two entries that open them. LCS_MENU_SPLIT=0: one page as before.
        bool videoRow(int row)
        {
            static const char *const kVideo[] = {"MSAA", "TEXTURE FILTERING", "DRAW DISTANCE", "TRAILS", "BETA POLICE"};
            for (const char *v : kVideo)
                if (std::strncmp(g_rows[row].label, v, std::strlen(v)) == 0)
                    return true;
            return false;
        }
        void splitDisplay(uint8_t *ram)
        {
            if ((std::getenv("LCS_MENU_SPLIT") && std::getenv("LCS_MENU_SPLIT")[0] == '0') || !g_pageBuilt || !g_controlsBuilt)
                return;
            for (uint32_t sc : {kScrDisplayOpts, kScrVideoOpts})
            {
                const uint8_t *e = ram + kScreens + sc * kScreenSize + 8u;
                for (uint32_t s = 0; s < kEntries; ++s)
                    if (e[s * kEntrySize] || e[s * kEntrySize + 1] || e[s * kEntrySize + 2])
                    {
                        std::fprintf(stderr, "[lcs-menu] screen %u is in use: DISPLAY stays one page\n", sc);
                        return;
                    }
            }
            uint8_t page[kEntries * kEntrySize];
            std::memcpy(page, ram + kDisplayEntries, sizeof(page));
            int rowOf[kEntries];
            for (uint32_t s = 0; s < kEntries; ++s)
                rowOf[s] = g_slotRow[kDisplay][s];
            const uint8_t *linkTmpl = nullptr; // a label : value entry of ours (its x and flags)
            for (uint32_t s = 0; s < kEntries && !linkTmpl; ++s)
                if (rowOf[s] >= 0 && g_rows[rowOf[s]].count > 1 && !g_rows[rowOf[s]].bar)
                    linkTmpl = page + s * kEntrySize;
            if (!linkTmpl)
                return;
            uint8_t name[8];
            std::memcpy(name, ram + kScreens + kDisplay * kScreenSize, 8);
            clearScreen(ram, kScrDisplayOpts, g_fixed[kFixedDisplayTitle].key);
            clearScreen(ram, kScrVideoOpts, g_fixed[kFixedVideoTitle].key);
            uint32_t used[2] = {0, 0};
            int restore = -1;
            uint32_t restoreSlot = 0;
            auto place = [&](int page2, const uint8_t *e, int row) {
                const uint32_t sc = page2 ? kScrVideoOpts : kScrDisplayOpts;
                const uint32_t slot = used[page2]++;
                if (slot >= kEntries)
                    return;
                uint8_t *to = ram + kScreens + sc * kScreenSize + 8u + slot * kEntrySize;
                std::memcpy(to, e, kEntrySize);
                const uint16_t y = static_cast<uint16_t>(kFirstY + slot * kStepY + (row >= 0 && g_rows[row].count <= 0 ? kStepY / 2u : 0u));
                std::memcpy(to + 14, &y, 2);
                g_slotRow[sc][slot] = row;
                if (row >= 0)
                    g_rowY[row] = y;
                else
                {
                    g_brightnessY = y; // the game's BRIGHTNESS (its bars follow)
                    g_brightnessScreen = sc;
                }
            };
            // SUBTITLES (the game's entry, buildPage put it on the audio page) back on DISPLAY OPTIONS under BRIGHTNESS
            // (the user's ask 2026-11-18)
            uint8_t subtitles[kEntrySize] = {};
            bool haveSubtitles = false;
            {
                uint8_t *audio = ram + kScreens + kAudio * kScreenSize + 8u;
                for (uint32_t s = 0; s < kEntries && !haveSubtitles; ++s)
                    if (std::memcmp(audio + s * kEntrySize + 2, "FED_SUB", 7) == 0)
                    {
                        std::memcpy(subtitles, audio + s * kEntrySize, kEntrySize);
                        std::memset(audio + s * kEntrySize, 0, kEntrySize);
                        haveSubtitles = true;
                    }
            }
            for (uint32_t s = 0; s < kEntries; ++s)
            {
                const uint8_t *e = page + s * kEntrySize;
                uint16_t action;
                std::memcpy(&action, e, 2);
                if (action == 0u && e[2] == 0u)
                    continue;
                const int row = rowOf[s];
                if (row >= 0 && g_rows[row].count <= 0)
                {
                    restore = row;
                    restoreSlot = s;
                    continue;
                }
                place(row >= 0 && videoRow(row) ? 1 : 0, e, row);
                if (row < 0 && haveSubtitles) // after the game's BRIGHTNESS
                {
                    const uint32_t slot = used[0]++;
                    if (slot < kEntries)
                    {
                        uint8_t *to = ram + kScreens + kScrDisplayOpts * kScreenSize + 8u + slot * kEntrySize;
                        std::memcpy(to, subtitles, kEntrySize);
                        const uint16_t y = static_cast<uint16_t>(kFirstY + slot * kStepY);
                        std::memcpy(to + 14, &y, 2);
                        g_slotRow[kScrDisplayOpts][slot] = -1;
                    }
                    haveSubtitles = false;
                }
            }
            if (restore >= 0)
                for (int p = 0; p < 2; ++p)
                    place(p, page + restoreSlot * kEntrySize, restore);
            clearScreen(ram, kDisplay, reinterpret_cast<const char *>(name));
            entry(ram, kDisplay, 0, linkTmpl, kOpenAction, g_fixed[kFixedDisplayLink].key, 60, kAlignCentre, kScrDisplayOpts);
            entry(ram, kDisplay, 1, linkTmpl, kOpenAction, g_fixed[kFixedVideoLink].key, 80, kAlignCentre, kScrVideoOpts);
            g_displaySplit = true;
            std::fprintf(stderr, "[lcs-menu] DISPLAY -> DISPLAY OPTIONS %u (%u entries) / VIDEO OPTIONS %u (%u entries)\n",
                         kScrDisplayOpts, used[0], kScrVideoOpts, used[1]);
        }

        // the controller picture of the CONTROLS screen: not drawn once the page is split
        PS2_GUEST_CC void ctrlPicA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kCtrlPicA && g_controlsBuilt && GPR_U32(ctx, 31) == kCtrlPicRetA)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origCtrlPicA(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void ctrlPicB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kCtrlPicB && g_controlsBuilt && GPR_U32(ctx, 31) == kCtrlPicRetB)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origCtrlPicB(rdram, ctx, runtime);
        }

        void dumpTable(const uint8_t *ram, const char *when)
        {
            std::fprintf(stderr, "[lcs-menu] screen table (%s):\n", when);
            for (int s = 0; s < kScreenCount; ++s)
            {
                const uint8_t *screen = ram + kScreens + s * kScreenSize;
                char name[9] = {};
                std::memcpy(name, screen, 8);
                std::fprintf(stderr, "[lcs-menu]   screen %d %s:", s, name);
                for (uint32_t e = 0; e < kEntries; ++e)
                {
                    const uint8_t *entry = screen + 8 + e * kEntrySize;
                    uint16_t action, y;
                    std::memcpy(&action, entry, 2);
                    std::memcpy(&y, entry + 14, 2);
                    char key[9] = {};
                    std::memcpy(key, entry + 2, 8);
                    if (action || key[0])
                        std::fprintf(stderr, " %u:%s@%u", action, key, y);
                }
                std::fprintf(stderr, "\n");
            }
        }

        PS2Runtime *g_runtime = nullptr;
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        for (auto &screen : g_slotRow)
            for (int &r : screen)
                r = -1;
        for (int i = 0; i < kMaxRows; ++i)
        {
            g_value[i].store(0, std::memory_order_relaxed);
            g_pending[i].store(-1, std::memory_order_relaxed);
        }
        std::fprintf(stderr, "[lcs-menu] experimental menu build: the screen table at 0x%06X\n", kScreens);
        // smooth menu text (gs_hw.cpp gsHwSetFontSmooth): this build only for now. LCS_FONT_SMOOTH=0: the game's texels
        const bool smooth = !(std::getenv("LCS_FONT_SMOOTH") && std::getenv("LCS_FONT_SMOOTH")[0] == '0');
        gsHwSetFontSmooth(smooth);
        std::fprintf(stderr, "[lcs-font] smooth menu text %s\n", smooth ? "on" : "off");
        g_origTextGet = runtime.lookupFunction(kTextGet);
        g_origButtons = runtime.lookupFunction(kButtons);
        g_origDrawBars = runtime.lookupFunction(kDrawBars);
        if (!g_origTextGet || !g_origButtons || !g_origDrawBars)
        {
            std::fprintf(stderr, "[lcs-menu] CText::Get 0x332E68 or the menu's buttons 0x340B80 missing: no video page\n");
            g_origTextGet = g_origButtons = g_origDrawBars = nullptr;
            return;
        }
        runtime.replaceFunction(kTextGet, &textGet);
        runtime.replaceFunction(kButtons, &buttons);
        runtime.replaceFunction(kDrawBars, &drawBars);
        g_origSetColour = runtime.lookupFunction(kSetColour);
        if (g_origSetColour)
            runtime.replaceFunction(kSetColour, &setColour);
        g_origChooseColour = runtime.lookupFunction(kChooseColour);
        if (g_origChooseColour)
            runtime.replaceFunction(kChooseColour, &chooseColour);
        else
            std::fprintf(stderr, "[lcs-menu] ChooseVehicleColour 0x2B9338 missing: no beta police cars\n");
        g_origPrint = runtime.lookupFunction(kPrint);
        if (g_origPrint)
            runtime.replaceFunction(kPrint, &print);
        g_origCtrlPicA = runtime.lookupFunction(kCtrlPicA);
        g_origCtrlPicB = runtime.lookupFunction(kCtrlPicB);
        if (g_origCtrlPicA && g_origCtrlPicB)
        {
            runtime.replaceFunction(kCtrlPicA, &ctrlPicA);
            runtime.replaceFunction(kCtrlPicB, &ctrlPicB);
        }
    }

    bool onSubpage()
    {
        const int s = g_screenNow.load(std::memory_order_relaxed);
        return s >= 0 && parentOf(static_cast<uint32_t>(s)) >= 0;
    }
    void requestBack() { g_backWanted.store(true, std::memory_order_release); }
    bool mouseOverEntry() { return g_mouseOver.load(std::memory_order_relaxed); }
    int currentScreen() { return g_menuScreen.load(std::memory_order_relaxed); }
    bool betaPolice() { return g_betaPolice.load(std::memory_order_relaxed); }
    void setBetaPolice(bool on)
    {
        g_betaPolice.store(on, std::memory_order_relaxed);
        std::fprintf(stderr, "[lcs-menu] beta police cars %s (cars made from now on)\n", on ? "on" : "off");
    }

    // [lcs-menu] The settings kept (user's ask 2026-11-17: "dass es endlich gespeichert wird"): lcs.ini next to the
    // exe (the launcher's run folder changes each start), one "LABEL=index" a row with a value (display, mouse and
    // keyboard pages). Read once after the defaults are taken (RESTORE DEFAULTS still means the built-in ones), written
    // when a value changes. BRIGHTNESS and SUBTITLES are the game's own (its save).
    namespace
    {
        extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
        std::string iniPath()
        {
            // the game folder of the release start (main.cpp releaseStart: the exe's folder or --game-dir)
            if (const char *dir = std::getenv("LCS_GAME_DIR"); dir && *dir)
                return std::string(dir) + "\\lcs.ini";
            char buf[1024] = {};
            const unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
            std::string p(buf, n);
            const size_t cut = p.find_last_of("\\/");
            return (cut == std::string::npos ? std::string() : p.substr(0, cut + 1)) + "lcs.ini";
        }
        std::string iniKey(const char *label)
        {
            std::string k = label ? label : "";
            while (!k.empty() && (k.back() == ':' || k.back() == ' '))
                k.pop_back();
            return k;
        }
        int g_saved[kMaxRows];
        bool g_iniLoaded = false;
        void loadIni()
        {
            for (int i = 0; i < g_rowCount; ++i)
                g_saved[i] = g_value[i].load(std::memory_order_relaxed);
            g_iniLoaded = true;
            FILE *f = std::fopen(iniPath().c_str(), "r");
            if (!f)
            {
                std::fprintf(stderr, "[lcs-menu] no %s yet: the defaults\n", iniPath().c_str());
                return;
            }
            char line[256];
            int read = 0;
            while (std::fgets(line, sizeof(line), f))
            {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                    l.pop_back();
                const size_t eq = l.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = l.substr(0, eq);
                const int v = std::atoi(l.c_str() + eq + 1);
                for (int i = 0; i < g_rowCount; ++i)
                    if (g_all[i].count > 1 && iniKey(g_all[i].label) == key && v >= 0 && v < g_all[i].count)
                    {
                        g_pending[i].store(v, std::memory_order_relaxed);
                        g_saved[i] = v;
                        ++read;
                    }
            }
            std::fclose(f);
            std::fprintf(stderr, "[lcs-menu] %d settings from %s\n", read, iniPath().c_str());
        }
        void saveIni()
        {
            FILE *f = std::fopen(iniPath().c_str(), "w");
            if (!f)
            {
                std::fprintf(stderr, "[lcs-menu] %s not writable: settings not kept\n", iniPath().c_str());
                return;
            }
            std::fprintf(f, "; LCS Recomp settings (pause menu), written by the game\n");
            for (int i = 0; i < g_rowCount; ++i)
                if (g_all[i].count > 1) // settings only (the bindings pages' lines are text)
                    std::fprintf(f, "%s=%d\n", iniKey(g_all[i].label).c_str(), g_saved[i]);
            std::fclose(f);
        }
    }

    void setRows(const Row *rows, int count)
    {
        g_mainRows = g_rowCount = count < kMaxRows ? count : kMaxRows;
        for (int i = 0; i < g_rowCount; ++i)
            g_all[i] = rows[i];
        addControlsRows();
        for (int i = 0; i < g_rowCount; ++i)
        {
            g_defaults[i] = g_all[i].count > 0 && g_all[i].get ? g_all[i].get() : 0; // g_all: main.cpp's and ours
            g_value[i].store(g_defaults[i], std::memory_order_relaxed);
            g_pending[i].store(-1, std::memory_order_relaxed);
        }
        loadIni();
        g_rowsReady.store(true, std::memory_order_release);
    }

    void hostFrame()
    {
        if (g_quitWanted.exchange(false, std::memory_order_acq_rel))
            platform::requestQuit();
        if (!g_rows)
            return;
        const bool restore = g_restore.exchange(false, std::memory_order_acq_rel);
        for (int i = 0; i < g_rowCount; ++i)
        {
            const Row &r = g_rows[i];
            if (r.count <= 0)
            {
                if (restore && r.set && i >= g_restoreFrom && i < g_restoreTo)
                    r.set(0);
                continue;
            }
            const bool reset = restore && i >= g_restoreFrom && i < g_restoreTo;
            const int want = reset ? g_defaults[i] : g_pending[i].exchange(-1, std::memory_order_acq_rel);
            if (reset)
                g_pending[i].store(-1, std::memory_order_relaxed);
            if (want >= 0 && r.set && (!r.get || r.get() != want))
                r.set(want);
            if (r.get)
                g_value[i].store(r.get(), std::memory_order_relaxed);
        }
        if (restore)
            std::fprintf(stderr, "[lcs-menu] defaults restored\n");
        // kept when a value changed (from the menu, the F2 page or a key), at most twice a second
        static auto lastSave = std::chrono::steady_clock::now();
        bool changed = false;
        if (g_iniLoaded)
            for (int i = 0; i < g_rowCount; ++i)
                if (g_all[i].count > 1 && g_pending[i].load(std::memory_order_relaxed) < 0)
                {
                    const int v = g_value[i].load(std::memory_order_relaxed);
                    if (v != g_saved[i])
                    {
                        g_saved[i] = v;
                        changed = true;
                    }
                }
        static bool dirty = false;
        dirty = dirty || changed;
        if (dirty && std::chrono::steady_clock::now() - lastSave > std::chrono::milliseconds(500))
        {
            dirty = false;
            lastSave = std::chrono::steady_clock::now();
            saveIni();
        }
    }

    // once a game frame (lcs_bootstrap submitGameFrame): the page at the first frame; the table logged then and when
    // the menu first comes up
    void frame(uint8_t *ram, bool paused)
    {
        static int dumps = 0;
        static bool wasPaused = false;
        static bool exitDone = false;
        if (!exitDone)
        {
            exitDone = true;
            buildExit(ram, g_runtime);
        }
        static bool pageDone = false, controlsDone = false;
        if (!pageDone)
            pageDone = buildPage(ram, g_runtime);
        if (pageDone && !controlsDone)
        {
            controlsDone = true;
            if (g_pageBuilt)
            {
                buildControls(ram, g_runtime);
                splitDisplay(ram);
            }
        }
        const bool dump = dumps == 0 || (paused && !wasPaused && dumps == 1);
        wasPaused = paused;
        if (!dump)
            return;
        ++dumps;
        dumpTable(ram, paused ? "menu up" : "first frame");
    }
#else
    void install(PS2Runtime &) {}
    void frame(uint8_t *, bool) {}
    bool onSubpage() { return false; }
    void requestBack() {}
    bool mouseOverEntry() { return false; }
    int currentScreen() { return -1; }
    bool betaPolice() { return false; }
    void setBetaPolice(bool) {}
    void setRows(const Row *, int) {}
    void hostFrame() {}
#endif
}
