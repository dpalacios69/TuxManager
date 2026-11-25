# Unit tests for the per-process GPU usage sampler (DRM fdinfo parsing and math).
# Build and run:  qmake6 tests/processgpu/processgpu.pro && make && ./processgpu_test
QT       += core testlib
QT       -= gui
CONFIG   += console testcase c++17
CONFIG   -= app_bundle
TARGET    = processgpu_test
TEMPLATE  = app
INCLUDEPATH += ../../src
DEFINES  += FIXTURES_DIR=\\\"$$PWD/../fixtures/drm\\\"
LIBS     += -ldl

SOURCES  += processgpu_test.cpp \
            ../../src/os/processgpu.cpp \
            ../../src/logger.cpp
HEADERS  += ../../src/os/processgpu.h \
            ../../src/os/process.h \
            ../../src/logger.h
