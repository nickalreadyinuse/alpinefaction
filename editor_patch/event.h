#pragma once


struct AlpineEventDialog
{
    std::string script_name_field;
    std::string delay_field;
    int int1_field;
    int int2_field;
    std::string float1_field;
    std::string float2_field;
    bool bool1_field;
    bool bool2_field;
    std::string str1_field;
    std::string str2_field;
};

// level editor alpine event IDs, separate from EventType in game
enum class AlpineDedEventID : int
{
    SetVar = 89,
    Clone_Entity,
    Set_Player_World_Collide,
    Switch_Random,
    Difficulty_Gate,
    HUD_Message,
    Play_Video,
    Set_Level_Hardness,
    Sequence,
    Clear_Queued,
    Remove_Link,
    Fixed_Delay,
    Add_Link,
    Valid_Gate,
    Goal_Math,
    Goal_Gate,
    Environment_Gate,
    Inside_Gate,
    Anchor_Marker,
    Force_Unhide,
    Set_Difficulty,
    Set_Fog_Far_Clip,
    AF_When_Dead,
    Gametype_Gate,
    When_Picked_Up
};

constexpr int af_ded_event_to_int(AlpineDedEventID event_id) noexcept
{
    return static_cast<int>(event_id);
}

constexpr AlpineDedEventID int_to_af_ded_event(int event_id) noexcept
{
    return static_cast<AlpineDedEventID>(event_id);
}
