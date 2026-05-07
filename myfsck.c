#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#define stat mp4_stat
#define dirent mp4_dirent
#include "types.h"
#include "fs.h"
#include "stat.h"
#undef stat
#undef dirent

#include "log.h"

static unsigned char *img;
static size_t img_size;
static superblock *sb;

static uint fs_size;
static uint nblocks;
static uint ninodes;
static uint data_start;
static uint bitmap_start;

static void *block_ptr(uint blockno) {
    return img + (size_t)blockno * BSIZE;
}

static dinode *get_inode(uint inum) {
    return (dinode *)block_ptr(IBLOCK(inum)) + (inum % IPB);
}

/* Return true if blockno lies inside the filesystem data region. */
static int valid_data_block(uint blockno){
    return blockno >= data_start && blockno < fs_size;
}

static int bitmap_used(uint blockno) {
    uchar *bitmap;

    //bitmap block containing its bit
    bitmap = (uchar *)block_ptr(bitmap_start + blockno / BPB);
    //     byte that contains the bit    bit mask within that byte
    return bitmap[(blockno % BPB) / 8] & (1 << (blockno % 8));
}









/* Check that the superblock layout fits within the image. */
static void check_superblock(void) {
    uint inode_blocks;
    uint bitmap_blocks;
    uint used_blocks;

    if (img_size < 2U * BSIZE) { //check for boot block and superblock
        superblock_error();
        exit(1);
    }

    sb = (superblock *)block_ptr(1);

    fs_size = sb->size;
    nblocks = sb->nblocks;
    ninodes = sb->ninodes;

    if (fs_size < 2 || ninodes <= ROOTINO || nblocks > fs_size) {
        superblock_error();
        exit(1);
    }

    // Check that the file system size in the superblock is consistent with the image size
    //Uint64_t cast prevent overflow 
    if ((uint64_t)fs_size * BSIZE > img_size) {
        superblock_error();
        exit(1);
    }

    inode_blocks = ninodes / IPB + 1;
    bitmap_blocks = fs_size / BPB + 1;
    used_blocks = 2 + inode_blocks + bitmap_blocks + nblocks;

    if (fs_size < used_blocks) {
        superblock_error();
        exit(1);
    }

    bitmap_start = 2 + inode_blocks;
    data_start = fs_size - nblocks;
}

/*
    Check that every allocated inode has a valid type. Valid types are
    T_FILE, T_DIR and T_DEV as defined in the filesystem header
 */
static void check_inode_types(void){
    uint i;
    dinode *dip;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type == 0) {
            continue;
        }

        if (dip->type != T_FILE && dip->type != T_DIR &&
            dip->type != T_DEV) {
            bad_inode_error();
            exit(1);
        }
    }
}

/*
    Check that every nonzero inode block pointer is in the data region.
    Verifies direct pointers and the indirect block and its entries
 */
static void check_inode_addresses(void) {
    uint i;
    uint j;
    uint addr;
    uint *indirect;
    dinode *dip;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type == 0) {
            continue;
        }

        for (j = 0; j < NDIRECT; j++) {
            addr = dip->addrs[j];

            //A zero direct pointer means unused slot. A nonzero direct pointer is intended to be a data block, so it must be inside the valid data region. If not, we error
            if (addr != 0 && !valid_data_block(addr)) {
                bad_direct_address_error();
                exit(1);
            }
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) { //if no indirect, skip
            continue;
        }

        if (!valid_data_block(addr)) {
            bad_indirect_address_error();
            exit(1);
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT; j++) {
            if (indirect[j] != 0 && !valid_data_block(indirect[j])) {
                bad_indirect_address_error();
                exit(1);
            }
        }
    }
}

/* Check that directories begin with . and .. entries. */
static void check_directory_format(void)
{
    uint i;
    dinode *dip;
    mp4_dirent *entries;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        if (dip->addrs[0] == 0) { //a directory must have at least one data block for . and .. entries
            bad_directory_format_error();
            exit(1);
        }

        entries = (mp4_dirent *)block_ptr(dip->addrs[0]);

        if (strncmp(entries[0].name, ".", DIRSIZ) != 0 || entries[0].inum != i) {
            bad_directory_format_error();
            exit(1);
        }

        if (strncmp(entries[1].name, "..", DIRSIZ) != 0 || entries[1].inum == 0) {
            bad_directory_format_error();
            exit(1);
        }
    }
}

/*
    For each in use inode, ensure every data block it references (direct and
    indirect) is marked as used in the bitmap. Reports the
    bad_used_address_error if it finds a block referenced by an inode but
    marked free in the bitmap
 */
static void check_inode_blocks_in_bitmap(void) {
    uint i;
    uint j;
    uint addr;
    uint *indirect;
    dinode *dip;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type == 0) {
            continue;
        }

        for (j = 0; j < NDIRECT; j++) {
            addr = dip->addrs[j];

            if (addr != 0 && !bitmap_used(addr)) {
                bad_used_address_error();
                exit(1);
            }
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        if (!bitmap_used(addr)) {
            bad_used_address_error();
            exit(1);
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT; j++) {
            if (indirect[j] != 0 && !bitmap_used(indirect[j])) {
                bad_used_address_error();
                exit(1);
            }
        }
    }
}

/*
    Ensure no bitmap bit is set for data blocks that are not referenced by any inode or indirect block
 */
static void check_bitmap_blocks_in_use(void) {
    uint i;
    uint j;
    uint addr;
    uint *indirect;
    uchar *block_used;
    dinode *dip;

    block_used = calloc(fs_size, sizeof(uchar));
    if (block_used == NULL) {
        exit(1);
    }

    for (i = 0; i < ninodes; i++) { //mark all blocks used by inodes
        dip = get_inode(i);

        if (dip->type == 0) {
            continue;
        }

        for (j = 0; j < NDIRECT; j++) { //marks all direct blocks used per inode
            addr = dip->addrs[j];

            if (addr != 0) {
                block_used[addr] = 1;
            }
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        block_used[addr] = 1; //marks indirect used 
        indirect = (uint *)block_ptr(addr);

        //mark every data block listed inside the indirect block as used by this inode
        for (j = 0; j < NINDIRECT; j++) { 
            if (indirect[j] != 0) {
                block_used[indirect[j]] = 1;
            }
        }
    }

    for (i = data_start; i < fs_size; i++) {
        if (bitmap_used(i) && !block_used[i]) {
            free(block_used);
            bad_bitmap_marked_used_error();
            exit(1);
        }
    }

    free(block_used);
}

/*
    Check that direct block addresses are used by at most one inode. If a
    direct data block is referenced by multiple inodes, report an error
    bad_direct_address_once_error
 */
static void check_direct_addresses_once(void) {
    uint i;
    uint j;
    uint addr;
    uchar *direct_used;
    dinode *dip;

    direct_used = calloc(fs_size, sizeof(uchar));
    if (direct_used == NULL) {
        exit(1);
    }

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        //if the inode is not allocated, skip it
        if (dip->type == 0) {
            continue;
        }

        //check each direct block address of the inode
        for (j = 0; j < NDIRECT; j++) {
            addr = dip->addrs[j]; 

            if (addr == 0) { //unused
                continue;
            }
            //if direct block already used by another inode, error
            if (direct_used[addr]) {
                free(direct_used);
                bad_direct_address_once_error();
                exit(1);
            }
            //mark used
            direct_used[addr] = 1;
        }
    }

    free(direct_used);
}

/*
    Verify that each inodes size field follows with the number of
    allocated data blocks
 */
static void check_inode_file_sizes(void) {
    uint i;
    uint j;
    uint addr;
    uint data_blocks;
    uint *indirect;
    uint64_t min_size;
    uint64_t max_size;
    dinode *dip;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type == 0) {
            continue;
        }

        data_blocks = 0; 

        //count num direct blocks
        for (j = 0; j < NDIRECT; j++) {
            if (dip->addrs[j] != 0) {
                data_blocks++;
            }
        }

        addr = dip->addrs[NDIRECT];

        //count num indirect blocks 
        if (addr != 0) {
            indirect = (uint *)block_ptr(addr);

            for (j = 0; j < NINDIRECT; j++) {
                if (indirect[j] != 0) {
                    data_blocks++;
                }
            }
        }

        if (data_blocks == 0) {
            if (dip->size != 0) {
                bad_file_size_error();
                exit(1);
            }

            continue;
        }

        min_size = (uint64_t)(data_blocks - 1) * BSIZE;
        max_size = (uint64_t)data_blocks * BSIZE;

        if ((uint64_t)dip->size <= min_size ||
            (uint64_t)dip->size > max_size) {
            bad_file_size_error();
            exit(1);
        }
    }
}

/*
    Ensure every allocated inode (except root) is referenced by at least one
    directory entry. If not, report bad_used_inode_not_found_error
 */
static void check_inodes_referenced(void) {
    uint i;
    uint j;
    uint k;
    uint addr;
    uint remaining;
    uint block_bytes;
    uint entries_in_block;
    uint *indirect;
    uint *inode_refs;
    dinode *dip;
    mp4_dirent *entries;

    inode_refs = calloc(ninodes, sizeof(uint));
    if (inode_refs == NULL) {
        exit(1);
    }

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        remaining = dip->size;

        for (j = 0; j < NDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = dip->addrs[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) { //empty directory entry
                        continue;
                    }
                    //exclude . and .. entries
                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 || strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }
                    
                    if (entries[k].inum < ninodes) {
                        inode_refs[entries[k].inum]++;
                    }
                }
            }

            remaining -= block_bytes;
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = indirect[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 ||
                        strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        inode_refs[entries[k].inum]++;
                    }
                }
            }

            remaining -= block_bytes;
        }
    }

    for (i = 1; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type == 0 || i == ROOTINO) {
            continue;
        }
        
        //if the inode is allocated but not referenced by any directory entry, error
        if (inode_refs[i] == 0) {
            free(inode_refs);
            bad_used_inode_not_found_error();
            exit(1);
        }
    }

    free(inode_refs);
}

/*
    Check each directory entry in every directory and ensure that the inode
    number it refers to is inside range and the inode is marked allocated.
    Reports  bad_inode_referred_marked_error  when a directory points to a
    free or out of range inode
 */
static void check_dir_entries_inode_allocated(void) {
    uint i;
    uint j;
    uint k;
    uint addr;
    uint remaining;
    uint block_bytes;
    uint entries_in_block;
    uint *indirect;
    dinode *dip;
    mp4_dirent *entries;

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        remaining = dip->size;

        for (j = 0; j < NDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = dip->addrs[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (entries[k].inum >= ninodes || get_inode(entries[k].inum)->type == 0) {
                        bad_inode_referred_marked_error();
                        exit(1);
                    }
                }
            }

            remaining -= block_bytes;
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = indirect[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (entries[k].inum >= ninodes || get_inode(entries[k].inum)->type == 0) {
                        bad_inode_referred_marked_error();
                        exit(1);
                    }
                }
            }

            remaining -= block_bytes;
        }
    }
}

/*
    Count directory references to regular files and compare with the inodes
    nlink field. If counts mismatch, report bad_reference_count_error
 */
static void check_file_reference_counts(void) {
    uint i;
    uint j;
    uint k;
    uint addr;
    uint remaining;
    uint block_bytes;
    uint entries_in_block;
    uint *indirect;
    uint *file_refs;
    dinode *dip;
    dinode *target;
    mp4_dirent *entries;

    file_refs = calloc(ninodes, sizeof(uint));
    if (file_refs == NULL) {
        exit(1);
    }

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        remaining = dip->size;

        for (j = 0; j < NDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = dip->addrs[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_FILE) {
                            file_refs[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = indirect[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_FILE) {
                            file_refs[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }
    }

    for (i = 1; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_FILE) {
            continue;
        }

        if ((uint)dip->nlink != file_refs[i]) {
            free(file_refs);
            bad_reference_count_error();
            exit(1);
        }
    }

    free(file_refs);
}

/*
    Ensure each directory appears as a child in exactly one other directory
    (excluding the root). If a directory appears more than once report
     bad_directory_once_error
 */
static void check_directories_once(void) {
    uint i;
    uint j;
    uint k;
    uint addr;
    uint remaining;
    uint block_bytes;
    uint entries_in_block;
    uint *indirect;
    uint *dir_refs;
    dinode *dip;
    dinode *target;
    mp4_dirent *entries;

    dir_refs = calloc(ninodes, sizeof(uint));
    if (dir_refs == NULL) {
        exit(1);
    }

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        remaining = dip->size;

        for (j = 0; j < NDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = dip->addrs[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 ||
                        strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_DIR) {
                            dir_refs[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = indirect[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 ||
                        strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_DIR) {
                            dir_refs[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }
    }

    for (i = 1; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }
        //the root directory should not be referenced by any other directory
        if (i == ROOTINO) {
            if (dir_refs[i] > 0) {
                free(dir_refs);
                bad_directory_once_error();
                exit(1);
            }

            continue;
        }
        //if a directory is referenced by more than one directory, error
        if (dir_refs[i] > 1) {
            free(dir_refs);
            bad_directory_once_error();
            exit(1);
        }
    }

    free(dir_refs);
}

/*
    verify that each directory's ".." entry matches the
    parent discovered by scanning directory entries (and that there are no
    multiple parents), reports bad_parent_directory_error
 */
static void check_parent_directories(void) {
    uint i;
    uint j;
    uint k;
    uint addr;
    uint remaining;
    uint block_bytes;
    uint entries_in_block;
    uint *indirect;
    uint *parent_of;
    uint *parent_count;
    dinode *dip;
    dinode *target;
    mp4_dirent *entries;

    parent_of = calloc(ninodes, sizeof(uint));
    if (parent_of == NULL) {
        exit(1);
    }

    parent_count = calloc(ninodes, sizeof(uint));
    if (parent_count == NULL) {
        free(parent_of);
        exit(1);
    }

    for (i = 0; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        remaining = dip->size;

        for (j = 0; j < NDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = dip->addrs[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 ||
                        strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_DIR) {
                            parent_of[entries[k].inum] = i;
                            parent_count[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }

        addr = dip->addrs[NDIRECT];

        if (addr == 0) {
            continue;
        }

        indirect = (uint *)block_ptr(addr);

        for (j = 0; j < NINDIRECT && remaining > 0; j++) {
            if (remaining > BSIZE) {
                block_bytes = BSIZE;
            } else {
                block_bytes = remaining;
            }
            entries_in_block = block_bytes / sizeof(mp4_dirent);

            addr = indirect[j];

            if (addr != 0) {
                entries = (mp4_dirent *)block_ptr(addr);

                for (k = 0; k < entries_in_block; k++) {
                    if (entries[k].inum == 0) {
                        continue;
                    }

                    if (strncmp(entries[k].name, ".", DIRSIZ) == 0 ||
                        strncmp(entries[k].name, "..", DIRSIZ) == 0) {
                        continue;
                    }

                    if (entries[k].inum < ninodes) {
                        target = get_inode(entries[k].inum);

                        if (target->type == T_DIR) {
                            parent_of[entries[k].inum] = i;
                            parent_count[entries[k].inum]++;
                        }
                    }
                }
            }

            remaining -= block_bytes;
        }
    }

    for (i = 1; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        entries = (mp4_dirent *)block_ptr(dip->addrs[0]);

        if (i == ROOTINO) {
            if (entries[1].inum != ROOTINO) {
                free(parent_count);
                free(parent_of);
                bad_parent_directory_error();
                exit(1);
            }

            continue;
        }

        if (parent_count[i] != 1 || entries[1].inum != parent_of[i]) {
            free(parent_count);
            free(parent_of);
            bad_parent_directory_error();
            exit(1);
        }
    }

    free(parent_count);
    free(parent_of);
}

/* Check that every directory reaches root through parent links */
static void check_directories_reach_root(void) {
    uint i;
    uint cur;
    uint steps;
    dinode *dip;
    mp4_dirent *entries;

    for (i = 1; i < ninodes; i++) {
        dip = get_inode(i);

        if (dip->type != T_DIR) {
            continue;
        }

        cur = i;
        steps = 0;

        while (cur != ROOTINO) {
            if (steps >= ninodes) {
                bad_directory_error();
                exit(1);
            }

            dip = get_inode(cur);

            if (dip->type != T_DIR || dip->addrs[0] == 0) {
                bad_directory_error();
                exit(1);
            }

            entries = (mp4_dirent *)block_ptr(dip->addrs[0]);

            if (entries[1].inum == 0 || entries[1].inum >= ninodes) {
                bad_directory_error();
                exit(1);
            }

            cur = entries[1].inum;
            steps++;
        }
    }
}

int main(int argc, char *argv[]) {
    int fd;
    struct stat st;
    void *mapped;

    if (argc != 2) {
        fprintf(stderr, "Usage: myfsck <file_system_image>\n");
        return 1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "image not found.\n");
        return 1;
    }

    if (fstat(fd, &st) < 0) {
        close(fd);
        fprintf(stderr, "image not found.\n");
        return 1;
    }

    if (st.st_size <= 0) {
        close(fd);
        superblock_error();
        return 1;
    }

    img_size = (size_t)st.st_size;

    mapped = mmap(NULL, img_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapped == MAP_FAILED) {
        fprintf(stderr, "image not found.\n");
        return 1;
    }

    img = mapped;

    check_superblock();
    check_inode_types();
    check_inode_addresses();
    check_directory_format();
    check_inode_blocks_in_bitmap();
    check_bitmap_blocks_in_use();
    check_direct_addresses_once();
    check_inode_file_sizes();
    check_inodes_referenced();
    check_dir_entries_inode_allocated();
    check_file_reference_counts();
    check_directories_once();
    check_parent_directories();
    check_directories_reach_root();

    munmap(img, img_size);
    return 0;
}
    // You can exit right after reporting the first error in normal mode
    // Don't print anything in stderr except error messages defined in log.h
    // return 1 if any errors are detected, otherwise return 0