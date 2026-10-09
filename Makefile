ARCHS = arm64
TARGET = iphone:clang:16.5:15.0
THEOS_PACKAGE_SCHEME = rootless

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = VCamLiveBridge
VCamLiveBridge_FILES = Tweak.c
VCamLiveBridge_CFLAGS = -std=c11 -O2 -Wall -Wextra
VCamLiveBridge_LDFLAGS = -Wl,-undefined,dynamic_lookup

include $(THEOS_MAKE_PATH)/tweak.mk
