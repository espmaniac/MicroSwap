CXX      ?= c++
CXXFLAGS ?= -std=c++17 -g -Wall -Wextra -fsanitize=address,undefined

test: test_containers
	./test_containers

test_containers: test_containers.cpp ../containers.h mock/FS.h
	$(CXX) $(CXXFLAGS) -Imock -o $@ test_containers.cpp

clean:
	rm -f test_containers

.PHONY: test clean
