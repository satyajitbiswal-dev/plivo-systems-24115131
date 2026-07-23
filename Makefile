# CC ?= cc
# CFLAGS ?= -O2 -Wall

# all: sender receiver

# sender: sender.cpp
# 	$(CC) $(CFLAGS) -o sender sender.cpp

# receiver: receiver.cpp
# 	$(CC) $(CFLAGS) -o receiver receiver.cpp

# clean:
# 	rm -f sender receiver

CXX = g++
CXXFLAGS = -O2 -Wall -std=c++17 -pthread

all: sender receiver

sender: sender.cpp
	$(CXX) $(CXXFLAGS) -o sender sender.cpp

receiver: receiver.cpp
	$(CXX) $(CXXFLAGS) -o receiver receiver.cpp

clean:
	rm -f sender receiver
