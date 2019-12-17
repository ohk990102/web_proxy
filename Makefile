CXXFLAGS =
LDLIBS = -lpthread

all: web_proxy

debug: CXXFLAGS += -DDEBUG -g
debug: web_proxy

web_proxy: web_proxy.cpp
	$(CXX) -o $@ $^ $(CXXFLAGS)	$(LDLIBS)

clean:
	rm -rf web_proxy

.PHONY: all clean