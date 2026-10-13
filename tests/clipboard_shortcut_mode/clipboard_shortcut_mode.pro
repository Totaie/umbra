QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_shortcut_mode
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardipc.cpp \
    ../../app/streaming/clipboardsync.cpp

HEADERS += \
    ../../app/streaming/clipboardipc.h \
    ../../app/streaming/clipboardlogging.h \
    ../../app/streaming/clipboardsync.h \
    testhostidentity.h
