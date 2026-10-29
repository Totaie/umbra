QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = span_screens
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/spanscreens.cpp

HEADERS += \
    ../../app/streaming/spanscreens.h
