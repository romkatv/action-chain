appname := action_chain_test

CXX := g++
CXXFLAGS := -std=c++17 -fno-exceptions -Wall -Werror -g -DNDEBUG -O2
LDFLAGS := -pthread

SRCS := $(shell find src -name "*.cc")
OBJS := $(patsubst src/%.cc, obj/%.o, $(SRCS))

all: $(appname)

$(appname): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $(appname)

obj:
	mkdir -p obj

obj/%.o: src/%.cc Makefile | obj
	$(CXX) $(CXXFLAGS) -MM -MT $@ src/$*.cc >obj/$*.dep
	$(CXX) $(CXXFLAGS) -c -o $@ src/$*.cc

clean:
	rm -rf obj

-include $(OBJS:.o=.dep)
