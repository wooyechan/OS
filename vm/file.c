/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"

/* Page offset (bits 0:12). */
#define PGSHIFT 0                          /* Index of first offset bit. */
#define PGBITS  12                         /* Number of offset bits. */
#define PGSIZE  (1 << PGBITS)              /* Bytes in a page. */
#define PGMASK  BITMASK(PGSHIFT, PGBITS)   /* Page offset bits (0:12). */

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

    struct load_arg *aux = (struct load_arg *)page->uninit.aux;
    file_page->file = aux->file;
    file_page->offset = aux->offset;
    file_page->read_bytes = aux->read_bytes;
    file_page->zero_bytes = aux->zero_bytes;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
    uint8_t *paddr=page->frame->kva;
    file_read_at(file_page->file, paddr, file_page->read_bytes, file_page->offset);
	memset(paddr+file_page->read_bytes, 0, file_page->zero_bytes);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
    
    bool flg=false;

    lock_acquire(&page->box_lock);
	struct list_elem *box_elem=list_begin(&page->box_list);
	for(; box_elem!=list_end(&page->box_list); box_elem=list_next(box_elem)){
		struct thread *th=list_entry(box_elem, struct page_box, box_elem)->th;
		if(pml4_is_dirty(th->pml4, page->va))flg=true;
	}
    if(flg){
        file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->offset);
    }

    box_elem=list_begin(&page->box_list);
	for(; box_elem!=list_end(&page->box_list); box_elem=list_next(box_elem)){
		struct thread *th=list_entry(box_elem, struct page_box, box_elem)->th;
		if(flg)pml4_set_dirty(th->pml4, page->va, false);
        pml4_clear_page(th->pml4, page->va);
	}
    lock_release(&page->box_lock);
	page->frame=NULL;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *t = thread_current();
    
    if (pml4_is_dirty(t->pml4, page->va)) {
        file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->offset);
    }

    if(page->frame){
        free(page->frame);
        page->frame=NULL;
    }
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
    
    struct file *reopened_file = file_reopen(file);
    if (reopened_file == NULL) {
        return NULL;
    }

	// to return original addr
    void *original = addr;

    off_t file_len = file_length(reopened_file);
    size_t read_bytes = length > file_len ? file_len : length;
    size_t zero_bytes = PGSIZE - (read_bytes % PGSIZE);

    while (read_bytes > 0 || zero_bytes > 0) {

        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct load_arg *aux = (struct load_arg *)malloc(sizeof(struct load_arg));
        if (aux == NULL) {
            return NULL; 
        }
        aux->file = reopened_file;
        aux->offset = offset;
        aux->read_bytes = page_read_bytes;
        aux->zero_bytes = page_zero_bytes;

        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment, aux)) {
            free(aux); 
            return NULL;
        }

        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        addr += PGSIZE;
        offset += page_read_bytes;
    }

    return original;
}

/* Do the munmap */
void
do_munmap(void *addr) {
    struct thread *curr=thread_current();
    struct page *page;
    void *looking_addr=addr;
    bool flg = false;

    while(!flg){
        page=spt_find_page(&curr->spt, looking_addr);
        if(!page || page->type!=VM_FILE)break;
        if(page->operations->type==VM_UNINIT){
            if(((struct load_arg*)(page->uninit.aux))->zero_bytes)flg=true;
        }
        if(page->operations->type==VM_FILE){
            if(page->file.zero_bytes)flg=true;
        }
        spt_remove_page(&curr->spt, page);

        looking_addr+=PGSIZE;
    }
}
