# 因为文件中既有 C 文件也有 C++ 文件，所以分别指定编译器。
# 交叉编译时可以传 CROSS_COMPILE，例如：
#   make CROSS_COMPILE=arm-linux-gnueabihf-
# 或直接传 SDK 注入的 CC/CXX：
#   make CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc
CXX ?= $(CROSS_COMPILE)g++
AR ?= $(CROSS_COMPILE)ar
STRIP ?= $(CROSS_COMPILE)strip
PKG_CONFIG ?= pkg-config
ENABLE_GSTREAMER ?= 0
ENABLE_CEDAR ?= 0
override CPPFLAGS += -DENABLE_CEDAR=$(ENABLE_CEDAR)
override LDLIBS += -ldl
ifeq ($(ENABLE_CEDAR),1)
override CPPFLAGS += -I$(CEDARC_ROOT)/include -I$(CEDARC_ROOT)/base/include -I$(CEDARC_ROOT)/memory/include
override LDFLAGS += -L$(CEDARC_LIBDIR) -Wl,-rpath-link,$(CEDARC_LIBDIR)
override LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib:$$ORIGIN/../lib/webserver_camera'
override LDLIBS += -lvencoder -lMemAdapter
endif
ENABLE_JPEG ?= 0
override CPPFLAGS += -DENABLE_JPEG=$(ENABLE_JPEG)
ifeq ($(ENABLE_JPEG),1)
override LDLIBS += -ljpeg
endif

CPPFLAGS ?=
CFLAGS ?= -O2
CXXFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=
override CPPFLAGS += -Iinclude -DENABLE_GSTREAMER=$(ENABLE_GSTREAMER)
override CFLAGS += -Wall
override CXXFLAGS += -Wall -std=c++11
override LDLIBS += -lpthread

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin
TOOLS_DIR=tools
RES_DIR=resources
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/webserver_camera
TARGET_NAME ?= my_program
INSTALL_NAME ?= webserver_camera

# 默认只构建 HTTP、线程池、V4L2 和 MJPEG。
# rtsp_server.cpp 依赖 GStreamer，只有 ENABLE_GSTREAMER=1 时才加入构建。
C_SRCS=$(wildcard $(SRC_DIR)/*.c)
CPP_SRCS=$(filter-out $(SRC_DIR)/rtsp_server.cpp,$(wildcard $(SRC_DIR)/*.cpp))

ifeq ($(ENABLE_GSTREAMER),1)
GST_PACKAGES=gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0
GST_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES))
GST_LIBS := $(shell $(PKG_CONFIG) --libs $(GST_PACKAGES))
CPP_SRCS += $(SRC_DIR)/rtsp_server.cpp
override CPPFLAGS += $(GST_CFLAGS)
override LDLIBS += $(GST_LIBS)
endif

C_OBJS=$(C_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CPP_OBJS=$(CPP_SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

TARGET=$(BIN_DIR)/$(TARGET_NAME)
FLV_TOOL=$(BIN_DIR)/h264_to_flv
FLV_TOOL_OBJ=$(OBJ_DIR)/h264_to_flv_tool.o
FLV_MEDIA_OBJS=$(OBJ_DIR)/h264_parser.o $(OBJ_DIR)/flv_muxer.o
MEDIA_TEST=$(BIN_DIR)/media_pipeline_test
MEDIA_TEST_OBJ=$(OBJ_DIR)/media_pipeline_test.o

all: $(TARGET) $(FLV_TOOL)

$(TARGET): $(C_OBJS) $(CPP_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(FLV_TOOL): $(FLV_TOOL_OBJ) $(FLV_MEDIA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(FLV_TOOL_OBJ): $(TOOLS_DIR)/h264_to_flv.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(MEDIA_TEST): $(MEDIA_TEST_OBJ) $(FLV_MEDIA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(MEDIA_TEST_OBJ): tests/media_pipeline_test.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test-media: $(MEDIA_TEST)
	$(abspath $(MEDIA_TEST))

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(INSTALL_NAME)
	install -d $(DESTDIR)$(DATADIR)
	cp -r $(RES_DIR)/* $(DESTDIR)$(DATADIR)/

strip: $(TARGET)
	$(STRIP) $(TARGET)

# C 文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# C++ 文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)/*.o $(BIN_DIR)/$(TARGET_NAME) $(BIN_DIR)/h264_to_flv $(MEDIA_TEST)

.PHONY: all clean install strip test-media
