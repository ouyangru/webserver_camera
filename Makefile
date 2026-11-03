include $(TOPDIR)/rules.mk

PKG_NAME:=webserver_camera
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)
  SUBMENU:=Vision
  SECTION:=allwinner
  CATEGORY:=Allwinner
  MENU:=1
  TITLE:=epoll HTTP MJPEG and HTTP-FLV camera server
  DEPENDS:=+libstdcpp +libjpeg-turbo +libAWIspApi
endef

define Package/$(PKG_NAME)/description
  A camera web server based on epoll and a thread pool. It provides
  HTTP-MJPEG, HTTP-FLV, control APIs, and a browser dashboard.
endef

# The repository itself is the Tina package directory.  Copy only source
# inputs into BUILD_DIR so stale host objects/binaries can never be reused.
define Build/Prepare
	rm -rf $(PKG_BUILD_DIR)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./app.mk $(PKG_BUILD_DIR)/Makefile
	$(CP) -r ./src ./include ./resources $(PKG_BUILD_DIR)/
	$(CP) -r ./vendor ./scripts $(PKG_BUILD_DIR)/
	$(CP) -r ./tools $(PKG_BUILD_DIR)/
endef

define Build/Compile
	TINA_SDK_ROOT="$(LICHEE_TOP_DIR)" V853_CC="$(TARGET_CC)" \
		V853_STAGING_DIR="$(STAGING_DIR)" bash $(PKG_BUILD_DIR)/scripts/build-cedar-runtime.sh
	$(MAKE) -C $(PKG_BUILD_DIR) \
		ARCH="$(TARGET_ARCH)" \
		AR="$(TARGET_AR)" \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		STRIP="$(TARGET_STRIP)" \
		CPPFLAGS="$(TARGET_CPPFLAGS) -I$(STAGING_DIR)/usr/include" \
		CFLAGS="$(TARGET_CFLAGS)" \
		CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib" \
		ENABLE_GSTREAMER=0 \
		ENABLE_JPEG=1 \
		ENABLE_CEDAR=1 \
		CEDARC_ROOT="$(PKG_BUILD_DIR)/vendor/cedarc-v853" \
		CEDARC_LIBDIR="$(PKG_BUILD_DIR)/build-v853/lib" \
		TARGET_NAME="$(PKG_NAME)" \
		INSTALL_NAME="$(PKG_NAME)" \
		bin/$(PKG_NAME)
	TINA_SDK_ROOT="$(LICHEE_TOP_DIR)" V853_CXX="$(TARGET_CXX)" \
		STAGING_DIR="$(STAGING_DIR)" bash $(PKG_BUILD_DIR)/scripts/build-cv.sh
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/$(PKG_NAME) $(1)/usr/bin/$(PKG_NAME)
	$(INSTALL_DIR) $(1)/usr/share/webserver_camera
	$(CP) $(PKG_BUILD_DIR)/resources/. $(1)/usr/share/webserver_camera/
	$(INSTALL_DIR) $(1)/usr/lib/webserver_camera
	$(CP) $(PKG_BUILD_DIR)/build-v853/lib/*.so $(1)/usr/lib/webserver_camera/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/scripts/webserver_camera.init $(1)/etc/init.d/webserver_camera
	ln -sf webserver_camera $(1)/etc/init.d/S95webserver_camera
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build-v853/bin/webserver_cv $(1)/usr/bin/webserver_cv
	$(INSTALL_DIR) $(1)/usr/share/webserver_camera/models
	$(CP) $(PKG_BUILD_DIR)/vendor/100ask-cv/100ASK_Algo/pdet/100ASK_humandet_384_224_V1.nb $(1)/usr/share/webserver_camera/models/
	$(CP) $(PKG_BUILD_DIR)/vendor/100ask-cv/yolo/yolov5_model.nb $(1)/usr/share/webserver_camera/models/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/scripts/webserver_cv.init $(1)/etc/init.d/webserver_cv
	ln -sf webserver_cv $(1)/etc/init.d/S96webserver_cv
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
