TARGET   := bully
BUILD    := build
SRCDIRS  := source source/hooks source/etcpak

CFILES   := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))
CPPFILES := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.cpp))
OBJS     := $(patsubst %,$(BUILD)/%.o,$(CFILES) $(CPPFILES))

# RK3326 = Cortex-A35
ARCH     := -march=armv8-a+crc -mtune=cortex-a35
CFLAGS   := -g -O2 -Wall -ffunction-sections $(ARCH) -Isource -MMD -MP
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(ARCH)

# minizip/zlib static (not guaranteed on device); GL/SDL/AL from device (libMali, SDL2 2.32, OpenAL 1.24)
LIBS     := -l:libminizip.a -l:libz.a -lSDL2 -lopenal -lEGL -lGLESv2 -lpthread -ldl -lm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# third-party texture encoder: its own warnings are upstream's
$(BUILD)/source/etcpak/%.cpp.o: CXXFLAGS += -w

clean:
	rm -rf $(BUILD) $(TARGET)

-include $(OBJS:.o=.d)