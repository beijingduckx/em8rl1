IMGUI_DIR = imgui
IMGUI_FILE_DLG_DIR = ImGuiFileDialog

IMGUI = imgui.cpp imgui_demo.cpp imgui_draw.cpp imgui_impl_sdl2.cpp imgui_impl_sdlrenderer2.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI += ImGuiFileDialog.cpp

BUILD_DIR=build

SRCS = fx2load.cpp em8RL1.cpp $(IMGUI)
OBJS = $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(notdir $(SRCS)))

INCLUDE=-I imgui -I imgui/backends -I ImGuiFileDialog -I firmware

CXXFLAGS := -O3 ${INCLUDE} 
CXXFLAGS += $(shell pkg-config --cflags libusb-1.0)
CXXFLAGS += $(shell pkg-config --cflags sdl2)

LIBS := $(shell pkg-config --libs libusb-1.0)
LIBS += $(shell pkg-config --libs sdl2)

all: em8rl1

$(BUILD_DIR)/em8RL1.o: Recorder.h
$(BUILD_DIR)/fx2load.o: firmware/fx2firm.inc

vpath %.cpp $(IMGUI_DIR)
vpath %.cpp $(IMGUI_DIR)/backends
vpath %.cpp $(IMGUI_FILE_DLG_DIR)

firmware/fx2firm.inc: firmware/em8rl1.c
	(cd firmware && $(MAKE))

$(BUILD_DIR):
	mkdir $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

em8rl1: $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LIBS)

clean:
	-rm build/*.o
	-rm em8rl1

