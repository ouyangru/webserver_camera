include $(TOPDIR)/rules.mk

PKG_NAME:=webserver_camera
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SUBMENU:=Vision
  SECTION:=allwinner
  CATEGORY:=Allwinner
  MENU:=1
  TITLE:=epoll HTTP MJPEG and HTTP-FLV camera server
  DEPENDS:=+libstdcpp
endef

define Package/$(PKG_NAME)/description
  A camera web server based on epoll and a thread pool. It provides
  HTTP-MJPEG, HTTP-FLV, control APIs, and a browser dashboard.
endef

# Usage in Tina/OpenWrt SDK:
#   1. Create package/allwinner/webserver_camera/
#   2. Copy this file to package/allwinner/webserver_camera/Makefile
#   3. Copy this repository into package/allwinner/webserver_camera/src/
#      so src/makefile, src/src/, src/include/, and src/resources/ exist.
#   4. Select the package in menuconfig, then rebuild and pack the image.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) -r ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/ \
		ARCH="$(TARGET_ARCH)" \
		AR="$(TARGET_AR)" \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -O2" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -Wall -O2" \
		LDFLAGS="$(TARGET_LDFLAGS) -lpthread -lstdc++" \
		ENABLE_GSTREAMER=0 \
		TARGET_NAME="$(PKG_NAME)" \
		INSTALL_NAME="$(PKG_NAME)" \
		all
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/$(PKG_NAME) $(1)/usr/bin/$(PKG_NAME)
	$(INSTALL_DIR) $(1)/usr/share/webserver_camera/
	$(CP) $(PKG_BUILD_DIR)/resources/* $(1)/usr/share/webserver_camera/
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
