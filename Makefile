CC = g++
CFLAGS = -std=c++11 -Wall -Wextra -Werror

SRC_DIR = src
BUILD_DIR = build

.PHONY: all client server clean

all: client server

client: $(BUILD_DIR)/client.o
	$(CC) $< -o $@

server: $(BUILD_DIR)/server.o
	$(CC) $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -c -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -r client server $(BUILD_DIR)
