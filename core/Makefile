CC ?= cc
NDKBIN ?= $(wildcard /home/z/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin)
ROM := ../rom/rompack.py

all: vmdemo demo.rom

vmdemo: src/vm_core.c src/vm_rom.c src/vm_loader.c src/vm_backend_sigsys.c src/vm_binder.c src/vmdemo.c
	$(CC) -O2 -g -Wall -Iinclude -o $@ $^

build_guest: guest/guest_demo.c
	mkdir -p build
	$(CC) -O2 -Wall -nostdlib -static -ffreestanding -fno-stack-protector -fno-pic -fno-builtin -no-pie -o $@ $<

# Stage 1：静态 bionic guest（x86_64，NDK sysroot；真机版 arm64 在 android/build.sh）
NDKBIN ?= $(wildcard /home/z/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin)
build_guest1: guest/guest_stage1.c
	mkdir -p build
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie -o $@ $<

guest_arm64: guest/guest_demo.c
	mkdir -p build
	$(NDKBIN)/clang -target aarch64-linux-android24 -O2 -nostdlib -static -ffreestanding -fno-stack-protector -fno-pic -fno-builtin -o build/guest64 $<

demo.rom: build_guest1 ../rom/fixture/rom/hello.txt
	mkdir -p build/romroot/bin build/romroot/rom
	cp ../rom/fixture/rom/hello.txt build/romroot/rom/
	cp build_guest1 build/romroot/bin/guest
	python3 $(ROM) pack build/romroot $@

run: vmdemo demo.rom
	./vmdemo demo.rom auto

# ── v0.2.8：离线回归 + 真机/容器自检宿主 ──
# test: 离线回归（都不需要 seccomp/真机）
#   bpf_sim    —— 过滤器判决表/放行清单一致性（铁律：直发+引擎自用 syscall 必须放行）
#   vm_memtest —— guest 野指针防护 + 控制台 NUL 卫生（v0.7.22 真机 -31/断流同型故障）
test: test-bpf test-mem

test-bpf:
	python3 tests/bpf_sim.py

# v0.7.23：ROM 一致性（ROM 里所有 .so 必须来自同一次系统快照）
#   症状：dlopen failed: cannot locate symbol "X" referenced by "libY.so"（ART 库 BIND_NOW，
#        强未定义符号必须加载期解析；旧版库就会让 dalvikvm 直接 code=1）
checkrom:
	python3 ../rom/checkrom.py ../android/assets/demo.rom --strict

# 用设备当前系统库同步 ROM（本机能看到 /apex、/system 时用；ART 库优先取 APEX）
syncrom:
	python3 ../rom/syncrom.py --rom ../android/assets/demo.rom \
	    --out ../android/assets/demo.rom.new --device-root /
	python3 ../rom/checkrom.py ../android/assets/demo.rom.new --strict

test-mem:
	mkdir -p build
	$(CC) -O2 -Wall -Iinclude -Itests/compat -o build/vm_memtest \
	    tests/vm_memtest.c src/vm_core.c src/vm_rom.c src/vm_binder.c \
	    src/vm_backend_sigsys.c src/vm_loader.c
	./build/vm_memtest
	$(CC) -O2 -Wall -Iinclude -Itests/compat -o build/guardtest \
	    tests/guardtest.c src/vm_backend_sigsys.c src/vm_rom.c src/vm_binder.c src/vm_loader.c
	./build/guardtest
	$(CC) -O2 -Wall -Iinclude -Itests/compat -o build/envtest \
	    tests/envtest.c src/vm_loader.c src/vm_rom.c src/vm_core.c src/vm_binder.c src/vm_backend_sigsys.c
	./build/envtest ../android/assets/demo.rom
	$(CC) -O2 -Wall -Iinclude -Itests/compat -o build/maptest \
	    tests/maptest.c src/vm_core.c src/vm_rom.c src/vm_binder.c src/vm_loader.c src/vm_backend_sigsys.c
	./build/maptest ../android/assets/demo.rom

# 生成过滤器（改 gen_bpf.py 的放行清单后必须重跑，否则直发 syscall 二次 TRAP）
bpf:
	python3 gen_bpf.py

# aarch64 自检宿主（等价 Android JNI stage0 路径；glibc 静态，可在 Termux 直接跑）
# a64_selftest 仅限 aarch64 环境（真机 Termux/arm64 容器）：x86_64 编译会因 mcontext.regs 不存在而失败
selftest_a64:
	mkdir -p build
	gcc -O2 -Wall -Iinclude -Itests/compat -o build/selftest-a64 \
	    tests/a64_selftest.c src/vm_core.c src/vm_rom.c src/vm_loader.c \
	    src/vm_backend_sigsys.c src/vm_binder.c
	gcc -O2 -static -fno-pie -no-pie -pthread -o build/guest-glibc guest/guest_stage1.c
	mkdir -p build/grom/bin build/grom/rom
	cp build/guest-glibc build/grom/bin/guest
	cp ../rom/fixture/rom/hello.txt build/grom/rom/
	python3 $(ROM) pack build/grom build/glibc.rom

clean:
	rm -rf build vmdemo demo.rom

build_prove: guest/guest_prove.c
	mkdir -p build
	$(CC) -O2 -Wall -nostdlib -static -ffreestanding -fno-stack-protector -fno-pic -fno-builtin -no-pie -o $@ $<

prove.rom: build_prove ../rom/fixture/rom/hello.txt
	mkdir -p build/proveroot/bin build/proveroot/rom
	cp ../rom/fixture/rom/hello.txt build/proveroot/rom/
	cp build_prove build/proveroot/bin/guest
	python3 $(ROM) pack build/proveroot $@

# Stage 2.1：双镜像 ROM（父 + exec 目标 child）
build_guest2: guest/guest_stage2.c guest/guest_child.c
	mkdir -p build
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie -o build/parent2 guest/guest_stage2.c
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie -o build/child guest/guest_child.c

stage2.rom: build_guest2 ../rom/fixture/rom/hello.txt
	mkdir -p build/rom2/bin build/rom2/rom
	cp ../rom/fixture/rom/hello.txt build/rom2/rom/
	cp build/parent2 build/rom2/bin/guest
	cp build/child build/rom2/bin/child
	python3 $(ROM) pack build/rom2 $@

run2: vmdemo stage2.rom
	./vmdemo stage2.rom auto

# Stage 2.2：mini-ylinker（x86_64 本地）
build_guest22: guest/guest_stage22.c guest/mini_linker.c guest/hello_so.c
	mkdir -p build
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie \
	    -o build/parent22 guest/guest_stage22.c guest/mini_linker.c
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -shared -fPIC \
	    -o build/libhello.so guest/hello_so.c
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie \
	    -o build/child22 guest/guest_child.c

stage22.rom: build_guest22 ../rom/fixture/rom/hello.txt
	mkdir -p build/rom22/bin build/rom22/rom/lib
	cp ../rom/fixture/rom/hello.txt build/rom22/rom/
	cp build/parent22 build/rom22/bin/guest
	cp build/child22 build/rom22/bin/child
	cp build/libhello.so build/rom22/rom/lib/libhello.so
	python3 $(ROM) pack build/rom22 $@

run22: vmdemo stage22.rom
	./vmdemo stage22.rom ptrace

# Stage 3：binder 三态镜像
build_binder: guest/guest_binder.c
	mkdir -p build
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie \
	    -o build/binder_demo guest/guest_binder.c

stage3.rom: build_binder ../rom/fixture/rom/hello.txt
	mkdir -p build/rom3/bin build/rom3/rom
	cp ../rom/fixture/rom/hello.txt build/rom3/rom/
	cp build/binder_demo build/rom3/bin/guest
	cp build/binder_demo build/rom3/bin/binder_demo
	python3 $(ROM) pack build/rom3 $@

run3: vmdemo stage3.rom
	./vmdemo stage3.rom ptrace

# Stage 4.0：多线程 guest（bionic 静态：pthread+epoll+pipe）
build_stage40: guest/guest_stage40.c
	mkdir -p build
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie \
	    -o build/stage40_demo guest/guest_stage40.c

stage40.rom: build_stage40
	mkdir -p build/rom40/bin
	cp build/stage40_demo build/rom40/bin/guest
	cp build/stage40_demo build/rom40/bin/stage40_demo
	python3 $(ROM) pack build/rom40 $@

run40: vmdemo stage40.rom
	./vmdemo stage40.rom ptrace

# Stage 4.1：ylinker 依赖链（a←b←c 三级 + 主 guest）
build_stage41: guest/guest_stage41.c /tmp/liba.c /tmp/libb.c /tmp/libc.c
	mkdir -p build/so41
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -shared -fPIC -nostdlib \
	    -Wl,-soname,liba.so -o build/so41/liba.so /tmp/liba.c
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -shared -fPIC -nostdlib \
	    -Wl,-soname,libb.so -Lbuild/so41 -o build/so41/libb.so /tmp/libb.c -la
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -shared -fPIC -nostdlib \
	    -Wl,-soname,libc.so -Lbuild/so41 -o build/so41/libc.so /tmp/libc.c -lb
	$(NDKBIN)/clang -target x86_64-linux-android24 -O2 -static -fno-pie -no-pie \
	    -o build/stage41_demo guest/guest_stage41.c guest/mini_linker.c

stage41.rom: build_stage41
	mkdir -p build/rom41/bin build/rom41/rom/lib
	cp build/stage41_demo build/rom41/bin/guest
	cp build/so41/liba.so build/so41/libb.so build/so41/libc.so build/rom41/rom/lib/
	python3 $(ROM) pack build/rom41 $@

run41: vmdemo stage41.rom
	./vmdemo stage41.rom ptrace

# Stage 4.2：动态 PIE 装载协议（glibc ld.so 白鼠）
build/fix42/dynhello: tests/dynhello.c
	mkdir -p build/fix42
	cc -O2 -pie -fPIE -o $@ $<
build/test_loader: tests/test_loader.c src/vm_loader.c
	mkdir -p build
	cc -O2 -g -Wall -Iinclude -o $@ tests/test_loader.c src/vm_loader.c
test42: build/fix42/dynhello build/test_loader
	@echo "== 运行动态 PIE 装载协议测试（ld.so 接管）=="
	./build/test_loader build/fix42/dynhello /lib64/ld-linux-x86-64.so.2; echo "== 退出码 $$?（期望 42）=="
