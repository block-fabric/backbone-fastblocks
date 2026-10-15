# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# chains-app: the node with its HTML interface in a native web view (webview/webview), no Qt.

# The hooks of the end-to-end tests (-webuitestscript, -webuitestshots and the test.* methods of the bridge,
# which answer the dialogs of the desktop): only in a build for the tests, never in a release.
option(WEBUI_TEST_HOOKS "Build chains-app with the hooks of its end-to-end tests (never for a release)." OFF)

set(WEBUI_PREBUILT_DIST "" CACHE PATH "Use this already built interface (the output of `npm run build` in src/webui/app) instead of building it with npm.")

# The interface: built with npm (Svelte, TypeScript, Vite), then embedded into the program.
set(webui_app_dir ${CMAKE_CURRENT_SOURCE_DIR}/app)
if(WEBUI_PREBUILT_DIST)
  set(webui_dist ${WEBUI_PREBUILT_DIST})
  file(GLOB_RECURSE webui_dist_files CONFIGURE_DEPENDS ${webui_dist}/*)
  set(webui_dist_stamp ${webui_dist_files})
else()
  find_program(NPM_EXECUTABLE npm REQUIRED)
  set(webui_dist ${CMAKE_CURRENT_BINARY_DIR}/ui-dist)
  file(GLOB_RECURSE webui_app_sources CONFIGURE_DEPENDS
    ${webui_app_dir}/src/*
    ${webui_app_dir}/public/*
    ${CMAKE_CURRENT_SOURCE_DIR}/locale/*.ts
  )
  list(FILTER webui_app_sources EXCLUDE REGEX "\\.test\\.ts$")
  add_custom_command(
    OUTPUT ${webui_dist}/index.html
    COMMAND ${CMAKE_COMMAND} -DNPM=${NPM_EXECUTABLE} -DAPP_DIR=${webui_app_dir} -DOUT_DIR=${webui_dist} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/npm_build.cmake
    DEPENDS
      ${webui_app_sources}
      ${webui_app_dir}/index.html
      ${webui_app_dir}/package.json
      ${webui_app_dir}/package-lock.json
      ${webui_app_dir}/vite.config.ts
      ${webui_app_dir}/tsconfig.json
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/npm_build.cmake
    COMMENT "Building the interface of chains-app (npm)"
    VERBATIM
  )
  set(webui_dist_stamp ${webui_dist}/index.html)
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp
  COMMAND ${CMAKE_COMMAND} -DDIST_DIR=${webui_dist} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${webui_dist_stamp} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding the interface of chains-app"
  VERBATIM
)

add_library(chains_webui_assets STATIC EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp
)
target_link_libraries(chains_webui_assets PRIVATE core_interface)

# What does not need a window: the bridge, the events, the settings of the app, the assets.
add_library(chains_webui_core STATIC EXCLUDE_FROM_ALL
  appconfig.cpp
  assets.cpp
  bridge.cpp
  rpcpolicy.cpp
  console.cpp
  events.cpp
  i18n.cpp
  icontint.cpp
  nodeapi.cpp
  nodeevents.cpp
  options.cpp
  sidechainnodes.cpp
  singleinstance.cpp
  windowframe.cpp
)
target_link_libraries(chains_webui_core
  PUBLIC
    core_interface
    bitcoin_cli
    bitcoin_common
    bitcoin_util
    univalue
    Boost::headers
)
if(WIN32)
  # The named pipe of the single instance, the link of the startup folder.
  target_link_libraries(chains_webui_core PUBLIC advapi32 ole32 shell32 uuid)
endif()

# The wallet functions of the page (the wallet pages of chains-qt), in a build with wallets.
if(ENABLE_WALLET)
  add_library(chains_webui_wallet STATIC EXCLUDE_FROM_ALL
    walletapi.cpp
  )
  target_link_libraries(chains_webui_wallet
    PUBLIC
      core_interface
      chains_webui_core
      bitcoin_wallet
      bitcoin_node
  )
endif()

if(WIN32)
  # WebView2: its headers (the depends package webview2, or WEBVIEW2_INCLUDE_DIR); the loader is the
  # one built into webview.h, so no WebView2Loader.dll.
  find_path(WEBVIEW2_INCLUDE_DIR WebView2.h PATH_SUFFIXES webview2 REQUIRED)
  set(webui_window window_win32.cpp urischeme_win.cpp res/chains-app-res.rc)
  # The manifest: per monitor DPI awareness (v2), UTF-8, Windows 8 to 11, common controls 6.
  configure_file(res/chains-app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/chains-app.manifest USE_SOURCE_PERMISSIONS)
  file(CONFIGURE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/chains-app-manifest.rc
    CONTENT "1 /* CREATEPROCESS_MANIFEST_RESOURCE_ID */ 24 /* RT_MANIFEST */ \"${CMAKE_CURRENT_BINARY_DIR}/chains-app.manifest\""
  )
  list(APPEND webui_window ${CMAKE_CURRENT_BINARY_DIR}/chains-app-manifest.rc)
elseif(APPLE)
  set(webui_window window_generic.cpp)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
  set(webui_window window_gtk.cpp tray_gtk.cpp)
endif()

add_executable(chains-app WIN32
  main.cpp
  app.cpp
  ${webui_window}
  ../init/chains-app.cpp
)
if(WEBUI_TEST_HOOKS)
  target_compile_definitions(chains-app PRIVATE WEBUI_TEST_HOOKS)
endif()
# The vendored webview header is not ours: no warnings from it.
target_include_directories(chains-app SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/webview)
target_link_libraries(chains-app
  core_interface
  chains_webui_core
  chains_webui_assets
  $<TARGET_NAME_IF_EXISTS:chains_webui_wallet>
  bitcoin_node
  $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  $<TARGET_NAME_IF_EXISTS:PkgConfig::WEBKITGTK>
)
if(WIN32)
  target_include_directories(chains-app SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIR})
  target_compile_definitions(chains-app PRIVATE WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK=1)
  target_link_libraries(chains-app advapi32 dwmapi gdi32 ole32 shell32 shlwapi user32 uuid version)
elseif(APPLE)
  target_link_libraries(chains-app "-framework WebKit" "-framework Cocoa")
endif()
install_binary_component(chains-app)
if(NOT WIN32 AND NOT APPLE)
  # The desktop entry: the app in the menus of the desktop, and the handler of chains: links (BIP 21).
  install(FILES ${PROJECT_SOURCE_DIR}/share/chains-app.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    COMPONENT chains-app
  )
  install(FILES ${PROJECT_SOURCE_DIR}/share/pixmaps/bitcoin256.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    RENAME chains.png
    COMPONENT chains-app
  )
endif()

# The release (target deploy-app), with chains-app and the programs of the node, in release-app of the
# build: on Windows a zip and an installer (NSIS), on Linux a tarball. Never with the test hooks.
set(webui_release ${PROJECT_BINARY_DIR}/release-app)
if(WEBUI_TEST_HOOKS)
  set(webui_no_test_hooks COMMAND ${CMAKE_COMMAND} -E echo "deploy-app: this build has the test hooks (WEBUI_TEST_HOOKS=ON); a release is made with them OFF"
                          COMMAND ${CMAKE_COMMAND} -E false)
endif()
if(MINGW AND TARGET bitcoind AND TARGET bitcoin-cli AND TARGET bitcoin-tx AND TARGET bitcoin-wallet AND TARGET bitcoin-util)
  set(webui_stem chains-app-${CLIENT_VERSION_STRING}-win64)
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(BIN_DIR ${webui_release}/bin)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  configure_file(res/setup-app.nsi.in ${webui_release}/chains-app-setup.nsi @ONLY)
  add_custom_target(deploy-app
    ${webui_no_test_hooks}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${webui_release}/bin
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:chains-app> -o ${webui_release}/bin/chains-app.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoind> -o ${webui_release}/bin/chainsd.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-cli> -o ${webui_release}/bin/chains-cli.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-tx> -o ${webui_release}/bin/chains-tx.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-wallet> -o ${webui_release}/bin/chains-wallet.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-util> -o ${webui_release}/bin/chains-util.exe
    COMMAND ${CMAKE_COMMAND} -DBIN_DIR=${webui_release}/bin -DOUT_DIR=${webui_release} -DSRC_DIR=${PROJECT_SOURCE_DIR}
      -DNSI=${webui_release}/chains-app-setup.nsi -DSTEM=${webui_stem} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_windows.cmake
    DEPENDS chains-app bitcoind bitcoin-cli bitcoin-tx bitcoin-wallet bitcoin-util
    COMMENT "The Windows release of chains-app: ${webui_release}/${webui_stem}.zip and -setup.exe"
    VERBATIM
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET bitcoind AND TARGET bitcoin-cli)
  set(webui_stem chains-app-${CLIENT_VERSION_STRING}-${CMAKE_SYSTEM_PROCESSOR}-linux-gnu)
  set(webui_components chains-app)
  foreach(program bitcoind bitcoin-cli bitcoin-tx bitcoin-wallet bitcoin-util)
    if(TARGET ${program})
      list(APPEND webui_components ${program})
    endif()
  endforeach()
  string(REPLACE ";" "," webui_components_arg "${webui_components}")
  add_custom_target(deploy-app
    ${webui_no_test_hooks}
    COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${PROJECT_BINARY_DIR} -DOUT_DIR=${webui_release} -DSRC_DIR=${PROJECT_SOURCE_DIR}
      -DSTEM=${webui_stem} -DCOMPONENTS=${webui_components_arg} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_linux.cmake
    DEPENDS ${webui_components}
    COMMENT "The Linux release of chains-app: ${webui_release}/${webui_stem}.tar.gz"
    VERBATIM
  )
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()
