CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=
LTO ?= 0
STATIC ?= 0
TARGET := powercalc
SRC := src/powercalc.cpp

ifeq ($(LTO),1)
CXXFLAGS += -flto=auto
LDFLAGS += -flto=auto
endif

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

.PHONY: all clean check install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

check: $(TARGET)
	./$(TARGET) --version
	./$(TARGET) --help >/dev/null
	./$(TARGET) cost 80 0.23 6 30 >/dev/null
	./$(TARGET) estimate 500 35 8 0.23 >/dev/null

install: $(TARGET)
	install -Dm755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -f $(TARGET)
