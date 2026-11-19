LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := gpuspoofer
LOCAL_SRC_FILES := main.cpp
LOCAL_CPPFLAGS := -std=c++17 -O2 -Wall
LOCAL_LDLIBS := -llog -lGLESv2
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384
include $(BUILD_SHARED_LIBRARY)
