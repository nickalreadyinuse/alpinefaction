#include <cstring>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <format>
#include <algorithm>
#include "alpine_settings.h"
#include "misc.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../rf/ui.h"
#include "../rf/sound/sound.h"
#include "../rf/input.h"
#include "../rf/misc.h"
#include "../rf/os/os.h"

#define DEBUG_UI_LAYOUT 0
#define SHARP_UI_TEXT 1

static rf::ui::Gadget* new_gadgets[6]; // Allocate space for 6 options buttons
static rf::ui::Button alpine_options_btn;
static rf::ui::Panel alpine_options_panel;
std::vector<rf::ui::Gadget*> alpine_options_panel_settings;
std::vector<rf::ui::Label*> alpine_options_panel_labels;

// alpine options panel elements
static rf::ui::Checkbox ao_bighud_cbox;
static rf::ui::Label ao_bighud_label;
static rf::ui::Checkbox ao_linearpitch_cbox;
static rf::ui::Label ao_linearpitch_label;
static rf::ui::Checkbox ao_ctfwh_cbox;
static rf::ui::Label ao_ctfwh_label;
static rf::ui::Checkbox ao_damagenum_cbox;
static rf::ui::Label ao_damagenum_label;
static rf::ui::Checkbox ao_hitsounds_cbox;
static rf::ui::Label ao_hitsounds_label;
static rf::ui::Checkbox ao_taunts_cbox;
static rf::ui::Label ao_taunts_label;

static rf::ui::Checkbox ao_swapar_cbox;
static rf::ui::Label ao_swapar_label;
static rf::ui::Checkbox ao_swapgn_cbox;
static rf::ui::Label ao_swapgn_label;
static rf::ui::Checkbox ao_swapsg_cbox;
static rf::ui::Label ao_swapsg_label;
static rf::ui::Checkbox ao_weapshake_cbox;
static rf::ui::Label ao_weapshake_label;
static rf::ui::Checkbox ao_fullbrightchar_cbox;
static rf::ui::Label ao_fullbrightchar_label;
static rf::ui::Checkbox ao_notex_cbox;
static rf::ui::Label ao_notex_label;

static inline void debug_ui_layout([[ maybe_unused ]] rf::ui::Gadget& gadget)
{
#if DEBUG_UI_LAYOUT
    int x = gadget.get_absolute_x() * rf::ui::scale_x;
    int y = gadget.get_absolute_y() * rf::ui::scale_y;
    int w = gadget.w * rf::ui::scale_x;
    int h = gadget.h * rf::ui::scale_y;
    rf::gr::set_color((x ^ y) & 255, 0, 0, 64);
    rf::gr::rect(x, y, w, h);
#endif
}

void __fastcall UiButton_create(rf::ui::Button& this_, int, const char *normal_bm, const char *selected_bm, int x, int y, int id, const char *text, int font)
{
    this_.key = id;
    this_.x = x;
    this_.y = y;
    if (*normal_bm) {
        this_.bg_bitmap = rf::bm::load(normal_bm, -1, false);
        rf::gr::tcache_add_ref(this_.bg_bitmap);
        rf::bm::get_dimensions(this_.bg_bitmap, &this_.w, &this_.h);
    }
    if (*selected_bm) {
        this_.selected_bitmap = rf::bm::load(selected_bm, -1, false);
        rf::gr::tcache_add_ref(this_.selected_bitmap);
        if (this_.bg_bitmap < 0) {
            rf::bm::get_dimensions(this_.selected_bitmap, &this_.w, &this_.h);
        }
    }
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiButton_create_hook{0x004574D0, UiButton_create};

void __fastcall UiButton_set_text(rf::ui::Button& this_, int, const char *text, int font)
{
    delete[] this_.text;
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiButton_set_text_hook{0x00457710, UiButton_set_text};

void __fastcall UiButton_render(rf::ui::Button& this_)
{
    int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
    int w = static_cast<int>(this_.w * rf::ui::scale_x);
    int h = static_cast<int>(this_.h * rf::ui::scale_y);

    if (this_.bg_bitmap >= 0) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::bitmap_scaled(this_.bg_bitmap, x, y, w, h, 0, 0, this_.w, this_.h, false, false, rf::gr::bitmap_clamp_mode);
    }

    if (!this_.enabled) {
        rf::gr::set_color(96, 96, 96, 255);
    }
    else if (this_.highlighted) {
        rf::gr::set_color(240, 240, 240, 255);
    }
    else {
        rf::gr::set_color(192, 192, 192, 255);
    }

    if (this_.enabled && this_.highlighted && this_.selected_bitmap >= 0) {
        auto mode = addr_as_ref<rf::gr::Mode>(0x01775B0C);
        rf::gr::bitmap_scaled(this_.selected_bitmap, x, y, w, h, 0, 0, this_.w, this_.h, false, false, mode);
    }

    // Change clip region for text rendering
    int clip_x, clip_y, clip_w, clip_h;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    rf::gr::set_clip(x, y, w, h);

    std::string_view text_sv{this_.text};
    int num_lines = 1 + std::count(text_sv.begin(), text_sv.end(), '\n');
    int text_h = rf::gr::get_font_height(this_.font) * num_lines;
    int text_y = (h - text_h) / 2;
    rf::gr::string(rf::gr::center_x, text_y, this_.text, this_.font);

    // Restore clip region
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);

    debug_ui_layout(this_);
}
FunHook UiButton_render_hook{0x004577A0, UiButton_render};

void __fastcall UiLabel_create(rf::ui::Label& this_, int, rf::ui::Gadget *parent, int x, int y, const char *text, int font)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    int text_w, text_h;
    rf::gr::get_string_size(&text_w, &text_h, text, -1, font);
    this_.w = static_cast<int>(text_w / rf::ui::scale_x);
    this_.h = static_cast<int>(text_h / rf::ui::scale_y);
    this_.text = strdup(text);
    this_.font = font;
    this_.align = rf::gr::ALIGN_LEFT;
    this_.clr.set(0, 0, 0, 255);
}
FunHook UiLabel_create_hook{0x00456B60, UiLabel_create};

void __fastcall UiLabel_create2(rf::ui::Label& this_, int, rf::ui::Gadget *parent, int x, int y, int w, int h, const char *text, int font)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    this_.w = w;
    this_.h = h;
    if (*text == ' ') {
        while (*text == ' ') {
            ++text;
        }
        this_.align = rf::gr::ALIGN_CENTER;
    }
    else {
        this_.align = rf::gr::ALIGN_LEFT;
    }
    this_.text = strdup(text);
    this_.font = font;
    this_.clr.set(0, 0, 0, 255);
}
FunHook UiLabel_create2_hook{0x00456C20, UiLabel_create2};

void __fastcall UiLabel_set_text(rf::ui::Label& this_, int, const char *text, int font)
{
    delete[] this_.text;
    this_.text = strdup(text);
    this_.font = font;
}
FunHook UiLabel_set_text_hook{0x00456DC0, UiLabel_set_text};

void __fastcall UiLabel_render(rf::ui::Label& this_)
{
    if (!this_.enabled) {
        rf::gr::set_color(48, 48, 48, 128);
    }
    else if (this_.highlighted) {
        rf::gr::set_color(240, 240, 240, 255);
    }
    else {
        rf::gr::set_color(this_.clr);
    }
    int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
    int text_w, text_h;
    rf::gr::get_string_size(&text_w, &text_h, this_.text, -1, this_.font);
    if (this_.align == rf::gr::ALIGN_CENTER) {
        x += static_cast<int>(this_.w * rf::ui::scale_x / 2);
    }
    else if (this_.align == rf::gr::ALIGN_RIGHT) {
        x += static_cast<int>(this_.w * rf::ui::scale_x);
    }
    else {
        x += static_cast<int>(1 * rf::ui::scale_x);
    }
    rf::gr::string_aligned(this_.align, x, y, this_.text, this_.font);

    debug_ui_layout(this_);
}
FunHook UiLabel_render_hook{0x00456ED0, UiLabel_render};

void __fastcall UiInputBox_create(rf::ui::InputBox& this_, int, rf::ui::Gadget *parent, int x, int y, const char *text, int font, int w)
{
    this_.parent = parent;
    this_.x = x;
    this_.y = y;
    this_.w = w;
    this_.h = static_cast<int>(rf::gr::get_font_height(font) / rf::ui::scale_y);
    this_.max_text_width = static_cast<int>(w * rf::ui::scale_x);
    this_.font = font;
    std::strncpy(this_.text, text, std::size(this_.text));
    this_.text[std::size(this_.text) - 1] = '\0';
}
FunHook UiInputBox_create_hook{0x00456FE0, UiInputBox_create};

void __fastcall UiInputBox_render(rf::ui::InputBox& this_, void*)
{
    if (this_.enabled && this_.highlighted) {
        rf::gr::set_color(240, 240, 240, 255);
    }
    else {
        rf::gr::set_color(192, 192, 192, 255);
    }

    int x = static_cast<int>((this_.get_absolute_x() + 1) * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);
    int clip_x, clip_y, clip_w, clip_h;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    rf::gr::set_clip(x, y, this_.max_text_width, static_cast<int>(this_.h * rf::ui::scale_y + 5)); // for some reason input fields are too thin
    int text_offset_x = static_cast<int>(1 * rf::ui::scale_x);
    rf::gr::string(text_offset_x, 0, this_.text, this_.font);

    if (this_.enabled && this_.highlighted) {
        rf::ui::update_input_box_cursor();
        if (rf::ui::input_box_cursor_visible) {
            int text_w, text_h;
            rf::gr::get_string_size(&text_w, &text_h, this_.text, -1, this_.font);
            rf::gr::string(text_offset_x + text_w, 0, "_", this_.font);
        }
    }
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);

    debug_ui_layout(this_);
}
FunHook UiInputBox_render_hook{0x004570E0, UiInputBox_render};

void __fastcall UiCycler_add_item(rf::ui::Cycler& this_, int, const char *text, int font)
{
    if (this_.num_items < rf::ui::Cycler::max_items) {
        this_.items_text[this_.num_items] = strdup(text);
        this_.items_font[this_.num_items] = font;
        ++this_.num_items;
    }
}
FunHook UiCycler_add_item_hook{0x00458080, UiCycler_add_item};

void __fastcall UiCycler_render(rf::ui::Cycler& this_)
{
    if (this_.enabled && this_.highlighted) {
        rf::gr::set_color(255, 255, 255, 255);
    }
    else if (this_.enabled) {
        rf::gr::set_color(192, 192, 192, 255);
    }
    else {
        rf::gr::set_color(96, 96, 96, 255);
    }

    int x = static_cast<int>(this_.get_absolute_x() * rf::ui::scale_x);
    int y = static_cast<int>(this_.get_absolute_y() * rf::ui::scale_y);

    const char* text = this_.items_text[this_.current_item];
    int font = this_.items_font[this_.current_item];
    int font_h = rf::gr::get_font_height(font);
    int text_x = x + static_cast<int>(this_.w * rf::ui::scale_x / 2);
    int text_y = y + static_cast<int>((this_.h * rf::ui::scale_y - font_h) / 2);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, text_x, text_y, text, font);

    debug_ui_layout(this_);
}
FunHook UiCycler_render_hook{0x00457F40, UiCycler_render};

CallHook<void(int*, int*, const char*, int, int, char, int)> popup_set_text_gr_split_str_hook{
    0x00455A7D,
    [](int *n_chars, int *start_indices, const char *str, int max_pixel_w, int max_lines, char ignore_char, int font) {
        max_pixel_w = static_cast<int>(max_pixel_w * rf::ui::scale_x);
        popup_set_text_gr_split_str_hook.call_target(n_chars, start_indices, str, max_pixel_w, max_lines, ignore_char, font);
    },
};

static bool is_any_font_modded()
{
    auto rfpc_large_checksum = rf::get_file_checksum("rfpc-large.vf");
    auto rfpc_medium_checksum = rf::get_file_checksum("rfpc-medium.vf");
    auto rfpc_small_checksum = rf::get_file_checksum("rfpc-small.vf");
    // Note: rfpc-large differs between Steam and CD game distributions
    bool rfpc_large_modded = rfpc_large_checksum != 0x5E7DC24Au && rfpc_large_checksum != 0xEB80AD63u;
    bool rfpc_medium_modded = rfpc_medium_checksum != 0x19E7184Cu;
    bool rfpc_small_modded = rfpc_small_checksum != 0xAABA52E6u;
    bool any_font_modded = rfpc_large_modded || rfpc_medium_modded || rfpc_small_modded;
    if (any_font_modded) {
        xlog::info("Detected modded fonts: rfpc-large {} ({:08X}) rfpc-medium {} ({:08X}) rfpc-small {} ({:08X})",
            rfpc_large_modded, rfpc_large_checksum,
            rfpc_medium_modded, rfpc_medium_checksum,
            rfpc_small_modded, rfpc_small_checksum
        );
    }
    return any_font_modded;
}

FunHook<void()> menu_init_hook{
    0x00442BB0,
    []() {
        menu_init_hook.call_target();
#if SHARP_UI_TEXT
        xlog::info("UI scale: {:.4f} {:.4f}", rf::ui::scale_x, rf::ui::scale_y);
        if (rf::ui::scale_y > 1.0f && !is_any_font_modded()) {
            int large_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 14.5f))); // 32
            int medium_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 9.0f))); // 20
            int small_font_size = std::min(128, static_cast<int>(std::round(rf::ui::scale_y * 7.5f))); // 16
            xlog::info("UI font sizes: {} {} {}", large_font_size, medium_font_size, small_font_size);

            rf::ui::large_font = rf::gr::load_font(std::format("boldfont.ttf:{}", large_font_size).c_str());
            rf::ui::medium_font_0 = rf::gr::load_font(std::format("regularfont.ttf:{}", medium_font_size).c_str());
            rf::ui::medium_font_1 = rf::ui::medium_font_0;
            rf::ui::small_font = rf::gr::load_font(std::format("regularfont.ttf:{}", small_font_size).c_str());
        }
#endif
    },
};

auto UiInputBox_add_char = reinterpret_cast<bool (__thiscall*)(void *this_, char c)>(0x00457260);

extern FunHook<bool __fastcall(void*, int, rf::Key)> UiInputBox_process_key_hook;
bool __fastcall UiInputBox_process_key_new(void *this_, int edx, rf::Key key)
{
    if (key == (rf::KEY_V | rf::KEY_CTRLED)) {
        char buf[256];
        rf::os_get_clipboard_text(buf, static_cast<int>(std::size(buf) - 1));
        for (int i = 0; buf[i]; ++i) {
            UiInputBox_add_char(this_, buf[i]);
        }
        return true;
    }
    return UiInputBox_process_key_hook.call_target(this_, edx, key);
}
FunHook<bool __fastcall(void *this_, int edx, rf::Key key)> UiInputBox_process_key_hook{
    0x00457300,
    UiInputBox_process_key_new,
};

void ao_bighud_cbox_on_click(int x, int y) {
    g_alpine_game_config.big_hud = !g_alpine_game_config.big_hud;
    ao_bighud_cbox.checked = g_alpine_game_config.big_hud;
    set_big_hud(g_alpine_game_config.big_hud);

    if (ao_bighud_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {} {}", x, y, ao_bighud_cbox.checked, g_alpine_game_config.big_hud);
}

void ao_linearpitch_cbox_on_click(int x, int y) {
    g_alpine_game_config.mouse_linear_pitch = !g_alpine_game_config.mouse_linear_pitch;
    ao_linearpitch_cbox.checked = g_alpine_game_config.mouse_linear_pitch;

    if (ao_linearpitch_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_linearpitch_cbox.checked);
}

void ao_ctfwh_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_ctf_icons = !g_alpine_game_config.world_hud_ctf_icons;
    ao_ctfwh_cbox.checked = g_alpine_game_config.world_hud_ctf_icons;

    if (ao_ctfwh_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_ctfwh_cbox.checked);
}

void ao_damagenum_cbox_on_click(int x, int y) {
    g_alpine_game_config.world_hud_damage_numbers = !g_alpine_game_config.world_hud_damage_numbers;
    ao_damagenum_cbox.checked = g_alpine_game_config.world_hud_damage_numbers;

    if (ao_damagenum_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_damagenum_cbox.checked);
}

void ao_hitsounds_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_hit_sounds = !g_alpine_game_config.play_hit_sounds;
    ao_hitsounds_cbox.checked = g_alpine_game_config.play_hit_sounds;

    if (ao_hitsounds_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_hitsounds_cbox.checked);
}

void ao_taunts_cbox_on_click(int x, int y) {
    g_alpine_game_config.play_taunt_sounds = !g_alpine_game_config.play_taunt_sounds;
    ao_taunts_cbox.checked = g_alpine_game_config.play_taunt_sounds;

    if (ao_taunts_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_taunts_cbox.checked);
}

void ao_swapar_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_ar_controls = !g_alpine_game_config.swap_ar_controls;
    ao_swapar_cbox.checked = g_alpine_game_config.swap_ar_controls;

    if (ao_swapar_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_swapar_cbox.checked);
}

void ao_swapgn_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_gn_controls = !g_alpine_game_config.swap_gn_controls;
    ao_swapgn_cbox.checked = g_alpine_game_config.swap_gn_controls;

    if (ao_swapgn_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_swapgn_cbox.checked);
}

void ao_swapsg_cbox_on_click(int x, int y) {
    g_alpine_game_config.swap_sg_controls = !g_alpine_game_config.swap_sg_controls;
    ao_swapsg_cbox.checked = g_alpine_game_config.swap_sg_controls;

    if (ao_swapsg_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_swapsg_cbox.checked);
}

void ao_weapshake_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_disable_weapon_shake = !g_alpine_game_config.try_disable_weapon_shake;
    ao_weapshake_cbox.checked = !g_alpine_game_config.try_disable_weapon_shake;
    evaluate_restrict_disable_ss();

    if (ao_weapshake_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_weapshake_cbox.checked);
}

void ao_fullbrightchar_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_fullbright_characters = !g_alpine_game_config.try_fullbright_characters;
    ao_fullbrightchar_cbox.checked = g_alpine_game_config.try_fullbright_characters;
    evaluate_fullbright_meshes();

    if (ao_fullbrightchar_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_fullbrightchar_cbox.checked);
}

void ao_notex_cbox_on_click(int x, int y) {
    g_alpine_game_config.try_disable_textures = !g_alpine_game_config.try_disable_textures;
    ao_notex_cbox.checked = g_alpine_game_config.try_disable_textures;
    evaluate_lightmaps_only();

    if (ao_notex_cbox.checked) {
        rf::snd_play(45, 0, 0.0f, 1.0f); // on
    }
    else {
        rf::snd_play(44, 0, 0.0f, 1.0f); // off
    }

    //xlog::warn("cbox clicked {}, {}, is on? {}", x, y, ao_notex_cbox.checked);
}

void alpine_options_panel_handle_key(rf::Key* key){
    // todo: more key support (tab, etc.)
    // close panel on escape
    if (*key == rf::Key::KEY_ESC) {
        rf::ui::options_close_current_panel();
        rf::snd_play(43, 0, 0.0f, 1.0f);
        return;
    }
}

void alpine_options_panel_handle_mouse(int x, int y) {
    int hovered_index = -1;
    //xlog::warn("handling mouse {}, {}", x, y);

    // Check which gadget is being hovered over
    for (size_t i = 0; i < alpine_options_panel_settings.size(); ++i) {
        auto* gadget = alpine_options_panel_settings[i];
        if (gadget && gadget->enabled) {
            int abs_x = static_cast<int>(gadget->get_absolute_x() * rf::ui::scale_x);
            int abs_y = static_cast<int>(gadget->get_absolute_y() * rf::ui::scale_y);
            int abs_w = static_cast<int>(gadget->w * rf::ui::scale_x);
            int abs_h = static_cast<int>(gadget->h * rf::ui::scale_y);

            //xlog::warn("Checking gadget {} at ({}, {}) size ({}, {})", i, abs_x, abs_y, abs_w, abs_h);

            if (x >= abs_x && x <= abs_x + abs_w &&
                y >= abs_y && y <= abs_y + abs_h) {
                hovered_index = static_cast<int>(i);
                break;
            }
        }
    }
    //xlog::warn("hovered {}", hovered_index);
    if (hovered_index >= 0) {
        auto* gadget = alpine_options_panel_settings[hovered_index];

        if (gadget) {
            if (rf::mouse_was_button_pressed(0)) { // Left mouse button pressed
                //xlog::warn("Clicked on gadget index {}", hovered_index);

                // Call on_click if assigned
                if (gadget->on_click) {
                    gadget->on_click(x, y);
                }
            }
            else if (rf::mouse_button_is_down(0) && gadget->on_mouse_btn_down) {
                // Handle mouse button being held down
                gadget->on_mouse_btn_down(x, y);
            }
        }
    }

    // Update all gadgets
    for (auto* gadget : alpine_options_panel_settings) {
        if (gadget) {
            gadget->highlighted = false;
        }
    }

    if (hovered_index >= 0) {
        alpine_options_panel_settings[hovered_index]->highlighted = true;
    }
}

void alpine_options_panel_checkbox_init(rf::ui::Checkbox* checkbox, rf::ui::Label* label, void (*on_click)(int, int),
    bool checked, int x, int y, std::string label_text) {
    checkbox->create("checkbox.tga", "checkbox_selected.tga", "checkbox_checked.tga", x, y, 45, "", 0);
    checkbox->parent = &alpine_options_panel;
    checkbox->checked = checked;
    checkbox->on_click = on_click;
    checkbox->enabled = true;
    alpine_options_panel_settings.push_back(checkbox);

    label->create(&alpine_options_panel, x + 87, y + 5, label_text.c_str(), rf::ui::medium_font_0);
    label->enabled = true;
    alpine_options_panel_labels.push_back(label);
}

void alpine_options_panel_init() {
    alpine_options_panel.create("alpine_options_panel.tga", rf::ui::options_panel_x, rf::ui::options_panel_y);

    alpine_options_panel_checkbox_init(
        &ao_bighud_cbox, &ao_bighud_label, ao_bighud_cbox_on_click, g_alpine_game_config.big_hud, 113, 18, "Big HUD");
    alpine_options_panel_checkbox_init(
        &ao_linearpitch_cbox, &ao_linearpitch_label, ao_linearpitch_cbox_on_click, g_alpine_game_config.mouse_linear_pitch, 113, 43, "Linear pitch");
    alpine_options_panel_checkbox_init(
        &ao_ctfwh_cbox, &ao_ctfwh_label, ao_ctfwh_cbox_on_click, g_alpine_game_config.world_hud_ctf_icons, 113, 68, "CTF icons");
    alpine_options_panel_checkbox_init(
        &ao_damagenum_cbox, &ao_damagenum_label, ao_damagenum_cbox_on_click, g_alpine_game_config.world_hud_damage_numbers, 113, 93, "Hit numbers");
    alpine_options_panel_checkbox_init(
        &ao_hitsounds_cbox, &ao_hitsounds_label, ao_hitsounds_cbox_on_click, g_alpine_game_config.play_hit_sounds, 113, 118, "Hit sounds");
    alpine_options_panel_checkbox_init(
        &ao_taunts_cbox, &ao_taunts_label, ao_taunts_cbox_on_click, g_alpine_game_config.play_taunt_sounds, 113, 143, "Play taunts");

    alpine_options_panel_checkbox_init(
        &ao_swapar_cbox, &ao_swapar_label, ao_swapar_cbox_on_click, g_alpine_game_config.swap_ar_controls, 280, 18, "Swap AR binds");
    alpine_options_panel_checkbox_init(
        &ao_swapgn_cbox, &ao_swapgn_label, ao_swapgn_cbox_on_click, g_alpine_game_config.swap_gn_controls, 280, 43, "Swap GN binds");
    alpine_options_panel_checkbox_init(
        &ao_swapsg_cbox, &ao_swapsg_label, ao_swapsg_cbox_on_click, g_alpine_game_config.swap_sg_controls, 280, 68, "Swap SG binds");
    alpine_options_panel_checkbox_init(
        &ao_weapshake_cbox, &ao_weapshake_label, ao_weapshake_cbox_on_click, !g_alpine_game_config.try_disable_weapon_shake, 280, 93, "Weapon shake");
    alpine_options_panel_checkbox_init(
        &ao_notex_cbox, &ao_notex_label, ao_notex_cbox_on_click, g_alpine_game_config.try_disable_textures, 280, 118, "Lightmaps only");
    alpine_options_panel_checkbox_init(
        &ao_fullbrightchar_cbox, &ao_fullbrightchar_label, ao_fullbrightchar_cbox_on_click, g_alpine_game_config.try_fullbright_characters, 280, 143, "Fullbright models");
}

void alpine_options_panel_do_frame(int x) {
    alpine_options_panel.x = x;
    alpine_options_panel.render();

    // render labels
    for (auto* ui_label : alpine_options_panel_labels) {
        if (ui_label) {
            ui_label->render();
        }
    }

    // render dynamic elements
    for (auto* ui_element : alpine_options_panel_settings) {
        if (ui_element) {
            auto checkbox = static_cast<rf::ui::Checkbox*>(ui_element);
            if (checkbox) {
                checkbox->render();
            }
        }
    }
}

static void options_alpine_on_click() {
    //xlog::warn("Hello frozen world!");

    constexpr int alpine_options_panel_id = 4;

    if (rf::ui::options_current_panel == alpine_options_panel_id) {
        rf::ui::options_close_current_panel();
        return;
    }

    rf::ui::options_menu_tab_move_anim_speed = -rf::ui::menu_move_anim_speed;
    rf::ui::options_current_panel_id = alpine_options_panel_id;
    rf::ui::options_set_panel_open(); // Transition to new panel
}

// build alpine options button
CodeInjection options_init_build_button_patch{
    0x0044F038,
    [](auto& regs) {
        //xlog::warn("Creating new button...");
        alpine_options_btn.init();
        alpine_options_btn.create("button_more.tga", "button_selected.tga", 0, 0, 99, "ALPINE FACTION", rf::ui::medium_font_0);
        alpine_options_btn.key = 0x2E;
        alpine_options_btn.enabled = true;
    },
};

// build new gadgets array
CodeInjection options_init_build_button_array_patch{
    0x0044F051,
    [](auto& regs) {
        regs.ecx += 0x4; // realignment

        // fetch stock buttons, add to new array
        rf::ui::Gadget** old_gadgets = reinterpret_cast<rf::ui::Gadget**>(0x0063FB6C);
        for (int i = 0; i < 4; ++i) {
            new_gadgets[i] = old_gadgets[i];
        }

        new_gadgets[4] = &alpine_options_btn; // add alpine options button
        new_gadgets[5] = old_gadgets[4]; // position back button after alpine options

        alpine_options_panel_init();
    },
};

// handle button click
CodeInjection handle_options_button_click_patch{
    0x0044F337,
    [](auto& regs) {
        int index = regs.eax;
        xlog::warn("button index {} clicked", index);

        // 4 = alpine, 5 = back
        if (index == 4 || index == 5) {
            if (index == 4) {
                options_alpine_on_click();
                regs.eip = 0x0044F3D2;
            }
            if (index == 5) {
                regs.eip = 0x0044F3A8;
            }
        }
    },
};

// handle alpine options panel rendering
CodeInjection options_render_alpine_panel_patch{
    0x0044F80B,
    []() {
        int index = rf::ui::options_current_panel;
        //xlog::warn("render index {}", index);

        // render alpine options panel
        if (index == 4) {
            alpine_options_panel_do_frame(rf::ui::options_animated_offset);
        }
    },
};

// forward pressed keys to alpine options panel handler
CodeInjection options_handle_key_patch{
    0x0044F2D3,
    [](auto& regs) {
        rf::Key* key = reinterpret_cast<rf::Key*>(regs.esp + 0x4);
        int index = rf::ui::options_current_panel;

        if (index == 4) {
            alpine_options_panel_handle_key(key);
        }
    },
};

// forward mouse activity to alpine options panel handler
CodeInjection options_handle_mouse_patch{
    0x0044F609,
    [](auto& regs) {
        int x = *reinterpret_cast<int*>(regs.esp + 0x8);
        int y = *reinterpret_cast<int*>(regs.esp + 0x4);
        int index = rf::ui::options_current_panel;

        if (index == 4) {
            alpine_options_panel_handle_mouse(x, y);
        }
    },
};

// unhighlight buttons when not active
CodeInjection options_do_frame_unhighlight_buttons_patch{
    0x0044F1E1,
    [](auto& regs) {

        for (int i = 0; i < 6; ++i) {
            if (new_gadgets[i]) { // Avoid null pointer dereference
                regs.ecx = regs.esi;
                new_gadgets[i]->highlighted = false;
                regs.esi += 0x44;
            }
        }

        //regs.eip = 0x0044F1F8;

    },
};

// render alpine options button
CodeInjection options_render_alpine_options_button_patch{
    0x0044F879,
    [](auto& regs) {
        // Loop through all options buttons and render them dynamically
        for (int i = 0; i < 6; ++i) {
            if (new_gadgets[i]) { // ensure the gadget pointer is valid
                rf::ui::Button* button = static_cast<rf::ui::Button*>(new_gadgets[i]);
                if (button) { // ensure the button pointer is valid
                    button->x = static_cast<int>(rf::ui::g_fOptionsMenuOffset);
                    button->y = rf::ui::g_MenuMainButtonsY + (i * rf::ui::menu_button_offset_y);
                    button->render(); // Render the button
                }
            }
        }

        regs.eip = 0x0044F8A3; // skip stock rendering loop
    },
};

// highlight options buttons with mouse
CodeInjection options_do_frame_highlight_buttons_patch{
    0x0044F233,
    [](auto& regs) {
        int index = regs.ecx;
        int new_index = index / 16;
        //xlog::warn("adding4  index {}", new_index);
        new_gadgets[new_index]->highlighted = true;
        regs.eip = 0x0044F23F;
    },
};

void ui_apply_patch()
{
    // Alpine Faction options button and panel
    options_init_build_button_patch.install();
    options_init_build_button_array_patch.install();
    handle_options_button_click_patch.install();
    options_render_alpine_panel_patch.install();
    options_render_alpine_options_button_patch.install();
    options_do_frame_highlight_buttons_patch.install();
    options_do_frame_unhighlight_buttons_patch.install();
    options_handle_key_patch.install();
    options_handle_mouse_patch.install();
    AsmWriter{0x0044F550}.push(6); // num buttons in options menu
    AsmWriter{0x0044F552}.push(&new_gadgets); // support mouseover for alpine options button
    AsmWriter{0x0044F285}.push(5); // back button index, used when hitting esc in options menu

    // Sharp UI text
#if SHARP_UI_TEXT
    UiButton_create_hook.install();
    UiButton_set_text_hook.install();
    UiButton_render_hook.install();
    UiLabel_create_hook.install();
    UiLabel_create2_hook.install();
    UiLabel_set_text_hook.install();
    UiLabel_render_hook.install();
    UiInputBox_create_hook.install();
    UiInputBox_render_hook.install();
    UiCycler_add_item_hook.install();
    UiCycler_render_hook.install();
    popup_set_text_gr_split_str_hook.install();
#endif

    // Init
    menu_init_hook.install();

    // Handle CTRL+V in input boxes
    UiInputBox_process_key_hook.install();
}

void ui_get_string_size(int* w, int* h, const char* s, int s_len, int font_num)
{
    rf::gr::get_string_size(w, h, s, s_len, font_num);
#if SHARP_UI_TEXT
    *w = static_cast<int>(*w / rf::ui::scale_x);
    *h = static_cast<int>(*h / rf::ui::scale_y);
#endif
}
