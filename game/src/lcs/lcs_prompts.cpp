#include "lcs_prompts.h"

#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/ps2_render_worker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lcs_input { int device(); bool inVehicle(); }

namespace lcs_prompts
{
    namespace
    {
        struct IconSize { unsigned w, h; };
        struct PixelRun { unsigned count; uint32_t rgba; };
#include "lcs_prompt_atlas.inc"

        enum Control : unsigned
        {
            Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, LeftStick, RightStick,
            Dpad, Up, Down, Left, Right, Vertical, Horizontal, Start, Select, ControlCount,
        };
        constexpr Icon kXbox[] = {
            xbox_button_color_a, xbox_button_color_b, xbox_button_color_x, xbox_button_color_y,
            xbox_lb, xbox_rb, xbox_lt, xbox_rt, xbox_ls, xbox_rs, xbox_stick_l, xbox_stick_r,
            xbox_dpad, xbox_dpad_up, xbox_dpad_down, xbox_dpad_left, xbox_dpad_right,
            xbox_dpad_vertical, xbox_dpad_horizontal, xbox_button_menu, xbox_button_view,
        };
        constexpr Icon kPlayStation[] = {
            playstation_button_cross, playstation_button_circle, playstation_button_square,
            playstation_button_triangle, playstation_trigger_l1, playstation_trigger_r1,
            playstation_trigger_l2, playstation_trigger_r2, playstation_button_l3, playstation_button_r3,
            playstation_stick_l, playstation_stick_r, playstation_dpad, playstation_dpad_up, playstation_dpad_down,
            playstation_dpad_left, playstation_dpad_right, playstation_dpad_vertical, playstation_dpad_horizontal,
            playstation5_button_options, playstation5_button_create,
        };
        Icon playStationIcon(Control button)
        {
            using Model = platform::PlayStationModel;
            const auto model = platform::playStationModel();
            if (model == Model::DualSense || model == Model::Unknown)
                return kPlayStation[button];
            constexpr Icon outline[] = {
                playstation_button_color_cross_outline, playstation_button_color_circle_outline,
                playstation_button_color_square_outline, playstation_button_color_triangle_outline,
                playstation_trigger_l1_outline, playstation_trigger_r1_outline,
                playstation_trigger_l2_outline, playstation_trigger_r2_outline,
                playstation_button_l3_outline, playstation_button_r3_outline,
            };
            if (button < std::size(outline))
                return outline[button];
            if (button == Start)
                return model == Model::DualShock4 ? playstation4_button_options_outline : playstation3_button_start_outline;
            if (button == Select)
                return model == Model::DualShock4 ? playstation4_button_share_outline : playstation3_button_select_outline;
            return kPlayStation[button];
        }

        constexpr Icon kMenuKeys[] = {
            keyboard_enter, keyboard_escape, keyboard_space, keyboard_escape, keyboard_tab, keyboard_tab,
            keyboard_arrow_left, keyboard_arrow_right, keyboard_enter, keyboard_escape, keyboard_arrows, mouse_move,
            keyboard_arrows, keyboard_arrow_up, keyboard_arrow_down, keyboard_arrow_left, keyboard_arrow_right,
            keyboard_arrows_vertical, keyboard_arrows_horizontal, keyboard_escape, keyboard_tab,
        };
        static_assert(std::size(kXbox) == ControlCount && std::size(kPlayStation) == ControlCount &&
                      std::size(kMenuKeys) == ControlCount);

        struct Action { std::string_view name; Icon key; Control pad; };
        constexpr Action kActions[] = {
            {"AMBUY", keyboard_enter, Cross}, {"AMEXI", keyboard_f, Triangle}, {"AMMOV", keyboard_arrows, LeftStick},
            {"ANS", keyboard_tab, L1}, {"CVEIW", keyboard_v, Select}, {"FREE1", mouse_right, L3}, {"FREE2", mouse_move, LeftStick},
            {"PDBAK", mouse_scroll, R3}, {"PDCTL", mouse_scroll, R2}, {"PDCWE", mouse_scroll, R2},
            {"PDFW", mouse_left, Circle}, {"PDLOO", mouse_move, RightStick}, {"PDLT", mouse_right, R1},
            {"PDSPR", keyboard_space, Cross}, {"PUCF", keyboard_tab, L1},
            {"SNZI", mouse_scroll_up, Square}, {"SNZO", mouse_scroll_down, Cross},
            {"TGSUB", keyboard_2, R3}, {"TRSK", keyboard_y, Cross},
            {"VEACC", keyboard_w, Cross}, {"VEBRK", keyboard_s, Square}, {"VECRS", mouse_scroll_up, L1},
            {"VEEE", keyboard_f, Triangle}, {"VEHB", keyboard_space, R1}, {"VEHN", keyboard_h, L3},
            {"VELB", mouse_scroll, R3}, {"VELL", keyboard_q, L2}, {"VELR", keyboard_e, R2},
            {"VEWEA", mouse_move, RightStick}, {"VEWEI", keyboard_arrows_vertical, LeftStick}, {"VEWEP", mouse_left, Circle},
        };
        constexpr uint16_t kActionBase = 0xE100u;
        constexpr unsigned kTile = 64, kWidth = 1024, kHeight = 512;
        static_assert(IconCount <= (kWidth / kTile) * (kHeight / kTile));
        static_assert(std::size(kIconSizes) == IconCount);

        constexpr uint32_t kConvertChar = 0x166E60u, kDrawChar = 0x164C50u, kFontVertices = 0x321740u;
        constexpr uint32_t kDrawMenuSprite = 0x320D20u, kAdvance = 0x1670B0u, kWrapAdvance = 0x167258u;
        PS2Runtime::RecompiledFunction g_convert, g_drawChar, g_fontVertices, g_drawMenuSprite, g_advance, g_wrapAdvance;
        uint32_t g_tokens;
        bool g_ready;
        bool g_trace;
        struct Selection { Icon key = Original; Control pad = Cross; bool shadow = false; bool menu = false; };
        Selection g_font, g_menu;
        constexpr uint32_t kMenuVertices = 0x2B1A70u;
        PS2Runtime::RecompiledFunction g_menuVertices;
        std::array<Control, std::size(kActions) * 4> g_profileControls{};

        template<typename T>
        T read(const uint8_t *ram, uint32_t at)
        {
            T value{};
            if (at <= PS2_RAM_SIZE - sizeof(value))
                std::memcpy(&value, ram + at, sizeof(value));
            return value;
        }

        struct Quad
        {
            Selection selection;
            int16_t x[4], y[4];
            int xmin, xmax, ymin, ymax;
        };
        using Quads = std::vector<Quad>;
        std::mutex g_queueMutex;
        Quads g_building;
        std::unordered_map<const void *, std::shared_ptr<const Quads>> g_pending;
        std::shared_ptr<const Quads> g_rendering;
        std::shared_ptr<const std::vector<uint32_t>> g_pixels;

        void capture(const uint8_t *ram, uint32_t address, Selection selection)
        {
            if (selection.key == Original || address > PS2_RAM_SIZE - 64u)
                return;
            Quad q{};
            q.selection = selection;
            for (unsigned i = 0; i < 4; ++i)
            {
                q.x[i] = read<int16_t>(ram, address + i * 16u + 8u);
                q.y[i] = read<int16_t>(ram, address + i * 16u + 10u);
            }
            const auto x = std::minmax_element(q.x, q.x + 4), y = std::minmax_element(q.y, q.y + 4);
            q.xmin = *x.first; q.xmax = *x.second;
            q.ymin = *y.first; q.ymax = *y.second;
            if (q.xmin == q.xmax || q.ymin == q.ymax)
                return;
            std::lock_guard lock(g_queueMutex);
            if (g_building.size() < 1024)
                g_building.push_back(q);
        }

        void queued(const void *key, uint32_t sizeBytes)
        {
            std::lock_guard lock(g_queueMutex);
            if (sizeBytes < (256u << 10))
                return;
            if (g_pending.size() > 16)
                g_pending.clear();
            g_pending[key] = std::make_shared<const Quads>(std::move(g_building));
            g_building.clear();
        }

        void processing(const void *key, bool begin)
        {
            if (!begin)
                return;
            std::shared_ptr<const Quads> quads;
            {
                std::lock_guard lock(g_queueMutex);
                const auto it = g_pending.find(key);
                if (it == g_pending.end())
                    return;
                quads = std::move(it->second);
                g_pending.erase(it);
            }
            auto apply = [quads] {
                if (!g_rendering)
                    gsHwSetGlyphAtlas(kWidth, kHeight, g_pixels);
                g_rendering = quads;
            };
            if (ps2_render_worker::enabled())
                ps2_render_worker::runOnGsStage(apply);
            else
                apply();
        }

        bool glyph(const float *xy, uint32_t count, GSHwGlyphHit &hit)
        {
            if (!g_rendering || g_rendering->empty() || (count != 3 && count != 4 && count != 6))
                return false;
            for (const Quad &q : *g_rendering)
            {
                bool all = true, left = false, right = false, top = false, bottom = false;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const float x = xy[i * 2], y = xy[i * 2 + 1];
                    bool found = false;
                    for (unsigned j = 0; j < 4; ++j)
                        found |= std::abs(q.x[j] - x) <= 0.5f && std::abs(q.y[j] - y) <= 0.5f;
                    if (!found) { all = false; break; }
                    left   |= std::abs(q.xmin - x) <= 0.5f;
                    right  |= std::abs(q.xmax - x) <= 0.5f;
                    top    |= std::abs(q.ymin - y) <= 0.5f;
                    bottom |= std::abs(q.ymax - y) <= 0.5f;
                }
                if (!all || !left || !right || !top || !bottom)
                    continue;
                Icon icon = q.selection.key;
                if (lcs_input::device() != 0)
                {
                    switch (platform::gamepadFamily())
                    {
                    case platform::GamepadFamily::Xbox: icon = kXbox[q.selection.pad]; break;
                    case platform::GamepadFamily::PlayStation: icon = playStationIcon(q.selection.pad); break;
                    default: return false;
                    }
                }
                const IconSize size = kIconSizes[icon];
                const float u = float(icon % (kWidth / kTile) * kTile + 1);
                const float v = float(icon / (kWidth / kTile) * kTile + 1);
                if (g_trace)
                {
                    static unsigned shown = 0;
                    if (shown < 24)
                    {
                        ++shown;
                        std::fprintf(stderr, "[lcs-prompts] drawing icon %u (%u vertices)\n", icon, count);
                    }
                }
                hit.shadow = q.selection.shadow;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const bool r = xy[2 * i] > (q.xmin + q.xmax) * 0.5f;
                    const bool b = xy[2 * i + 1] > (q.ymin + q.ymax) * 0.5f;
                    const float growth = q.selection.menu ? 0.12f : 0.18f;
                    hit.dx[i] = (q.xmax - q.xmin) * (r ? 0.5f : -0.5f) * growth;
                    hit.dy[i] = (q.ymax - q.ymin) * (b ? 0.5f : -0.5f) * growth;
                    hit.u[i] = (u + (r ? size.w - 0.5f : 0.5f)) / kWidth;
                    hit.v[i] = (v + (b ? size.h - 0.5f : 0.5f)) / kHeight;
                }
                return true;
            }
            return false;
        }

        unsigned actionIndex(uint16_t c)
        {
            return static_cast<unsigned>(c) - kActionBase;
        }

        Selection literal(uint16_t c)
        {
            if (c == 0xE0) return {keyboard_f, Triangle};
            if (c == 0xE1) return {lcs_input::inVehicle() ? keyboard_s : keyboard_shift, Square};
            if (c == 0xE2) return {lcs_input::inVehicle() ? keyboard_w : keyboard_space, Cross};
            if (c == 0xE3) return {mouse_left, Circle};
            return {};
        }

        PS2_GUEST_CC void convert(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            const unsigned action = actionIndex(static_cast<uint16_t>(GPR_U32(ctx, 4) + 0x20u));
            if (ctx->pc == kConvertChar && action < g_profileControls.size())
            {
                SET_GPR_U32(ctx, 2, 0xC2u); // use the original face glyph's layout and geometry
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_convert(ram, ctx, rt);
        }

        PS2_GUEST_CC void advance(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            if (ctx->pc == kAdvance && actionIndex(static_cast<uint16_t>(GPR_U32(ctx, 4) + 0x20u)) < g_profileControls.size())
                SET_GPR_U32(ctx, 4, 0xC2u);
            g_advance(ram, ctx, rt);
        }

        PS2_GUEST_CC void wrapAdvance(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            if (ctx->pc == kWrapAdvance && actionIndex(static_cast<uint16_t>(GPR_U32(ctx, 4) + 0x20u)) < g_profileControls.size())
                SET_GPR_U32(ctx, 4, 0xC2u);
            g_wrapAdvance(ram, ctx, rt);
        }

        PS2_GUEST_CC void drawChar(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            const Selection previous = g_font;
            if (ctx->pc == kDrawChar)
            {
                const auto caller = GPR_U32(ctx, 31);
                if (caller == 0x16530Cu || caller == 0x16532Cu || caller == 0x16534Cu)
                {
                    const uint16_t c = read<uint16_t>(ram, GPR_U32(ctx, 18) & PS2_RAM_MASK);
                    const auto action = actionIndex(c);
                    if (action < g_profileControls.size())
                    {
                        g_font = {kActions[action / 4].key, g_profileControls[action]};
                        // Some fonts bypass character conversion. DrawChar rejects
                        // indices above 0xD0, so always submit a native face glyph.
                        SET_GPR_U32(ctx, 4, 0xC2u);
                    }
                    else
                        g_font = literal(c);
                }
            }
            g_drawChar(ram, ctx, rt);
            g_font = previous;
        }

        PS2_GUEST_CC void fontVertices(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            const uint32_t vertices = GPR_U32(ctx, 4) & PS2_RAM_MASK;
            const bool mark = ctx->pc == kFontVertices && g_font.key != Original;
            g_fontVertices(ram, ctx, rt);
            if (mark)
                capture(ram, vertices, g_font);
        }

        Selection menuButton(const uint8_t *ram, uint32_t sprite)
        {
            const uint32_t texture = read<uint32_t>(ram, sprite) & PS2_RAM_MASK;
            if (texture < 0x10000u || texture > PS2_RAM_SIZE - 0x30u)
                return {};
            constexpr std::pair<std::string_view, Control> controls[] = {
                {"cross", Cross}, {"circle", Circle}, {"square", Square}, {"triangle", Triangle},
                {"L", L1}, {"R", R1}, {"start", Start}, {"home", Start}, {"select", Select},
                {"up", Up}, {"down", Down}, {"left", Left}, {"right", Right},
            };
            const std::string_view header(reinterpret_cast<const char *>(ram + texture), 0x30u);
            const auto at = header.find("btn_");
            if (at == std::string_view::npos)
                return {};
            const auto name = header.substr(at + 4);
            for (const auto &[base, control] : controls)
                if (name.size() > base.size() && name.starts_with(base) && name[base.size()] == '_')
                    return {kMenuKeys[control], control, false, true};
            return {};
        }

        PS2_GUEST_CC void drawMenuSprite(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            const Selection previous = g_menu;
            if (ctx->pc == kDrawMenuSprite)
            {
                const uint32_t sprite = GPR_U32(ctx, 4) & PS2_RAM_MASK;
                g_menu = menuButton(ram, sprite);
                const uint32_t caller = GPR_U32(ctx, 31);
                if (g_trace && caller >= 0x344E14u && caller <= 0x345990u)
                {
                    static std::vector<uint32_t> seen;
                    if (std::find(seen.begin(), seen.end(), caller) == seen.end())
                    {
                        seen.push_back(caller);
                        const uint32_t texture = read<uint32_t>(ram, sprite) & PS2_RAM_MASK;
                        std::fprintf(stderr, "[lcs-prompts] footer sprite caller=%06X texture=%06X icon=%u", caller,
                                     texture, g_menu.key);
                        if (texture && texture <= PS2_RAM_SIZE - 64u)
                            for (unsigned i = 0; i < 64; ++i)
                            {
                                const unsigned c = ram[texture + i];
                                std::fputc(c >= 32 && c < 127 ? int(c) : '.', stderr);
                            }
                        std::fputc('\n', stderr);
                    }
                }
            }
            g_drawMenuSprite(ram, ctx, rt);
            g_menu = previous;
        }

        PS2_GUEST_CC void menuVertices(uint8_t *ram, R5900Context *ctx, PS2Runtime *rt)
        {
            // The sprite binds/uploads its texture before submitting this strip.
            // Taking the quad earlier can associate it with the texture upload chain.
            if (ctx->pc == kMenuVertices && GPR_U32(ctx, 31) == 0x320DC0u && GPR_U32(ctx, 6) == 4)
                capture(ram, GPR_U32(ctx, 5) & PS2_RAM_MASK, g_menu);
            g_menuVertices(ram, ctx, rt);
        }

        Control originalControl(const uint8_t *ram, uint32_t address, Control fallback)
        {
            if (!address || address > PS2_RAM_SIZE - 6u || read<uint16_t>(ram, address) != '~' ||
                read<uint16_t>(ram, address + 4) != '~')
                return fallback;
            switch (read<uint16_t>(ram, address + 2))
            {
            case 'X': return Cross;
            case 'O': return Circle;
            case 'S': return Square;
            case 'T': return Triangle;
            case 'L': return L1;
            case 'R': return R1;
            default: return fallback;
            }
        }
    }

    uint32_t expandedToken(uint8_t *ram, PS2Runtime &rt, uint32_t stack, uint32_t length, uint32_t original)
    {
        // The expander stores C, profile, and the action name on its stack. Its
        // counter starts at one and advances once per action character.
        if (!g_ready || length < 2 || length > 6)
            return 0;
        stack &= PS2_RAM_MASK;
        original &= PS2_RAM_MASK;
        if (stack > PS2_RAM_SIZE - 16u || read<uint16_t>(ram, stack) != 'C')
            return 0;
        const auto profile = read<uint16_t>(ram, stack + 2);
        if (profile < '0' || profile > '3')
            return 0;
        char name[6]{};
        for (uint32_t i = 0; i < length - 1; ++i)
        {
            const uint16_t ch = read<uint16_t>(ram, stack + 4 + 2 * i);
            if (!(ch >= 'A' && ch <= 'Z') && ch != '1' && ch != '2')
                return 0;
            name[i] = static_cast<char>(ch);
        }
        const std::string_view action(name, length - 1);
        for (unsigned i = 0; i < std::size(kActions); ++i)
        {
            if (kActions[i].name != action)
                continue;
            // Preserve native face glyphs for acceleration and braking, whose literals have
            // the same keyboard meaning in the vehicle HUD. They already take the
            // adaptive glyph path and remain visible if atlas matching misses.
            const auto &entry = kActions[i];
            if (original && read<uint16_t>(ram, original) == '~' && read<uint16_t>(ram, original + 4) == '~')
            {
                const uint16_t button = read<uint16_t>(ram, original + 2);
                if ((entry.name == "VEACC" && button == 'X') ||
                    (entry.name == "VEBRK" && button == 'S'))
                    return 0;
            }
            if (!g_tokens)
                g_tokens = rt.guestMalloc(static_cast<uint32_t>(g_profileControls.size() * 4u), 16u);
            if (!g_tokens)
                return 0;
            const unsigned id = i * 4u + static_cast<unsigned>(profile - '0');
            g_profileControls[id] = originalControl(ram, original, kActions[i].pad);
            const uint16_t text[] = {static_cast<uint16_t>(kActionBase + id), 0};
            const uint32_t addr = g_tokens + id * sizeof(text);
            std::memcpy(ram + addr, text, sizeof(text));
            if (g_trace)
            {
                static std::array<bool, std::size(kActions) * 4> logged{};
                if (!logged[id])
                {
                    logged[id] = true;
                    std::fprintf(stderr, "[lcs-prompts] %s -> action %u\n", kActions[i].name.data(), id);
                }
            }
            return addr;
        }
        return 0;
    }

    uint16_t layoutChar(uint16_t glyph)
    {
        return actionIndex(static_cast<uint16_t>(glyph + 0x20u)) < g_profileControls.size() ? 0xC2u : glyph;
    }

    void install(PS2Runtime &rt)
    {
        g_menuVertices = rt.lookupFunction(kMenuVertices);
        g_convert = rt.lookupFunction(kConvertChar);
        g_drawChar = rt.lookupFunction(kDrawChar);
        g_fontVertices = rt.lookupFunction(kFontVertices);
        g_drawMenuSprite = rt.lookupFunction(kDrawMenuSprite);
        g_advance = rt.lookupFunction(kAdvance);
        g_wrapAdvance = rt.lookupFunction(kWrapAdvance);
        if (!g_menuVertices || !g_convert || !g_drawChar || !g_fontVertices || !g_drawMenuSprite || !g_advance || !g_wrapAdvance)
            return;
        auto pixels = std::make_shared<std::vector<uint32_t>>(kWidth * kHeight);
        size_t run = 0;
        unsigned remaining = kPixelRuns[0].count;
        for (unsigned icon = 1; icon < IconCount; ++icon)
        {
            const auto size = kIconSizes[icon];
            const unsigned x = icon % (kWidth / kTile) * kTile + 1, y = icon / (kWidth / kTile) * kTile + 1;
            for (unsigned row = 0; row < size.h; ++row)
                for (unsigned col = 0; col < size.w; ++col)
                {
                    if (run >= std::size(kPixelRuns))
                        return;
                    (*pixels)[(y + row) * kWidth + x + col] = kPixelRuns[run].rgba;
                    if (--remaining == 0 && ++run < std::size(kPixelRuns))
                        remaining = kPixelRuns[run].count;
                }
        }
        if (run != std::size(kPixelRuns))
            return;
        g_pixels = std::move(pixels);
        g_trace = std::getenv("LCS_PROMPT_LOG") != nullptr;
        rt.replaceFunction(kMenuVertices, &menuVertices);
        rt.replaceFunction(kConvertChar, &convert);
        rt.replaceFunction(kDrawChar, &drawChar);
        rt.replaceFunction(kFontVertices, &fontVertices);
        rt.replaceFunction(kDrawMenuSprite, &drawMenuSprite);
        rt.replaceFunction(kAdvance, &advance);
        rt.replaceFunction(kWrapAdvance, &wrapAdvance);
        ps2chain::addChainObservers(&queued, &processing);
        gsHwSetGlyphClassifier(&glyph);
        g_ready = true;
        std::fprintf(stderr, "[lcs-prompts] Kenney atlas available (%u icons)\n", IconCount - 1u);
    }
}
