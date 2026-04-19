MOC      := $(shell pkg-config --variable=libexecdir Qt6Core)/moc
CXX      := g++
TARGET   := ollama_gui
SRC      := main.cpp

CXXFLAGS := -std=c++20 -fPIC \
            -Wall -Wextra -Wpedantic -Werror \
            -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor \
            -Wold-style-cast -Woverloaded-virtual -Wnull-dereference \
            -Wdouble-promotion -Wformat=2

QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6Network | sed 's/-I/-isystem /g')
QT_LIBS   := $(shell pkg-config --libs   Qt6Widgets Qt6Network)

# Raw Qt flags without -isystem substitution — used in compile_commands.json
# so clangd/LSP can resolve Qt headers properly
QT_CFLAGS_RAW := $(shell pkg-config --cflags Qt6Widgets Qt6Network)

.PHONY: all clean compile_commands

all: compile_commands $(TARGET)

main.moc: $(SRC)
	$(MOC) $< -o $@

$(TARGET): $(SRC) main.moc
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $< -o $@ $(QT_LIBS)

# Generate compile_commands.json for clangd / LSP / static analysers
compile_commands: compile_commands.json

compile_commands.json: $(SRC) Makefile
	@printf '[\n' > $@
	@printf '  {\n' >> $@
	@printf '    "directory": "%s",\n' "$$(pwd)" >> $@
	@printf '    "file": "%s/%s",\n' "$$(pwd)" "$(SRC)" >> $@
	@printf '    "command": "%s %s %s %s -o %s %s"\n' \
	    "$(CXX)" \
	    "$(CXXFLAGS)" \
	    "$(QT_CFLAGS_RAW)" \
	    "$$(pwd)/$(SRC)" \
	    "$$(pwd)/$(TARGET)" \
	    "$(QT_LIBS)" >> $@
	@printf '  }\n' >> $@
	@printf ']\n' >> $@
	@echo "Generated compile_commands.json"

clean:
	rm -f main.moc $(TARGET) compile_commands.json
