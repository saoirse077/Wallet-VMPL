
# KERNELDIR := $(shell nix build --print-out-paths "/etc/nixos/#nixosConfigurations.ryan.config.boot.kernelPackages.kernel.dev")

# .PHONY: kvm unload_kvm load_kvm

# kvm: 
# 	make -C ${KERNELDIR}/lib/modules/*/build M=${CURDIR}/kvm/ -j64

# unload_kvm: 
# 	sudo rmmod kvm_amd
# 	sudo rmmod kvm

# load_kvm:
# 	sudo insmod kvm/kvm.ko
# 	sudo insmod kvm/kvm-amd.ko


# 自动检测：NixOS 使用 nix build，其他系统使用当前内核
ifeq ($(shell test -d /etc/nixos && echo yes),yes)
    KERNELDIR := $(shell nix build --print-out-paths "/etc/nixos/#nixosConfigurations.ryan.config.boot.kernelPackages.kernel.dev")
    KBUILD_DIR := ${KERNELDIR}/lib/modules/*/build
else
    # 非 NixOS 系统：使用当前运行内核的构建目录
    KBUILD_DIR := /lib/modules/$(shell uname -r)/build
endif

.PHONY: kvm unload_kvm load_kvm

kvm: 
	make -C ${KBUILD_DIR} M=${CURDIR}/kvm/ -j64

unload_kvm: 
	-sudo rmmod kvm_amd 2>/dev/null || true
	-sudo rmmod kvm 2>/dev/null || true

load_kvm:
	sudo insmod kvm/kvm.ko
	sudo insmod kvm/kvm-amd.ko