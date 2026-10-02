# Run from the repository root. GNU Make 3.81+ (including macOS make).
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CXX ?= c++
PYTHON ?= python3
CPPFLAGS ?=
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := .build
TEST_DIR := $(BUILD_DIR)/tests
SAN_DIR := $(BUILD_DIR)/sanitize
TOOL_DIR := $(BUILD_DIR)/tools
COMMON_FLAGS := -std=c++17 -pthread -Iinc
CORE_TESTS := model_test algorithm_test solver_options_test
CPP_TESTS := $(CORE_TESTS) official_qa_test property_test sample_maps_test
TEST_BINS := $(addprefix $(TEST_DIR)/,$(CPP_TESTS))
SAN_TESTS := $(CORE_TESTS) official_qa_test property_test
SAN_BINS := $(addprefix $(SAN_DIR)/,$(SAN_TESTS))
TOOL_BINS := $(addprefix $(TOOL_DIR)/,solver_benchmark agent_selection_benchmark tune_weights arena_benchmark)
PY_ENV = PYTHONPYCACHEPREFIX="$(abspath $(BUILD_DIR)/pycache)" PROCON_TEST_SOLVER="$(abspath main)"

.PHONY: all release tools test test-core test-cpp test-python test-samples test-property test-official-qa \
        test-model test-algorithm test-options test-gui test-server test-sanitize \
        test-all check-samples help

all: main
release: main
tools: $(TOOL_BINS)

main: main.cpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_FLAGS) -MMD -MP -MF $(BUILD_DIR)/main.d $< $(LDFLAGS) $(LDLIBS) -o $@

$(TEST_DIR)/%: tests/%.cpp Makefile | $(TEST_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_FLAGS) -UNDEBUG -MMD -MP -MF $@.d $< $(LDFLAGS) $(LDLIBS) -o $@

$(SAN_DIR)/%: tests/%.cpp Makefile | $(SAN_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O1 -g -Wall -Wextra -Wpedantic -Werror -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer -MMD -MP -MF $@.d $< $(LDFLAGS) $(LDLIBS) -fsanitize=address,undefined -o $@

$(TOOL_DIR)/%: tests/%.cpp Makefile | $(TOOL_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_FLAGS) -MMD -MP -MF $@.d $< $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR) $(TEST_DIR) $(SAN_DIR) $(TOOL_DIR):
	mkdir -p "$@"

# Normal regression suite: no GUI, real server, or external network.
test: test-cpp test-python
test-core: test-model test-algorithm test-options test-official-qa test-property test-python
test-cpp: test-model test-algorithm test-options test-official-qa test-property test-samples

test-model: $(TEST_DIR)/model_test
	$(TEST_DIR)/model_test
test-algorithm: $(TEST_DIR)/algorithm_test
	$(TEST_DIR)/algorithm_test
test-options: $(TEST_DIR)/solver_options_test
	$(TEST_DIR)/solver_options_test
test-property: $(TEST_DIR)/property_test
	$(TEST_DIR)/property_test
test-official-qa: $(TEST_DIR)/official_qa_test
	$(TEST_DIR)/official_qa_test

check-samples:
	@for size in 16 24 32; do \
		if ! test -f "sample_maps/sample_$$size.json"; then \
			echo "Missing sample_maps/sample_$$size.json; install the sample maps or use make test-core." >&2; \
			exit 1; \
		fi; \
	done

test-samples: check-samples $(TEST_DIR)/sample_maps_test
	$(TEST_DIR)/sample_maps_test

test-python: main
	$(PY_ENV) $(PYTHON) -m unittest -v tests/visualizer_test.py tests/recovery_test.py tests/visualizer_runtime_test.py

# Explicit opt-in: desktop window / localhost listener / sanitizers required.
test-gui:
	PYTHONPYCACHEPREFIX="$(abspath $(BUILD_DIR)/pycache)" $(PYTHON) tests/visualizer_gui_test.py
test-server: main
	$(PY_ENV) PROCON_TEST_SIMPLE_SERVER=1 $(PYTHON) -m unittest -v tests/visualizer_simple_server_test.py
test-sanitize: $(SAN_BINS)
	$(SAN_DIR)/model_test
	$(SAN_DIR)/algorithm_test
	$(SAN_DIR)/solver_options_test
	$(SAN_DIR)/official_qa_test
	$(SAN_DIR)/property_test
test-all: test test-gui test-server test-sanitize

help:
	@echo "make                 Build ./main (C++17, -O2, strict warnings)"
	@echo "make test            C++ + sample maps + Python/real solver regression"
	@echo "make test-property   Random-map and timed-refuel property regression"
	@echo "make test-official-qa Official Q&A rule regression"
	@echo "make test-core       Regression without sample map files"
	@echo "make test-gui        Display the GUI and run mocked UI tests"
	@echo "make test-server     Run the bundled localhost server integration test"
	@echo "make test-sanitize   Run core C++ tests with ASan/UBSan"
	@echo "make test-all        Run all of the above tests"
	@echo "make tools           Build benchmark and weight tuner (do not run)"
	@echo "Use make -B after changing compiler/flags; override CXX/PYTHON as needed."

-include $(BUILD_DIR)/main.d $(addsuffix .d,$(TEST_BINS) $(SAN_BINS) $(TOOL_BINS))
