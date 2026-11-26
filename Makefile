# よく使うコマンド（CLAUDE.md「コマンド」）。
#   make build | test | golden      … そのマシンでビルド・テスト（macOS でも Linux でも）
#   make tables | lint | contract   … Linux の開発用コンテナ（deploy/Dockerfile.dev）で行う
#   make IN_DOCKER=1 test           … build・test・golden もコンテナで行う（CI はこれ）
#   make PRESET=tsan test           … プリセットを変える（debug・release・asan-ubsan・tsan・coverage）
#
# iot-common-spec の版は cmake/IotSpec.cmake の IOTSPEC_REF だけで決める。

PRESET ?= debug
IOTSPEC_REF := $(shell grep '^set.IOTSPEC_REF ' cmake/IotSpec.cmake | cut -d'"' -f2)
IOTSPEC_DIR_REL := .iotspec/$(IOTSPEC_REF)
IOTSPEC_DIR := $(CURDIR)/$(IOTSPEC_DIR_REL)
PLATFORM := $(shell uname -s | tr A-Z a-z)-$(shell uname -m)
BUILD_DIR ?= build/$(PLATFORM)-$(PRESET)
COMPOSE := IOTSPEC_REF=$(IOTSPEC_REF) IOTSPEC_DIR=$(IOTSPEC_DIR) IOTSPEC_DIR_REL=$(IOTSPEC_DIR_REL) \
           GW_UID=$(shell id -u) GW_GID=$(shell id -g) \
           docker compose -p iot-gw -f deploy/docker-compose.yml
# 空なら --role gw-01 の全チェック（Phase 3 から）。一部だけ流すときは make contract CONTRACT_CHECKS="schema"
CONTRACT_CHECKS ?=
CONTRACT_JUNIT ?= contract-gw-01.xml
JOBS ?= 4
# 開発用コンテナに uv tool install で入れた iotspec の Python（check_tables.py が iotspec を import する）
IOTSPEC_PYTHON ?= /opt/uv/tools/iotspec/bin/python
# 作品02（io0323/iot-modular-node-esp32）のホスト版：結合テストの相手（取り出してビルドするだけで変更しない）
WORK02_REF ?= 186b138fb8fe22d1819bf0ee55625a52f4d64bbe
WORK02_DIR := .work02/$(WORK02_REF)
WORK02_BUILD := build/work02-$(PLATFORM)
COVERAGE_MIN ?= 80

.PHONY: build test golden tables lint format contract integration coverage work02 work02-src spec image clean

ifeq ($(IN_DOCKER):$(GW_IN_CONTAINER),1:)
# ---- ホストからコンテナに入って同じターゲットを実行する
build test golden: spec image
	$(COMPOSE) run --rm --no-deps dev make $@ PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF)
else
build: spec
	cmake --preset $(PRESET) -B $(BUILD_DIR) -DIOTSPEC_REF=$(IOTSPEC_REF) \
	  -DFETCHCONTENT_SOURCE_DIR_IOTSPEC=$(IOTSPEC_DIR)
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -j $(JOBS)

golden: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -L golden
endif

ifeq ($(GW_IN_CONTAINER),1)
# ---- コンテナの中
tables: build
	$(BUILD_DIR)/bin/gw-dump-tables > $(BUILD_DIR)/tables.json
	$(IOTSPEC_PYTHON) tools/check_tables.py $(BUILD_DIR)/tables.json

lint: build
	scripts/lint.sh $(BUILD_DIR)

contract: build
	scripts/run-contract.sh $(BUILD_DIR) $(CONTRACT_JUNIT) $(CONTRACT_CHECKS)

# 結合テスト（pytest、tests/integration）：iotspec-sim の代役・作品02 のホスト版・duplicate_and_gap
integration: build work02
	GW_BIN=$(BUILD_DIR)/bin/iot-gw MCU_CPP_01_BIN=$(WORK02_BUILD)/mcu_cpp_01 \
	  $(IOTSPEC_PYTHON) -m pytest -v tests/integration --junitxml=integration.xml

work02: $(WORK02_BUILD)/mcu_cpp_01
$(WORK02_BUILD)/mcu_cpp_01: $(WORK02_DIR)/.done
	cmake -S $(WORK02_DIR) -B $(WORK02_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
	cmake --build $(WORK02_BUILD) --target mcu_cpp_01 -j $(JOBS)

# core の行カバレッジ（gcovr）。COVERAGE_MIN（%）未満なら失敗（NFR-09）
coverage:
	$(MAKE) test PRESET=coverage
	gcovr --root . --filter 'src/core/' --object-directory build/$(PLATFORM)-coverage \
	  --print-summary --fail-under-line $(COVERAGE_MIN) \
	  --xml build/$(PLATFORM)-coverage/coverage.xml --txt build/$(PLATFORM)-coverage/coverage.txt
else
# ---- ホスト（macOS・Linux）からはコンテナで行う
tables lint: spec image
	$(COMPOSE) run --rm --no-deps dev make $@ PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF)

integration: spec image work02-src
	cd $(IOTSPEC_DIR) && IOTSPEC_MQTT_PASSWORD=$${IOTSPEC_MQTT_PASSWORD:-iotspec-dev} deploy/mosquitto/gen_passwd.sh
	$(COMPOSE) up -d mosquitto
	$(COMPOSE) run --rm dev make integration PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF); \
	  status=$$?; $(COMPOSE) down; exit $$status

coverage: spec image
	$(COMPOSE) run --rm --no-deps dev make coverage IOTSPEC_REF=$(IOTSPEC_REF)

contract: spec image
	cd $(IOTSPEC_DIR) && IOTSPEC_MQTT_PASSWORD=$${IOTSPEC_MQTT_PASSWORD:-iotspec-dev} deploy/mosquitto/gen_passwd.sh
	$(COMPOSE) up -d mosquitto
	$(COMPOSE) run --rm dev make contract PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF) CONTRACT_JUNIT=$(CONTRACT_JUNIT); \
	  status=$$?; $(COMPOSE) down; exit $$status
endif

format:
	clang-format -i $$(git ls-files --cached --others --exclude-standard '*.cpp' '*.h')

# 作品02 を WORK02_REF で取り出す（submodule の iot-common-spec v0.2.1 を含む。作品02 は変更しない）
work02-src: $(WORK02_DIR)/.done
$(WORK02_DIR)/.done:
	mkdir -p $(WORK02_DIR)
	git -C $(WORK02_DIR) init --quiet
	git -C $(WORK02_DIR) fetch --quiet --depth 1 https://github.com/io0323/iot-modular-node-esp32.git $(WORK02_REF)
	git -C $(WORK02_DIR) -c advice.detachedHead=false checkout --quiet --force FETCH_HEAD
	git -C $(WORK02_DIR) submodule update --quiet --init --recursive --depth 1
	touch $@

# iot-common-spec を IOTSPEC_REF で取り出す（Mosquitto の設定と、ビルドの FetchContent に使う）
spec: $(IOTSPEC_DIR)/.done
$(IOTSPEC_DIR)/.done:
	rm -rf $(IOTSPEC_DIR)
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch $(IOTSPEC_REF) \
	  https://github.com/io0323/iot-common-spec.git $(IOTSPEC_DIR)
	touch $@

# タグのコミットを Docker のキャッシュの鍵に入れる（同じ版番号で中身が変わっても作り直す）
image:
	IOTSPEC_COMMIT=$$(git ls-remote https://github.com/io0323/iot-common-spec.git "refs/tags/$(IOTSPEC_REF)^{}" "refs/heads/$(IOTSPEC_REF)" | head -n 1 | cut -f1) \
	  $(COMPOSE) build dev

clean:
	rm -rf build .iotspec .work02
