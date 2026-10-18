/* TwinVM ROM 镜像 v1 — 对齐 VMOS libvmkrnfs 实测结构：
 * 固定头 + inode 表（路径/权限/大小/数据偏移/crc32/xattr 槽）+ 顺序数据块。
 * ROM 是 guest 的"磁盘"：只读、可校验、可整包分发（VMOS 的 ROM 生态同思路）。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define VM_ROM_MAGIC "TVMROM1"
#define VM_ROM_HDR_SIZE 64
#define VM_ROM_INODE_DISK_SIZE 292  /* 256+4+4+4+8+8+4+4 */

/* 磁盘头（小端）：
 * 0x00 magic[8]="TVMROM1\0"  0x08 u32 inode_count  0x0C u32 inode_size
 * 0x10 u64 data_off          0x18 u64 data_size    0x20 u32 file_crc(数据+inode)
 * 0x24..0x3F 保留 0 */
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

uint32_t vm_crc32(uint32_t crc, const uint8_t* buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

int vm_rom_load_mem(VmRom* rom, const uint8_t* data, size_t size) {
    memset(rom, 0, sizeof(*rom));
    if (size < VM_ROM_HDR_SIZE || memcmp(data, VM_ROM_MAGIC, 7) != 0) return -1;
    uint32_t icount = rd32(data + 8);
    uint32_t isize  = rd32(data + 12);
    uint64_t data_off = rd64(data + 0x10);
    if (isize != VM_ROM_INODE_DISK_SIZE || icount == 0 || icount > 4096) return -2;
    if (data_off > size || (uint64_t)VM_ROM_HDR_SIZE + (uint64_t)icount * isize > data_off) return -3;
    rom->inodes = (VmRomInode*)calloc(icount, sizeof(VmRomInode));
    if (!rom->inodes) return -4;
    for (uint32_t i = 0; i < icount; i++) {
        const uint8_t* in = data + VM_ROM_HDR_SIZE + (size_t)i * isize;
        VmRomInode* out = &rom->inodes[i];
        memcpy(out->name, in, VM_MAX_PATH);
        out->name[VM_MAX_PATH - 1] = 0;
        out->mode = rd32(in + 256);
        out->uid  = rd32(in + 260);
        out->gid  = rd32(in + 264);
        out->size = rd64(in + 268);
        out->crc32 = rd32(in + 284);
        out->xattr_len = rd32(in + 288);
        uint64_t off = rd64(in + 276);
        if (off + out->size > size) { free(rom->inodes); rom->inodes = NULL; return -5; }
        out->map = data + off;
    }
    rom->base = (uint8_t*)data;   /* 非拥有，仅引用 */
    rom->size = size;
    rom->inode_count = icount;
    rom->file_crc = rd32(data + 0x20);
    return 0;
}

int vm_rom_load(VmRom* rom, const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -10;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < VM_ROM_HDR_SIZE) { close(fd); return -11; }
    size_t sz = (size_t)st.st_size;
    void* mapped = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) return -12;
    int r = vm_rom_load_mem(rom, (const uint8_t*)mapped, sz);
    if (r != 0) { munmap(mapped, sz); return r; }
    /* 校验每个 inode 的 crc32（装载即验，ROM 完整性红线） */
    for (uint32_t i = 0; i < rom->inode_count; i++) {
        if (vm_crc32(0, rom->inodes[i].map, rom->inodes[i].size) != rom->inodes[i].crc32) {
            vm_rom_close(rom);
            return -20;  /* 数据损坏 */
        }
    }
    return 0;
}

void vm_rom_close(VmRom* rom) {
    if (rom->base) munmap(rom->base, rom->size);
    free(rom->inodes);
    memset(rom, 0, sizeof(*rom));
}

const VmRomInode* vm_rom_find(const VmRom* rom, const char* path) {
    for (uint32_t i = 0; i < rom->inode_count; i++)
        if (strncmp(rom->inodes[i].name, path, VM_MAX_PATH) == 0) return &rom->inodes[i];
    return NULL;
}
