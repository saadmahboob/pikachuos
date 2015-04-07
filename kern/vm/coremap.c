#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <vm.h>
#include <coremap.h>
#include <bitmap.h>
#include <uio.h>
#include <vnode.h>
#include <vfs.h>
#include <kern/fcntl.h>

#define PAGE_LINEAR

#define CM_TO_PADDR(i) ((paddr_t)PAGE_SIZE * (i + cm_base))
#define PADDR_TO_CM(paddr)  ((paddr / PAGE_SIZE) - cm_base)

static struct cm_entry *coremap;
static struct spinlock busy_lock = SPINLOCK_INITIALIZER;

static int cm_entries;
static int cm_base;

static int cm_used;

static int evict_hand = 0;

struct vnode *bs_file;
struct bitmap *bs_map;
struct lock *bs_map_lock;

void cm_bootstrap(void) {
	int i;
    paddr_t mem_start, mem_end;
    uint32_t npages, cm_size;
    (void) &busy_lock;
    (void) evict_hand;

    // get size of memory
    mem_end = ram_getsize();
    mem_start = ram_getfirstfree();

    // assert alignment 
    KASSERT((mem_end & PAGE_FRAME) == mem_end);
    KASSERT((mem_start & PAGE_FRAME) == mem_start);

    // # of pages we need
    npages = (mem_end - mem_start) / PAGE_SIZE;

    // total size of cm
	cm_size = npages * sizeof(struct cm_entry);
	cm_size = ROUNDUP(cm_size, PAGE_SIZE);
	KASSERT((cm_size & PAGE_FRAME) == cm_size);

	// this is kinda strange, we may end up having unused cormap space.
	ram_stealmem(cm_size / PAGE_SIZE);
    mem_start += cm_size / PAGE_SIZE;

	cm_entries = (mem_end - mem_start) / PAGE_SIZE;
	cm_base = mem_start / PAGE_SIZE;
    cm_used = 0;

    // TODO: Can be replaced with memset
	for (i=0; i<(int)cm_entries; i++) {
        coremap[i].vm_addr = 0;
        coremap[i].busy = 0;
        coremap[i].pid = 0; // Tianyu, 0 PID is invalid. Kernel PID is 1. See kern\include\proc.h
        coremap[i].is_kernel = 0;
        coremap[i].allocated = 0;
        coremap[i].has_next = 0;
        coremap[i].dirty = 0;
        coremap[i].used_recently = 0;
        coremap[i].as = NULL;    
    }
}

/* Load page from back store to memory. May call page_evict_any if there’s no more physical memory. See Paging for more details. */
paddr_t cm_load_page(struct addrspace *as, vaddr_t va) {
    // Code goes here
    // Basically do cm_alloc_page and then load
    // TODO: bs_read_in should happen here
    (void) as;
    (void) va;
    return 0;
}

paddr_t cm_alloc_page(struct addrspace *as, vaddr_t va) {
    (void) as;
    (void) va;
    // TODO: design choice here, everything needs to be passed down
    int cm_index;
    // Try to find a free page. If we have one, it's easy. We probably
    // want to keep a global cm_free veriable to boost performance
    cm_index = cm_get_free_page();
    
    // We don't have any free page any more, needs to evict.
    if (cm_index < 0) {
        // Do page eviction
        cm_evict_page();
    }
    // cm_index should be a valid page index at this point
    KASSERT(coremap[cm_index].busy);
    KASSERT(coremap[cm_index].allocated = 0);
    coremap[cm_index].allocated = 1;

    // If not kernel, update as and vaddr_base
    // What do we do with kernel vs. user? -- Should be taken care of
    KASSERT(va != 0);
    coremap[cm_index].vm_addr = va;
    coremap[cm_index].as = as;
    coremap[cm_index].pid = curproc->pid;   // Should not be using this. Consider receiving struct proc instead of struct addrsoace
    coremap[cm_index].is_kernel = (curproc == kproc);
    return CM_TO_PADDR(cm_index);
}

// Returns a index where a page is free
int cm_get_free_page(void) {
    int i;
    KASSERT(cm_entries <= cm_used);
    if (cm_entries == cm_used) {
        return -1;
    }
    // Do we want to use busy_lock here?
    for (i = 0; i < cm_entries; i++){
        spinlock_acquire(&busy_lock);
        if (!coremap[i].allocated) {
            spinlock_release(&busy_lock);
            return i;
        }
        spinlock_release(&busy_lock);
    }

    // Either cm_used = cm_entries, or there was a free space
    KASSERT(false);
}

/* Evict page from memory. This function will update coremap, write to backstore and update the backing_index entry; */
// Need to sync 2 addrspaces
// Simply update the pte related to paddr
void cm_evict_page(){
    // Use our eviction policy to choose a page to evict
    // coremap[cm_index] should be busy when this returns
    // TODO: There's no synchronization at all. 
    int cm_index;

    cm_index = cm_choose_evict_page();

    // Shoot down other CPUs
    vm_tlbshootdown_all();

    // Write to backing storage no matter what -- Not anymore! We now have dirty bits!
    KASSERT(coremap[cm_index].busy);
    if (coremap[cm_index].dirty) {
        bs_write_out(cm_index);
    }

    // Need to find the pt entry and mark it as not in memory anymore
    struct pt_entry *pte = pt_get_entry(coremap[cm_index].as,coremap[cm_index].vm_addr<<12);
    KASSERT(pte != NULL);

    pte->in_memory = 0;
    coremap[cm_index].allocated = 0;
}

// NOT COMPLETE
/* Evict the "next" page from memory. This will be dependent on the 
eviction policy that we choose (clock, random, etc.). This is 
where we will switch out different eviction policies */
#ifdef PAGE_LINEAR
int cm_choose_evict_page() {
    int i = 0;
    struct cm_entry cm_entry;
    while (true) {
        cm_entry = coremap[i];
        spinlock_acquire(&busy_lock);
        if (cm_entry.busy){
            spinlock_release(&busy_lock);
            i = (i + 1) % cm_entries;
            continue;
        } else {
            CM_SET_BUSY(cm_entry);
            spinlock_release(&busy_lock);
            return i;
        }
    }
}
#elif PAGE_CLOCK
int cm_choose_evict_page() {
    struct cm_entry cm_entry;
    while (true) {
        cm_entry = coremap[i];
        spinlock_acquire(&busy_lock);
        if (cm_entry.busy || cm_entry.is_kernel || cm_entry.used_recently){
            spinlock_release(&busy_lock);
            if (cm_entry.used_recently) {
                cm_entry.used_recently = false;
            }
            evict_hand = (evict_hand + 1) % cm_entries;
            continue;
        } else {
            CM_SET_BUSY(cm_entry);
            spinlock_release(&busy_lock);
            return i;
        }
    }
}
#endif

/* Blocks until a coremap entry can be set as dirty */
void cm_set_dirty(paddr_t paddr) {
    // Don't worry about synchronization until we combine the bits with the vm_addr
    int cm_index = PADDR_TO_CM(paddr);
    coremap[cm_index].dirty = true;
}


// Code for backing storage, could be moved to somewhere else.

void bs_bootstrap() {
    // open file, bitmap and lock
    char *path = kstrdup("lhd0raw:");
    if (path == NULL)
        panic("bs_bootstrap: couldn't open disk");

    int err = vfs_open(path, O_RDWR, 0, &bs_file);
    if (err)
        panic("bs_bootstrap: couldn't open disk");

    bs_map = bitmap_create(1000);
    if (disk_map == NULL)
        panic("bs_bootstrap: couldn't create disk map");

    bs_map_lock = lock_create("disk map lock");
    if (bs_map_lock == NULL)
        panic("bs_bootstrap: couldn't create disk map lock");
    return;
}

int bs_write_out(int cm_index) {
    int err, offset;
    paddr_t paddr = CM_TO_PADDR(cm_index);
    struct iovec iov;
    struct uio u;
    struct addrspace *as = coremap[cm_index].as;
    vaddr_t va = coremap[cm_index].vm_addr;
    struct pt_entry *pte = pt_get_entry(as, va);

    // TODO: error checking
    offset = pte->store_index;
    uio_kinit(&iov, &u, (void *) PADDR_TO_KVADDR(paddr), PAGE_SIZE, 
        offset * PAGE_SIZE, UIO_WRITE);
    err = VOP_WRITE(bs_file, &u);

    // TODO: This will relate to dirty page management

    return err;
}

// Put stuff in dest.
int bs_read_in(struct addrspace *as, vaddr_t va, int cm_index) {
    int err, offset;
    paddr_t paddr = CM_TO_PADDR(cm_index);
    struct iovec iov;
    struct uio u;
    struct pt_entry *pte = pt_get_entry(as, va);

    // TODO: error checking
    offset = pte->store_index;
    uio_kinit(&iov, &u, (void *) PADDR_TO_KVADDR(paddr), PAGE_SIZE, 
        offset * PAGE_SIZE, UIO_READ);
    err = VOP_READ(bs_file, &u);

    if (!err){
        coremap[cm_index].vm_addr = va>>12;
        coremap[cm_index].as = as;

        pte->store_index = offset;
        pte->in_memory = 1;
        pte->p_addr = paddr>>12;
    }

    return err;
}

unsigned bs_alloc_index() {
    unsigned index;

    lock_acquire(bs_map_lock);
    if (bitmap_alloc(bs_map, &index))
        panic("no space on disk");
    lock_release(bs_map_lock);
    return index;
}

void bs_dealloc_index(unsigned index) {
    lock_acquire(bs_map_lock);

    KASSERT(bitmap_isset(bs_map, index));
    bitmap_unmark(bs_map, index);

    lock_release(bs_map_lock);
    return;
}
