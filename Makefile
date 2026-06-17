# 因为文件中既有 C 文件也有 C++ 文件，所以分别指定编译器。
CC ?= gcc
CXX ?= g++
PKG_CONFIG ?= pkg-config
ENABLE_GSTREAMER ?= 0

CFLAGS ?= -Wall -O2
CXXFLAGS ?= -Wall -O2
CFLAGS += -Iinclude
CXXFLAGS += -std=c++11 -Iinclude
CXXFLAGS += -DENABLE_GSTREAMER=$(ENABLE_GSTREAMER)
LDFLAGS += -lpthread -lstdc++

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin
TOOLS_DIR=tools

# 默认只构建 HTTP、线程池、V4L2 和 MJPEG。
# rtsp_server.cpp 依赖 GStreamer，只有 ENABLE_GSTREAMER=1 时才加入构建。
C_SRCS=$(wildcard $(SRC_DIR)/*.c)
CPP_SRCS=$(filter-out $(SRC_DIR)/rtsp_server.cpp,$(wildcard $(SRC_DIR)/*.cpp))

ifeq ($(ENABLE_GSTREAMER),1)
GST_PACKAGES=gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0
GST_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES))
GST_LIBS := $(shell $(PKG_CONFIG) --libs $(GST_PACKAGES))
CPP_SRCS += $(SRC_DIR)/rtsp_server.cpp
CXXFLAGS += $(GST_CFLAGS)
LDFLAGS += $(GST_LIBS)
endif

C_OBJS=$(C_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CPP_OBJS=$(CPP_SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

TARGET=$(BIN_DIR)/my_program
FLV_TOOL=$(BIN_DIR)/h264_to_flv
FLV_TOOL_OBJ=$(OBJ_DIR)/h264_to_flv_tool.o
FLV_MEDIA_OBJS=$(OBJ_DIR)/h264_parser.o $(OBJ_DIR)/flv_muxer.o
MEDIA_TEST=$(BIN_DIR)/media_pipeline_test
MEDIA_TEST_OBJ=$(OBJ_DIR)/media_pipeline_test.o

all: $(TARGET) $(FLV_TOOL)

$(TARGET): $(C_OBJS) $(CPP_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(FLV_TOOL): $(FLV_TOOL_OBJ) $(FLV_MEDIA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^

$(FLV_TOOL_OBJ): $(TOOLS_DIR)/h264_to_flv.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MEDIA_TEST): $(MEDIA_TEST_OBJ) $(FLV_MEDIA_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^

$(MEDIA_TEST_OBJ): tests/media_pipeline_test.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test-media: $(MEDIA_TEST)
	./$(MEDIA_TEST)

# C 文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# C++ 文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)/*.o $(BIN_DIR)/my_program $(BIN_DIR)/h264_to_flv $(MEDIA_TEST)

.PHONY: all clean test-media
