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
CONTRACT_CHECKS := golden_conversion bridged_offline_state schema seq_and_boot_id publish_rate
CONTRACT_JUNIT ?= contract-gw-01.xml
JOBS ?= 4
# 開発用コンテナに uv tool install で入れた iotspec の Python（check_tables.py が iotspec を import する）
IOTSPEC_PYTHON ?= /opt/uv/tools/iotspec/bin/python

.PHONY: build test golden tables lint format contract spec image clean

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
else
# ---- ホスト（macOS・Linux）からはコンテナで行う
tables lint: spec image
	$(COMPOSE) run --rm --no-deps dev make $@ PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF)

contract: spec image
	cd $(IOTSPEC_DIR) && IOTSPEC_MQTT_PASSWORD=$${IOTSPEC_MQTT_PASSWORD:-iotspec-dev} deploy/mosquitto/gen_passwd.sh
	$(COMPOSE) up -d mosquitto
	$(COMPOSE) run --rm dev make contract PRESET=$(PRESET) IOTSPEC_REF=$(IOTSPEC_REF) CONTRACT_JUNIT=$(CONTRACT_JUNIT); \
	  status=$$?; $(COMPOSE) down; exit $$status
endif

format:
	clang-format -i $$(git ls-files --cached --others --exclude-standard '*.cpp' '*.h')

# iot-common-spec を IOTSPEC_REF で取り出す（Mosquitto の設定と、ビルドの FetchContent に使う）
spec: $(IOTSPEC_DIR)/.done
$(IOTSPEC_DIR)/.done:
	rm -rf $(IOTSPEC_DIR)
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch $(IOTSPEC_REF) \
	  https://github.com/io0323/iot-common-spec.git $(IOTSPEC_DIR)
	touch $@

image:
	$(COMPOSE) build dev

clean:
	rm -rf build .iotspec
