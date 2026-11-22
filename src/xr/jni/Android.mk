LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := xr_loader
LOCAL_SRC_FILES := xr_core.c xr_swap.c xr_display.c xr_pose.c xr_jni.c slam_probe.c hidprobe.c ctrlprobe.c controller.c hvr_bridge.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include
LOCAL_CFLAGS := -O2 -fPIC -Wall -Wno-unused-parameter -std=c11
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -ldl -lm
LOCAL_LDFLAGS := -Wl,--hash-style=both
include $(BUILD_SHARED_LIBRARY)
