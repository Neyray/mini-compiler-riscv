CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Iinclude/frontend -Iinclude/midend -Iinclude/backend -Iinclude/common

SRCS := $(wildcard src/*.cpp) \
        $(wildcard src/frontend/*.cpp) \
        $(wildcard src/midend/*.cpp) \
        $(wildcard src/backend/*/*.cpp)
OBJS := $(SRCS:.cpp=.o)

compiler: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) compiler

.PHONY: clean
