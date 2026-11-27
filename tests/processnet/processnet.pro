# Unit tests for the per-process network sampler (sock_diag parsing and math).
# Build and run:  qmake6 tests/processnet/processnet.pro && make && ./processnet_test
QT       += core testlib
QT       -= gui
CONFIG   += console testcase c++17
CONFIG   -= app_bundle
TARGET    = processnet_test
TEMPLATE  = app
INCLUDEPATH += ../../src

SOURCES  += processnet_test.cpp \
            ../../src/os/processnet.cpp \
            ../../src/logger.cpp
HEADERS  += ../../src/os/processnet.h \
            ../../src/os/process.h \
            ../../src/logger.h
