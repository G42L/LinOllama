cmake_minimum_required(VERSION 3.20)
project(linollama LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Multimedia Svg WebEngineWidgets WebChannel)

add_executable(linollama
    src/main.cpp
    src/TrayApplication.cpp
    src/TrayApplication.h
    src/SystemMonitor.cpp
    src/SystemMonitor.h
    src/OllamaClient.cpp
    src/OllamaClient.h
    src/ChatQueue.cpp
    src/ChatQueue.h
    src/WebSearchClient.cpp
    src/WebSearchClient.h
    src/StackOverflowSearchClient.cpp
    src/StackOverflowSearchClient.h
    src/VoiceRecorder.cpp
    src/VoiceRecorder.h
    src/WhisperManager.cpp
    src/WhisperManager.h
    src/MapEmbedWidget.cpp
    src/MapEmbedWidget.h
    src/HtmlEmbedWidget.cpp
    src/HtmlEmbedWidget.h
    src/ServerController.cpp
    src/ServerController.h
    src/Conversation.h
    src/ConversationStore.cpp
    src/ConversationStore.h
    src/ConversationListItemWidget.cpp
    src/ConversationListItemWidget.h
    src/ChatInputEdit.cpp
    src/ChatInputEdit.h
    src/AutoHeightTextBrowser.cpp
    src/AutoHeightTextBrowser.h
    src/ThinkingSectionWidget.cpp
    src/ThinkingSectionWidget.h
    src/ToolCallSectionWidget.cpp
    src/ToolCallSectionWidget.h
    src/BuiltinTools.cpp
    src/BuiltinTools.h
    src/ToolExecutor.cpp
    src/ToolExecutor.h
    src/ChatWidget.cpp
    src/ChatWidget.h
    src/StatsStripWidget.cpp
    src/StatsStripWidget.h
    src/Theme.cpp
    src/Theme.h
    src/ThemeManager.cpp
    src/ThemeManager.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/resources.qrc
)

target_link_libraries(linollama
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        Qt6::Multimedia
        Qt6::Svg
        Qt6::WebEngineWidgets
        Qt6::WebChannel
        ${CMAKE_DL_LIBS}
)

target_include_directories(linollama PRIVATE src)

install(TARGETS linollama RUNTIME DESTINATION bin)
