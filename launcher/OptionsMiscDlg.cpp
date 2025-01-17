#include <wxx_wincore.h>
#include "OptionsMiscDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsMiscDlg::OptionsMiscDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_MISC), m_conf(conf)
{
}

BOOL OptionsMiscDlg::OnInitDialog()
{
    InitToolTip();

    CheckDlgButton(IDC_FAST_START_CHECK, m_conf.fast_start ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_ALLOW_OVERWRITE_GAME_CHECK, m_conf.allow_overwrite_game_files ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_REDUCED_SPEED_IN_BG_CHECK, m_conf.reduced_speed_in_background ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_PLAYER_JOIN_BEEP_CHECK, m_conf.player_join_beep ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_AUTOSAVE_CHECK, m_conf.autosave ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_ALPINE_BRAND_CHECK, m_conf.af_branding ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_FULLBRIGHT, m_conf.try_mesh_fullbright ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_LIGHTMAPS, m_conf.try_lightmaps_only ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_MUZZLEFLASH, m_conf.try_disable_muzzle_flash ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SCREENSHAKE, m_conf.try_disable_screenshake ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_DAMAGEFLASH, m_conf.damage_screen_flash ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_POW2TEX, m_conf.pow2tex ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

void OptionsMiscDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_FAST_START_CHECK), "Skip game intro videos and go straight to main menu");
    m_tool_tip.AddTool(GetDlgItem(IDC_ALLOW_OVERWRITE_GAME_CHECK), "Allows files in custom level folders to override core game files. Recommended to keep disabled (client_mods should be used for this instead)");
    m_tool_tip.AddTool(GetDlgItem(IDC_REDUCED_SPEED_IN_BG_CHECK), "Run the game at reduced speed when it doesn't have focus");
    m_tool_tip.AddTool(GetDlgItem(IDC_PLAYER_JOIN_BEEP_CHECK), "Play a beep when a player joins the server you are in while your game doesn't have focus");
    m_tool_tip.AddTool(GetDlgItem(IDC_AUTOSAVE_CHECK), "Automatically save the game after a level transition");
    m_tool_tip.AddTool(GetDlgItem(IDC_ALPINE_BRAND_CHECK), "Display Alpine Faction branding where available");
    m_tool_tip.AddTool(GetDlgItem(IDC_FULLBRIGHT), "Use fully bright lighting for all character meshes (in multiplayer, only applies if the server allows it)");
    m_tool_tip.AddTool(GetDlgItem(IDC_LIGHTMAPS), "Render levels using lightmaps only for increased visibility (in multiplayer, only applies if the server allows it)");
    m_tool_tip.AddTool(GetDlgItem(IDC_MUZZLEFLASH), "Disable blinking dynamic lights when players are shooting (in multiplayer, only applies if the server allows it)");
    m_tool_tip.AddTool(GetDlgItem(IDC_SCREENSHAKE), "Disable camera shake including when firing weapons (in multiplayer, only applies if the server allows it)");
    m_tool_tip.AddTool(GetDlgItem(IDC_DAMAGEFLASH), "Show a bright red screen flash when you suffer damage");
    m_tool_tip.AddTool(GetDlgItem(IDC_POW2TEX), "Enforce for textures with nonstandard resolutions, fixes textures in older levels but may cause issues with textures in newer ones");
}

void OptionsMiscDlg::OnSave()
{
    m_conf.fast_start = (IsDlgButtonChecked(IDC_FAST_START_CHECK) == BST_CHECKED);
    m_conf.allow_overwrite_game_files = (IsDlgButtonChecked(IDC_ALLOW_OVERWRITE_GAME_CHECK) == BST_CHECKED);
    m_conf.reduced_speed_in_background = (IsDlgButtonChecked(IDC_REDUCED_SPEED_IN_BG_CHECK) == BST_CHECKED);
    m_conf.player_join_beep = (IsDlgButtonChecked(IDC_PLAYER_JOIN_BEEP_CHECK) == BST_CHECKED);
    m_conf.autosave = (IsDlgButtonChecked(IDC_AUTOSAVE_CHECK) == BST_CHECKED);
    m_conf.af_branding = (IsDlgButtonChecked(IDC_ALPINE_BRAND_CHECK) == BST_CHECKED);
    m_conf.try_mesh_fullbright = (IsDlgButtonChecked(IDC_FULLBRIGHT) == BST_CHECKED);
    m_conf.try_lightmaps_only = (IsDlgButtonChecked(IDC_LIGHTMAPS) == BST_CHECKED);
    m_conf.try_disable_muzzle_flash = (IsDlgButtonChecked(IDC_MUZZLEFLASH) == BST_CHECKED);
    m_conf.try_disable_screenshake = (IsDlgButtonChecked(IDC_SCREENSHAKE) == BST_CHECKED);
    m_conf.damage_screen_flash = (IsDlgButtonChecked(IDC_DAMAGEFLASH) == BST_CHECKED);
    m_conf.pow2tex = (IsDlgButtonChecked(IDC_POW2TEX) == BST_CHECKED);
}
