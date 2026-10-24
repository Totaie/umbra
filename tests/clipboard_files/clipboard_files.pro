QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_files
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../clipboard_shortcut_mode

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardfiles.cpp \
    ../../app/streaming/clipboardipc.cpp \
    ../../app/streaming/clipboardsync.cpp

HEADERS += \
    ../../app/streaming/clipboardfiles.h \
    ../../app/streaming/clipboardipc.h \
    ../../app/streaming/clipboardlogging.h \
    ../../app/streaming/clipboardsync.h
