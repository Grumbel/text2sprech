# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

# Source of truth: top-level VERSION file (e.g. "0.1.0-dev"), or
# -DPROJECT_VERSION_FULL=... from packaging (Nix flake appends +g<rev>).
if(NOT DEFINED PROJECT_VERSION_FULL)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_FULL LIMIT_COUNT 1)
endif()
# CMake's project(VERSION) wants numeric major.minor.patch; strip any
# -dev / +git suffix for that only. The full string is what the app shows.
string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" PROJECT_VERSION_CMAKE
                     "${PROJECT_VERSION_FULL}")

project(text2sprech
  VERSION "${PROJECT_VERSION_CMAKE}"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia Network)

add_executable(text2sprech
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/DocumentView.cpp
    src/ui/DocumentView.h
    src/ui/PreferencesDialog.cpp
    src/ui/PreferencesDialog.h
    src/ui/ExportAudioDialog.cpp
    src/ui/ExportAudioDialog.h
    src/document/SentenceSplitter.cpp
    src/document/SentenceSplitter.h
    src/document/Sentence.h
    src/document/HtmlSanitizer.cpp
    src/document/HtmlSanitizer.h
    src/speech/PiperClient.cpp
    src/speech/PiperClient.h
    src/speech/PiperServerManager.cpp
    src/speech/PiperServerManager.h
    src/speech/PlaybackController.cpp
    src/speech/PlaybackController.h
    resources/text2sprech.qrc
)

target_include_directories(text2sprech PRIVATE src)

# Dev-tree convenience: if no `piper-server` executable is on PATH (i.e. not
# using the flake-packaged setup), PiperServerManager falls back to running
# this script directly with python3. Not meaningful once installed/packaged
# elsewhere -- that's what the flake's qtWrapperArgs PATH injection is for.
target_compile_definitions(text2sprech PRIVATE
    PIPER_SERVER_SCRIPT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/piper_server/server.py"
    TEXT2SPRECH_VERSION="${PROJECT_VERSION_FULL}"
)

target_link_libraries(text2sprech PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
)

install(TARGETS text2sprech RUNTIME DESTINATION bin)

# Python piper-server modules for non-Nix installs (PATH still preferred when
# a `piper-server` wrapper exists). Resolved at runtime relative to the binary.
install(DIRECTORY piper_server/
  DESTINATION share/text2sprech/piper_server
  FILES_MATCHING
    PATTERN "*.py"
  PATTERN "__pycache__" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
  PATTERN "tail2sprech.py"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# tail2sprech is a script living next to the modules it imports; Python
# resolves the symlink when it sets up sys.path, so those imports still work.
file(CREATE_LINK ../share/text2sprech/piper_server/tail2sprech.py
  "${CMAKE_CURRENT_BINARY_DIR}/tail2sprech" SYMBOLIC)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tail2sprech" DESTINATION bin)

# Desktop integration: app menu entry + icon theme lookup. The icon is also
# embedded into the binary itself via resources/text2sprech.qrc (AUTORCC)
# for cases where icon-theme lookup doesn't apply, e.g. running the binary
# directly during development rather than through a desktop launcher.
install(FILES
    resources/text2sprech.desktop
    resources/text2sprech-clipboard.desktop
  DESTINATION share/applications)

# AppStream metainfo: version comes from PROJECT_VERSION_FULL (VERSION file
# or -DPROJECT_VERSION_FULL from the flake), not a hardcoded string in git.
string(TIMESTAMP PROJECT_RELEASE_DATE "%Y-%m-%d" UTC)
if(PROJECT_VERSION_FULL MATCHES "-dev")
  set(PROJECT_RELEASE_TYPE "development")
  set(PROJECT_RELEASE_DESCRIPTION
      "Development snapshot (see top-level VERSION).")
else()
  set(PROJECT_RELEASE_TYPE "stable")
  set(PROJECT_RELEASE_DESCRIPTION
      "Release ${PROJECT_VERSION_FULL}.")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/text2sprech.metainfo.xml.in"
  "${CMAKE_CURRENT_BINARY_DIR}/text2sprech.metainfo.xml"
  @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/text2sprech.metainfo.xml"
  DESTINATION share/metainfo)

# hicolor icons at several sizes (generated from resources/text2sprech.png).
# The 512px original is also embedded in the binary via text2sprech.qrc.
foreach(_icon_size IN ITEMS 16 24 32 48 64 128 256 512)
  install(FILES "resources/icons/text2sprech-${_icon_size}.png"
    DESTINATION "share/icons/hicolor/${_icon_size}x${_icon_size}/apps"
    RENAME text2sprech.png)
endforeach()

# --- Tests (optional, only built if Qt Test is found) ---
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    enable_testing()
    add_executable(test_sentence_splitter
        tests/test_sentence_splitter.cpp
        src/document/SentenceSplitter.cpp
        src/document/SentenceSplitter.h
        src/document/Sentence.h
    )
    target_include_directories(test_sentence_splitter PRIVATE src)
    target_link_libraries(test_sentence_splitter PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME test_sentence_splitter COMMAND test_sentence_splitter)
    add_test(NAME test_tail2sprech
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tail2sprech.py")
endif()
