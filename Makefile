.PHONY: build
build:
	mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(shell nproc)

.PHONY: run
run: build
	build/glslls

.PHONY: install
install:
	install -Dm755 build/glslls $(DESTDIR)/$(PREFIX)/bin/glslls

.PHONY: clean
clean:
	rm -rf build
