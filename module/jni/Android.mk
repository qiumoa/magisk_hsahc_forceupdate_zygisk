LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := hsahc_zygisk
LOCAL_SRC_FILES := hsahc_module.cpp
LOCAL_LDLIBS := -llog -ldl -lstdc++
include $(BUILD_SHARED_LIBRARY)
