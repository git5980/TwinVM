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

/* ────────────────────────────────────────────────────────────────────────
 * v1.2.111 ★★★ vm_rom_install：运行期向 ROM 追加文件（installApk 引擎层重写）
 *
 * v1.2.109 的 JNI 内嵌版把 inode 表写错位（表在 64+count*292，误写 data_off 处
 * → 表区/数据区双损坏 → 真机 -5/-20 实锤）。本版布局严格按 rom_add.py：
 *
 *   [0,64) header   [64, 64+N*292) inode表   [data_off, data_off+dsize) 数据区
 *
 * 追加一个文件 = 表插一行（数据区整体后移 292）+ 数据区尾对齐追加。
 * file_crc@0x20 按 rom_add.py 语义重算（blob 顺序拼接的 zlib crc32——
 * vm_rom_load 不校验它，但保持一致性）。
 * 原子落盘（.tmp + rename）。返回 0 成功；out_name 回写实际安装路径。
 * ──────────────────────────────────────────────────────────────────────── */

static uint32_t rom_crc32_cat(uint32_t crc, const uint8_t* b, size_t n) {
    /* zlib 语义增量 crc32：crc32(a||b) = cat(cat(a), b)，cat(0,x)=crc32(x) */
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

int vm_rom_install(const char* src_path, const char* rom_path,
                   const char* rom_dir, char* out_name, size_t out_name_sz) {
    /* 1. 读源文件 */
    FILE* sf = fopen(src_path, "rb");
    if (!sf) return -1;
    fseek(sf, 0, SEEK_END);
    long ssz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    if (ssz <= 0) { fclose(sf); return -2; }
    uint8_t* sdata = (uint8_t*)malloc((size_t)ssz);
    if (!sdata || fread(sdata, 1, (size_t)ssz, sf) != (size_t)ssz) {
        free(sdata); fclose(sf); return -3;
    }
    fclose(sf);

    /* 2. 读整个 ROM */
    FILE* rf = fopen(rom_path, "rb");
    if (!rf) { free(sdata); return -4; }
    fseek(rf, 0, SEEK_END);
    long rsz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    if (rsz < VM_ROM_HDR_SIZE) { fclose(rf); free(sdata); return -5; }
    uint8_t* body = (uint8_t*)malloc((size_t)rsz);
    if (!body || fread(body, 1, (size_t)rsz, rf) != (size_t)rsz) {
        if (body) free(body); fclose(rf); free(sdata); return -6;
    }
    fclose(rf);

    if (memcmp(body, VM_ROM_MAGIC, 7) != 0) { free(body); free(sdata); return -7; }
    uint32_t count = rd32(body + 8);
    uint32_t isize = rd32(body + 12);
    uint64_t data_off = rd64(body + 0x10);
    uint64_t data_size = rd64(body + 0x18);
    if (isize != VM_ROM_INODE_DISK_SIZE || count == 0 || count >= 4096) {
        free(body); free(sdata); return -8;
    }
    size_t tbl = (size_t)count * VM_ROM_INODE_DISK_SIZE;
    if ((uint64_t)VM_ROM_HDR_SIZE + tbl > data_off ||
        data_off + data_size > (uint64_t)rsz) {
        free(body); free(sdata); return -9;
    }

    /* 3. 定新名：rom_dir 下 install-<已有同前缀数+1> */
    char newname[VM_MAX_PATH];
    {
        /* v1.2.111 三坑：前缀 = rom_dir + "/install-"（完整拼接再比，
         * 首版漏了分隔符 → 连装两次都叫 install-1 → 后者被前者遮蔽） */
        char pfx[VM_MAX_PATH];
        snprintf(pfx, sizeof(pfx), "%s/install-", rom_dir);
        size_t pl = strlen(pfx);
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; i++) {
            const char* nm = (const char*)(body + VM_ROM_HDR_SIZE + (size_t)i * isize);
            if (strncmp(nm, pfx, pl) == 0) n++;
        }
        snprintf(newname, sizeof(newname), "%s/install-%u.apk", rom_dir, n + 1);
    }
    if (out_name) snprintf(out_name, out_name_sz, "%s", newname);

    /* 4. 新布局
     *   表扩 292（数据区整体后移 292）→ 新 inode 放 [64+tbl, 64+tbl+292)
     *   数据区尾（逻辑尾）16 对齐处追加源文件 */
    uint64_t new_data_off = data_off + VM_ROM_INODE_DISK_SIZE;
    /* v1.2.111 二坑修复：追加点从【移位后】的数据区尾起算（旧逻辑尾+292）——
     * 首版用旧逻辑尾 → APK 写进旧数据区最后 292 字节里（自踩）→ -20 */
    uint64_t new_data_end = new_data_off + data_size;     /* 移位后数据尾 */
    uint64_t append_at = (new_data_end + 15) & ~(uint64_t)0xF;
    uint64_t new_total = append_at + (uint64_t)ssz;

    uint8_t* nb = (uint8_t*)calloc(1, (size_t)new_total);
    if (!nb) { free(body); free(sdata); return -10; }

    /* 5.1 header 原样拷（64B）——后面 patch */
    memcpy(nb, body, VM_ROM_HDR_SIZE);
    /* 5.2 旧表原位不动：[64, 64+tbl)——但每个 inode 的 off 要随数据区后移 +292
     *     （v1.2.111 首版又踩坑：只搬数据不改账本 → 真机 -20 数据 CRC 炸） */
    memcpy(nb + VM_ROM_HDR_SIZE, body + VM_ROM_HDR_SIZE, tbl);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* ino = nb + VM_ROM_HDR_SIZE + (size_t)i * VM_ROM_INODE_DISK_SIZE;
        uint64_t o = rd64(ino + 276);
        o += VM_ROM_INODE_DISK_SIZE;
        memcpy(ino + 276, &o, 8);
    }
    /* 5.3 旧数据整体后移 292：body[data_off..] → nb[new_data_off..] */
    memcpy(nb + (size_t)new_data_off, body + (size_t)data_off, (size_t)data_size);
    /* 5.4 新 inode：[64+tbl, 64+tbl+292) */
    {
        uint8_t* ino = nb + VM_ROM_HDR_SIZE + tbl;
        memset(ino, 0, VM_ROM_INODE_DISK_SIZE);
        size_t nl = strlen(newname);
        if (nl >= VM_MAX_PATH) nl = VM_MAX_PATH - 1;
        memcpy(ino, newname, nl);
        uint32_t mode = 0100644, uid = 1000, gid = 1000, xlen = 0;
        uint64_t fsz = (uint64_t)ssz, foff = append_at;
        uint32_t fcrc = (uint32_t)rom_crc32_cat(0, sdata, (size_t)ssz);
        memcpy(ino + 256, &mode, 4); memcpy(ino + 260, &uid, 4);
        memcpy(ino + 264, &gid, 4); memcpy(ino + 268, &fsz, 8);
        memcpy(ino + 276, &foff, 8); memcpy(ino + 284, &fcrc, 4);
        memcpy(ino + 288, &xlen, 4);
    }
    /* 5.5 追加源数据 */
    memcpy(nb + (size_t)append_at, sdata, (size_t)ssz);
    /* 5.6 patch header：count+1 / 新 data_off / 新 dsize / file_crc */
    {
        uint32_t new_count = count + 1;
        uint64_t new_dsize = new_total - new_data_off;
        memcpy(nb + 8, &new_count, 4);
        memcpy(nb + 0x10, &new_data_off, 8);
        memcpy(nb + 0x18, &new_dsize, 8);
        /* file_crc = 依 inode 顺序 blob 拼接的 zlib crc32（rom_add.py 语义） */
        uint32_t run = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* in = body + VM_ROM_HDR_SIZE + (size_t)i * isize;
            uint64_t off = rd64(in + 276), len = rd64(in + 268);
            run = rom_crc32_cat(run, body + (size_t)off, (size_t)len);
        }
        run = rom_crc32_cat(run, sdata, (size_t)ssz);
        memcpy(nb + 0x20, &run, 4);
    }

    /* 6. 原子落盘 */
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s.tmp", rom_path);
        FILE* wf = fopen(tmp, "wb");
        if (!wf) { free(nb); free(body); free(sdata); return -11; }
        size_t wt = fwrite(nb, 1, (size_t)new_total, wf);
        fclose(wf);
        if (wt != (size_t)new_total) { remove(tmp); free(nb); free(body); free(sdata); return -12; }
        if (rename(tmp, rom_path) != 0) { remove(tmp); free(nb); free(body); free(sdata); return -13; }
    }

    free(nb); free(body); free(sdata);
    return 0;
}
