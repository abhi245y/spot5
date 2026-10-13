# ps5-spotify app. Build/packaging and UI kit from ps5-homebrew-ui
# (GPL-3.0-or-later).
#
#   make            PS5 app folder in dist/
#   make deps       rebuild mbedTLS + bell/cspot libraries (after cspot changes)
#   make deploy PS5_HOST=<ip>

SHELL := /bin/bash
.DEFAULT_GOAL := app

PS5_CLANG ?= clang
USE_CCACHE ?= 0
PS5_HOST ?=
FTP_PORT ?= 2121
PS5_FTP_USER ?= anonymous
PS5_FTP_PASSWORD ?= anonymous
DEPLOY_DRY_RUN ?= 0

CSPOT := ../cspot/cspot
BELL := $(CSPOT)/bell
OPENGL_SDK := .deps/ps5-opengl/current

# Defines that change class layouts must match tools/build-deps.sh exactly.
APP_DEFINITIONS := GL_GLEXT_PROTOTYPES=1 BELL_TASK_MIN_STACK=1048576 \
	CSPOT_CDN_CHUNK_SIZE=262144 BELL_DISABLE_FMT BELL_DISABLE_REGEX \
	PB_ENABLE_MALLOC PB_FIELD_32BIT
APP_INCLUDE_PATHS := src $(OPENGL_SDK)/include \
	$(BELL)/main/io/include $(BELL)/main/utilities/include $(BELL)/main/platform \
	$(BELL)/external/nanopb $(BELL)/external/nlohmann_json/include $(BELL)/external/tremor \
	$(CSPOT)/include build/deps/proto .deps/src/mbedtls-3.6.4/include
APP_STATIC_ARCHIVES := build/deps/libbell.a build/deps/libmbedtls.a \
	.deps/ps5-opengl/libps5opengl-group.a
APP_IMPORT_STUBS := $(OPENGL_SDK)/lib/libSceAgc.so $(OPENGL_SDK)/lib/libSceAgcDriver.so

export PS5_CLANG USE_CCACHE PS5_HOST FTP_PORT PS5_FTP_USER PS5_FTP_PASSWORD DEPLOY_DRY_RUN
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS

.PHONY: app deps opengl deploy clean
app: build/deps/libbell.a opengl
	@bash tools/build.sh
deps:
	@bash tools/build-deps.sh
build/deps/libbell.a:
	@bash tools/build-deps.sh
opengl:
	@bash tools/prepare-opengl.sh
deploy:
	@bash tools/deploy.sh
clean:
	rm -rf build dist
