.PHONY: build
build:
	cmake -GNinja -Bbuild . && ninja -Cbuild

.PHONY: run
run: build
	build/glslls

.PHONY: clean
clean:
	rm -rf build
