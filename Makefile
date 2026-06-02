include $(TOPDIR)/rules.mk

PKG_NAME:=webserver_camera
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_BUILD_DIR := $(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
	SUBMENU:=Vision
	SECTION:=allwinner
	CATEGORY:=Allwinner
	TITLE:=webserver camera demo (HLS + RTSP)
	DEPENDS:=+libpthread +libstdcpp +gstreamer1 +gst1-rtsp-server +gst1-plugins-base +gst1-plugins-good +gst1-plugins-bad
endef

define Package/$(PKG_NAME)/description
	HTTP server with HLS streaming and a GStreamer RTSP server.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(CP) ./include $(PKG_BUILD_DIR)/
	$(CP) ./makefile $(PKG_BUILD_DIR)/Makefile
	$(CP) ./resources $(PKG_BUILD_DIR)/
	$(CP) ./scripts $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		ARCH="$(TARGET_ARCH)" \
		AR="$(TARGET_AR)" \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		all
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/my_program $(1)/usr/bin/webserver_camera

	$(INSTALL_DIR) $(1)/usr/share/webserver_camera
	$(CP) $(PKG_BUILD_DIR)/resources $(1)/usr/share/webserver_camera/
	$(CP) $(PKG_BUILD_DIR)/scripts $(1)/usr/share/webserver_camera/
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
