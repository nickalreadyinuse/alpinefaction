set(SRCS
    AboutDlg.cpp
    AboutDlg.h
    CMakeLists.txt
    DashFactionLauncher.rc
    faction_files.h
    faction_files.cpp
    ImageButton.h
    ImageButton.cpp
    LauncherApp.cpp
    LauncherApp.h
    LauncherCommandLineInfo.h
    main.cpp
    MainDlg.cpp
    MainDlg.h
    OptionsDlg.cpp
    OptionsDlg.h
    OptionsDisplayDlg.cpp
    OptionsDisplayDlg.h
    OptionsGraphicsDlg.cpp
    OptionsGraphicsDlg.h
    OptionsMiscDlg.cpp
    OptionsMiscDlg.h
    OptionsAudioDlg.cpp
    OptionsAudioDlg.h
    OptionsMultiplayerDlg.cpp
    OptionsMultiplayerDlg.h
    OptionsInputDlg.cpp
    OptionsInputDlg.h
    OptionsInterfaceDlg.cpp
    OptionsInterfaceDlg.h
    res
    resource.h
    stdafx.cpp
    stdafx.h
    targetver.h
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_executable(AlpineFactionLauncher WIN32 ${SRCS})

target_compile_features(AlpineFactionLauncher PUBLIC cxx_std_20)
set_target_properties(AlpineFactionLauncher PROPERTIES CXX_EXTENSIONS NO)
enable_warnings(AlpineFactionLauncher)
setup_debug_info(AlpineFactionLauncher)

target_compile_definitions(AlpineFactionLauncher PRIVATE XLOG_NO_MACROS)

target_include_directories(AlpineFactionLauncher SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
    ${CMAKE_SOURCE_DIR}/vendor/win32xx
    ${CMAKE_SOURCE_DIR}/vendor/zlib
    ${CMAKE_SOURCE_DIR}/vendor/zlib/contrib/minizip
)

target_link_libraries(AlpineFactionLauncher
    LauncherCommon
    Xlog
    zlib
    unzip
    CrashHandlerStub
    comctl32
)
