CXX ?= g++

CPPFLAGS := -I../../log_system/logs_code -isystem .
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

SERVER_SOURCES := Test.cpp Service.cpp DataManager.cpp base64.cpp
SERVER_OBJECTS := $(SERVER_SOURCES:%.cpp=$(OBJ_DIR)/%.o)
SERVER_LIBS := -ljsoncpp -lbundle -levent -lssl -lcrypto
TEST_LIBS := -ljsoncpp -lbundle

.DEFAULT_GOAL := all

.PHONY: all cloud_storage_server backup_server log_example benchmark unit_tests
.PHONY: test tests clean purge-data

all: cloud_storage_server backup_server log_example benchmark

cloud_storage_server: $(BIN_DIR)/cloud_storage_server

backup_server: $(BIN_DIR)/backup_server

log_example: $(BIN_DIR)/log_example

benchmark: $(BIN_DIR)/benchmark

unit_tests: $(BIN_DIR)/unit_tests

test tests: $(BIN_DIR)/unit_tests
	$(BIN_DIR)/unit_tests

$(BIN_DIR)/cloud_storage_server: $(SERVER_OBJECTS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) $^ $(SERVER_LIBS) -pthread -o $@

$(BIN_DIR)/backup_server: ../../log_system/logs_code/backlog/ServerBackupLog.cpp \
                          ../../log_system/logs_code/backlog/ServerBackupLog.hpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BIN_DIR)/log_example: ../../log_system/examples/test.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -ljsoncpp -o $@

$(BIN_DIR)/benchmark: bench_log.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -ljsoncpp -o $@

$(BIN_DIR)/unit_tests: tests/unit_tests.cpp DataManager.cpp base64.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) $(TEST_LIBS) -o $@

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

purge-data:
	@if [ "$(CONFIRM)" != "yes" ]; then \
		echo "Refusing to delete runtime data. Use: make purge-data CONFIRM=yes"; \
		exit 1; \
	fi
	rm -rf -- deep_storage low_storage tmp logfile storage.data

-include $(SERVER_OBJECTS:.o=.d)
