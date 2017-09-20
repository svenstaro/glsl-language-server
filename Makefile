.PHONY: build
build:
	mkdir -p build && cd build && cmake .. && make -j$(shell nproc)

.PHONY: run
run: build
	build/glslls

.PHONY: clean
clean:
	rm -rf build
