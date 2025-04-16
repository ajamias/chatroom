CC = g++
CFLAGS = -std=c++11 -Wall -Wextra -Werror

SRC_DIR = src
BUILD_DIR = build

INCLUDE_PG = $(shell pg_config --includedir)
LIB_PG = $(shell pg_config --libdir)

.PHONY: all client server clean

all: client server

client: $(BUILD_DIR)/client.o
	$(CC) $< -o $@

server: $(BUILD_DIR)/server.o
	$(CC) -L$(LIB_PG) -lpq $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@if [ "$*" = "server" ]; then \
		$(CC) -I$(INCLUDE_PG) $(CFLAGS) $< -c -o $@; \
	else \
		$(CC) $(CFLAGS) $< -c -o $@; \
	fi

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -r client server $(BUILD_DIR)
