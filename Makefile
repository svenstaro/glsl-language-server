.PHONY: build
build:
	cmake -GNinja -Bbuild . && ninja -Cbuild

.PHONY: run
run: build
	build/glslls

.PHONY: install
install: build
	ninja -Cbuild install

.PHONY: clean
clean:
	rm -rf build
