/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _SFS_H_
#define _SFS_H_

// The actual transaction types
#define TRANS_WRITE 0
#define TRANS_TRUNCATE 1
#define TRANS_CREAT 2
#define TRANS_LINK 3
#define TRANS_MKDIR 4
#define TRANS_RMDIR 5
#define TRANS_REMOVE 6
#define TRANS_RENAME 7
#define TRANS_RECLAIM 8


/*
 * Header for SFS, the Simple File System.
 */


struct buf; /* in buf.h */

/*
 * Get abstract structure definitions
 */
#include <fs.h>
#include <vnode.h>

/*
 * Get on-disk structures and constants that are made available to
 * userland for the benefit of mksfs, dumpsfs, etc.
 */
#include <kern/sfs.h>

/*
 * In-memory inode
 */
struct sfs_vnode {
	struct vnode sv_absvn;          /* abstract vnode structure */
	uint32_t sv_ino;                /* inode number */
	unsigned sv_type;		/* cache of sfi_type */
	struct buf *sv_dinobuf;		/* buffer holding dinode */
	uint32_t sv_dinobufcount;	/* # times dinobuf has been loaded */
	struct lock *sv_lock;		/* lock for vnode */
};

/*
 * In-memory info for a whole fs volume
 */
struct sfs_fs {
	struct fs sfs_absfs;            /* abstract filesystem structure */
	struct sfs_superblock sfs_sb;	/* copy of on-disk superblock */
	bool sfs_superdirty;            /* true if superblock modified */
	struct device *sfs_device;      /* device mounted on */
	struct vnodearray *sfs_vnodes;  /* vnodes loaded into memory */
	struct bitmap *sfs_freemap;     /* blocks in use are marked 1 */
	bool sfs_freemapdirty;          /* true if freemap modified */
	struct lock *sfs_vnlock;	/* lock for vnode table */
	struct lock *sfs_freemaplock;	/* lock for freemap/superblock */
	struct lock *sfs_renamelock;	/* lock for sfs_rename() */

	struct sfs_jphys *sfs_jphys;	/* physical journal container */

	struct array *sfs_transactions;
	uint64_t newest_freemap_lsn;	/* most recent lsn of an operation modifying the freemap */
	uint64_t oldest_freemap_lsn;	/* oldest unwritten lsn of an operation modifying the freemap */
	struct lock *trans_lock;
};

// We probably want to have 2 functions for transaction begin
// and transaction commit. Which will modify the transction
// table and assign the pids.
struct trans {
	int id;
	unsigned first_lsn;
};

/*
 * Function for mounting a sfs (calls vfs_mount)
 */
int sfs_mount(const char *device);

int sfs_trans_begin(struct sfs_fs* sfs, int trans_type);
int sfs_trans_commit(struct sfs_fs* sfs, int trans_type);
int sfs_checkpoint(struct sfs_fs* sfs);

#endif /* _SFS_H_ */
