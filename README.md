# MP4 README

Name: Josh Jenks  
NetID: jajenks2

## Implementation Overview

For this MP, I implemented `myfsck.c` as a user space file system checker for the provided file system image format. The checker opens the image, uses `mmap()` to map it into memory, reads the superblock from block 1, and then uses the structures in `fs.h` to interpret inodes, directory entries, indirect blocks, and the bitmap.

The checks are performed in the same general order as the MP4 document. When an error is found, the program calls the matching error function from `log.h` and exits with return code 1. If no errors are found, the program prints nothing and returns 0.

## Base Checks

**Superblock check:**  
The checker verifies that the superblock describes a valid layout. It checks that the image is large enough for the boot block, superblock, inode blocks, bitmap blocks, and data blocks. It also verifies that the file system size from the superblock fits within the actual image size.

**Inode type check:**  
The checker scans all inodes and verifies that each inode is either unallocated or has one of the valid types defined in `stat.h`: `T_FILE`, `T_DIR`, or `T_DEV`.

**Address validity check:**  
For every allocated inode, the checker verifies that every nonzero direct block address points within the valid data block range. It also checks that the indirect block pointer is valid if present, and that every nonzero address inside the indirect block is also valid.

**Directory format check:**  
For every directory inode, the checker verifies that the first two directory entries are `.` and `..`. It also verifies that the `.` entry points back to the directory’s own inode.

**Inode block bitmap check:**  
The checker verifies that every block referenced by an inode is marked used in the bitmap. This includes direct blocks, the indirect block itself, and blocks referenced from inside the indirect block.

**Bitmap block usage check:**  
The checker reconstructs which data blocks are actually referenced by inodes and indirect blocks. It then scans the data block region of the bitmap and verifies that every data block marked used is actually referenced somewhere in the file system.

**Duplicate direct address check:**  
The checker tracks all direct block addresses used by allocated inodes and reports an error if the same direct block address is used more than once. This check only applies to direct addresses.

**File size check:**  
The checker counts the number of data blocks used for storage by each allocated inode. Direct blocks and blocks pointed to by the indirect block count as storage blocks, but the indirect block itself does not. The inode size must fit within the valid range for the number of storage blocks used.

**Allocated inode reference check:**  
The checker scans all directory entries and counts ordinary references to inodes. It excludes `.` and `..` from this check. Every allocated inode except the root inode must be referenced by at least one ordinary directory entry.

**Directory entry inode check:**  
The checker scans all directory entries and verifies that every nonempty entry points to a valid allocated inode.

**File reference count check:**  
The checker counts how many directory entries refer to each regular file inode. It then compares this count with the inode’s `nlink` value and reports an error if they do not match.

**Directory duplicate link check:**  
The checker counts ordinary directory entries that point to directory inodes, excluding `.` and `..`. The root directory is handled specially, and every non-root directory is allowed to appear as a child directory only once.

## Extra Credit Checks

**Parent directory check:**  
I implemented the extra credit check for parent directory consistency. The checker reconstructs each directory’s actual parent by scanning ordinary directory entries that point to subdirectories. It then verifies that the child directory’s `..` entry points back to that actual parent. For the root directory, `..` must point to root.

**Directory reachability check:**  
I also implemented the extra credit check that every directory traces back to root. For each directory, the checker follows the `..` chain until it reaches the root. If the chain does not reach root or loops too many times, the checker reports an inaccessible directory error.

## Notes

The implementation does not assume there is only one bitmap block. Bitmap lookups use the block number to calculate which bitmap block contains the corresponding bit.

The implementation also separates address validity from bitmap consistency. Check 3 only verifies that nonzero inode block pointers are valid data block addresses. Bitmap correctness is handled separately by checks 5 and 6.

The checker exits immediately after the first detected error, as allowed by the MP4 specification.