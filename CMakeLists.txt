cmake_minimum_required(VERSION 3.21)
project(reader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia Network)

add_executable(reader
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/DocumentView.cpp
    src/ui/DocumentView.h
    src/document/SentenceSplitter.cpp
    src/document/SentenceSplitter.h
    src/document/Sentence.h
    src/speech/PiperClient.cpp
    src/speech/PiperClient.h
    src/speech/PiperServerManager.cpp
    src/speech/PiperServerManager.h
    src/speech/PlaybackController.cpp
    src/speech/PlaybackController.h
)

target_include_directories(reader PRIVATE src)

# Dev-tree convenience: if no `piper-server` executable is on PATH (i.e. not
# using the flake-packaged setup), PiperServerManager falls back to running
# this script directly with python3. Not meaningful once installed/packaged
# elsewhere -- that's what the flake's qtWrapperArgs PATH injection is for.
target_compile_definitions(reader PRIVATE
    PIPER_SERVER_SCRIPT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/piper_server/server.py"
)

target_link_libraries(reader PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
)

install(TARGETS reader RUNTIME DESTINATION bin)

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
endif()
