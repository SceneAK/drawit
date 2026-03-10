TARGET = drawit
CC = gcc
SRC_DIR = src
VENDOR_DIR = vendor
BUILD_DIR = build

BUILD ?= DEBUG
CFLAGS = -Wall -I$(VENDOR_DIR) -fno-strict-aliasing

ifeq ($(BUILD), RELEASE)
	CFLAGS += -O2
	LDFLAGS = -s
else
	CFLAGS += -g -O0
endif

TARGET_BIN = $(BUILD_DIR)/$(TARGET)
ifeq ($(OS), Windows_NT)
	LIBS += -lopengl32 -lgdi32 -lwinmm
	TARGET_BIN := $(TARGET_BIN).exe
	RM = rmdir /s /q
	# Black magic from gemini
	MD = if not exist "$(subst /,\,$(@D))" mkdir "$(subst /,\,$(@D))"
else
	LIBS += -lGL -lm -lpthread -ldl -lrt -lX11
	RM = rm -r
	MD = mkdir -p $@
endif

SRCS = $(wildcard $(SRC_DIR)/*.c) \
       $(wildcard $(VENDOR_DIR)/glad/*.c) \
       $(wildcard $(VENDOR_DIR)/nanovg/*.c) \
       $(wildcard $(VENDOR_DIR)/sokol/*.c)
       
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean

all: $(TARGET_BIN)

$(TARGET_BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/%.o: %.c 
	$(MD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(BUILD_DIR)