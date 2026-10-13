# ps5-spotify app. Build/packaging from ps5-homebrew-ui (GPL-3.0-or-later).
PS5_HOST ?=
FTP_PORT ?= 2121
PS5_FTP_USER ?= anonymous
PS5_FTP_PASSWORD ?= anonymous
DEPLOY_DRY_RUN ?= 0
export PS5_HOST FTP_PORT PS5_FTP_USER PS5_FTP_PASSWORD DEPLOY_DRY_RUN

.PHONY: app deploy clean
app: build/deps/libbell.a
	@APP_STATIC_ARCHIVES="build/deps/libbell.a build/deps/libmbedtls.a" APP_INCLUDE_PATHS="../cspot/cspot/bell/main/io/include ../cspot/cspot/bell/main/utilities/include ../cspot/cspot/bell/main/platform ../cspot/cspot/bell/external/nanopb ../cspot/cspot/bell/external/nlohmann_json/include ../cspot/cspot/include build/deps/proto ../cspot/cspot/bell/external/tremor src .deps/src/mbedtls-3.6.4/include" APP_DEFINITIONS="BELL_DISABLE_FMT BELL_DISABLE_REGEX PB_ENABLE_MALLOC PB_FIELD_32BIT" USE_CCACHE=$${USE_CCACHE:-0} PS5_CLANG=$${PS5_CLANG:-clang} bash tools/build.sh
deploy:
	@bash tools/deploy.sh
clean:
	rm -rf build dist
build/deps/libbell.a:
	@bash tools/build-deps.sh
