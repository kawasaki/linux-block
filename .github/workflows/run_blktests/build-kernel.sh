#!/bin/bash
set -e
set -x

mkdir -p /linux
cd /linux
git init
git remote add origin $KERNEL_TREE
git fetch origin --depth=5 "${KERNEL_COMMIT_SHA}"
git reset --hard "${KERNEL_COMMIT_SHA}"
git log -1

cp /base-kernel-config /linux/.config
yes "" | make olddefconfig
./scripts/config --enable CONFIG_BTRFS_FS
./scripts/config --enable CONFIG_BTRFS_FS_POSIX_ACL
./scripts/config --enable CONFIG_PSI
./scripts/config --enable CONFIG_MEMCG
./scripts/config --enable CONFIG_CRYPTO_LZO
./scripts/config --enable CONFIG_ZRAM
./scripts/config --enable CONFIG_ZRAM_DEF_COMP_LZORLE
./scripts/config --enable CONFIG_ISO9660_FS
./scripts/config --enable CONFIG_VFAT_FS
./scripts/config --enable CONFIG_NET_9P
./scripts/config --enable CONFIG_NET_9P_VIRTIO
./scripts/config --enable CONFIG_9P_FS
./scripts/config --enable CONFIG_9P_FS_POSIX_ACL
./scripts/config --enable CONFIG_VIRTIO
./scripts/config --enable CONFIG_VIRTIO_PCI
./scripts/config --enable CONFIG_PCI
./scripts/config --enable CONFIG_VIRTIO_BLK
./scripts/config --enable CONFIG_VIRTIO_NET
./scripts/config --enable CONFIG_VIRTIO_FS
./scripts/config --enable CONFIG_IKCONFIG
./scripts/config --enable CONFIG_IKCONFIG_PROC
./scripts/config --enable CONFIG_BLK_DEV_NULL_BLK
./scripts/config --enable CONFIG_BLK_DEV_ZONED
./scripts/config --enable CONFIG_F2FS_FS
./scripts/config --enable CONFIG_KASAN
./scripts/config --enable CONFIG_PROVE_LOCKING
./scripts/config --enable CONFIG_DEBUG_KERNEL
./scripts/config --enable CONFIG_LOCK_DEBUGGING_SUPPORT
make olddefconfig

make -j$(nproc)

#initramfs creation inspired by https://github.com/floatious/boot-scripts/blob/master/old/boot
mkdir -p /tmp/initramfs
cd /tmp/initramfs
gzip -dc /base-initramfs.cpio.gz | cpio -id
#Replace kernel modules
rm -rf lib/modules/*
cd /linux
make -j$(nproc) -s modules_install INSTALL_MOD_PATH=/tmp/initramfs INSTALL_MOD_STRIP=1
cd /tmp/initramfs
find . | cpio -o -H newc | gzip -c > /initramfs.cpio.gz

cd /linux
mkdir -p /version
echo "KERNEL_VERSION=$(git rev-parse --short=12 HEAD)" > /version/tag
source /version/tag

cp arch/x86_64/boot/bzImage /output/bzImage-${KERNEL_VERSION}
chmod 755 /output/bzImage-${KERNEL_VERSION}

cp /initramfs.cpio.gz /output/initramfs-${KERNEL_VERSION}.img
chmod 644 /output/initramfs-${KERNEL_VERSION}.img

#The generated initramfs from the following commands seem to not inculde all
#nessesary binaries -> The VM won't be able to mount the virtiofs that contains
#the jitconfig for the GitHub runner.
# cd /linux
# make modules_install
# make install
#
# cp /boot/vmlinuz* /output/vmlinuz-${KERNEL_VERSION}
# chmod 755 /output/vmlinuz-${KERNEL_VERSION}
# cp /boot/initr* /output/initramfs-${KERNEL_VERSION}.img
# chmod 644 /output/initramfs-${KERNEL_VERSION}.img
