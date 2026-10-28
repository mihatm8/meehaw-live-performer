# meehaw live performer — SDL2 desktop build (SPEC §7). Targets: all (default), run, selftest, clean.
#   make            builds build/liveplayer_desktop from every .cpp under firmware/liveplayer/src/{core,dsp,engine,gen,io,ui}
#                   plus firmware/desktop/*.cpp (ui_stub.cpp only while src/ui/ui.cpp is absent)
#   make run        opens the window with the card at ../../sdcard (CARD=... overrides)
#   make selftest   headless scripted session (SECONDS=... overrides, default 25; the frame lands in build/)
SRC := ../liveplayer/src
ifeq ($(origin CXX),default)
  CXX := clang++
endif
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$(SRC) $(SDL_CFLAGS) -MMD -MP
BIN := build/liveplayer_desktop
CARD ?= ../../sdcard
SECONDS ?= 25

FW_SRCS := $(wildcard $(SRC)/core/*.cpp $(SRC)/dsp/*.cpp $(SRC)/engine/*.cpp $(SRC)/gen/*.cpp $(SRC)/io/*.cpp $(SRC)/ui/*.cpp $(SRC)/rec/*.cpp)
DT_SRCS := $(wildcard *.cpp)
ifneq ($(wildcard $(SRC)/rec/recorder.cpp),)
  CXXFLAGS += -DLP_HAVE_RECORDER                      # main.cpp wires the session recorder (SPEC §9.23) when src/rec/recorder.cpp exists
endif
ifeq ($(wildcard $(SRC)/ui/ui.cpp),)
  CXXFLAGS += -DLP_UI_STUB
  UI_NOTE := ui: firmware/desktop/ui_stub.cpp (src/ui/ui.cpp absent)
else
  DT_SRCS := $(filter-out ui_stub.cpp,$(DT_SRCS))
  UI_NOTE := ui: src/ui/ui.cpp
endif

FW_OBJS := $(patsubst $(SRC)/%.cpp,build/obj/fw/%.o,$(FW_SRCS))
DT_OBJS := $(patsubst %.cpp,build/obj/%.o,$(DT_SRCS))
OBJS := $(FW_OBJS) $(DT_OBJS)

# Objects depend on the flag set (so switching between the stub and the real ui rebuilds main.o).
FLAGS_FILE := build/flags.txt
$(shell mkdir -p build; [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = "$(CXXFLAGS)" ] || printf '%s\n' "$(CXXFLAGS)" > $(FLAGS_FILE))

.PHONY: all run selftest clean
all: $(BIN)
	@echo "$(UI_NOTE)"
	@ls -l $(BIN) | awk '{ print "binary: " $$5 " bytes" }'

$(BIN): $(OBJS)
	$(CXX) $(OBJS) $(SDL_LIBS) -o $@

build/obj/fw/%.o: $(SRC)/%.cpp $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj/%.o: %.cpp $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) $(CARD)

selftest: $(BIN)
	./$(BIN) --selftest $(SECONDS) $(CARD)

clean:
	rm -rf build

-include $(OBJS:.o=.d)
