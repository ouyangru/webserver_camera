#因为文件中既有c文件也有cpp文件，所以需要分别指定编译器
CC ?= gcc
CXX ?= g++
PKG_CONFIG ?= pkg-config
CFLAGS ?= -Wall -O2
CXXFLAGS ?= -Wall -O2
CFLAGS += -Iinclude
CXXFLAGS += -Iinclude
CFLAGS += $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-rtsp-server-1.0)
CXXFLAGS += $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-rtsp-server-1.0)
LDFLAGS += -lpthread -lstdc++
LDFLAGS += $(shell $(PKG_CONFIG) --libs gstreamer-1.0 gstreamer-rtsp-server-1.0)

SRC_DIR=src
OBJ_DIR=obj
BIN_DIR=bin


#获取所有源文件和对应目标文件
C_SRCS=$(wildcard $(SRC_DIR)/*.c)
CPP_SRCS=$(wildcard $(SRC_DIR)/*.cpp)
C_OBJS=$(C_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CPP_OBJS=$(CPP_SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

TARGET=$(BIN_DIR)/my_program

all: $(TARGET)

$(TARGET): $(C_OBJS) $(CPP_OBJS)
	mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(LDFLAGS)

#C文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

#CPP文件编译规则
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)/*.o $(BIN_DIR)/my_program

.PHONY: all clean