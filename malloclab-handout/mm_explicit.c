/* 
 * Malloclab Submission, CMU 15213/513 Fall 2015
 * Aleksander Bapst (abapst)
 * This is a simple explicit free list memory allocator. Each block has a
 * header and footer. The header contains a 4-byte tag with the total size 
 * of the block and an 8-byte pointer to the previous free block (if the 
 * block is free). The allocation bit is the least significant bit of the tag.
 * The footer contains the same 4-byte tag and an 8-byte pointer to next free
 * block. A diagram of this block layout is shown below.
 *
 *                         Typical memory block in the heap
 *         ---------------------------------------------------------------
 *         | tag | previous pointer | <<<payload>>> | tag | next pointer |
 *         ---------------------------------------------------------------
 *            ^            ^        ^       ^          ^        ^ 
 *         4 bytes      8 bytes     |   Arbitrary    4 bytes    8 bytes
 *                                  |
 *                          block pointer (bp) 
 *
 * The min block size is thus 32 bytes, 24 for overhead and 8 for alignment.
 * The explicit list is maintained as a single linked list of free blocks.
 * When blocks are freed they are added to the beginning of the list, and
 * the list is searched for free blocks using the first-fit strategy. Blocks
 * are split and coalesced if necessary. When new memory is required, the heap
 * is expanded to fit the necessary amount.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mm.h"
#include "memlib.h"

/* do not change the following! */
#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#endif /* def DRIVER */

/* Basic constants and macros */
#define WSIZE       4       /* Word and header/footer size (bytes) */ 
#define DSIZE       8       /* Double word size (bytes) */
#define CHUNKSIZE  (1<<5)  /* Extend heap by this amount (bytes) */  
#define MIN_SIZE    32      /* Header + Footer + Payload Alignment */
#define LIST_END   (void *) 0 /* Front and end of linked lists */

#define MAX(x, y) ((x) > (y)? (x) : (y))  

/* Pack a size and allocated bit into a word */
#define PACK(size, alloc)  ((size) | (alloc)) 

/* Read and write a word at address p */
#define GET(p)       (*(unsigned int *)(p))            
#define PUT(p, val)  (*(unsigned int *)(p) = (val))    

/* Given a ptr p, read the size and allocated fields */
#define GET_SIZE(p)  (GET(p) & ~0x7)                   
#define GET_ALLOC(p) (GET(p) & 0x1)                    

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp)       ((char *)(bp) - 3*WSIZE)                      
#define FTRP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)) - 6*WSIZE) 

/* Given block ptr bp, get address of next bp and previous bp */
#define NEXT_BLKP(bp)  ((char *)(bp) + GET_SIZE(HDRP(bp))) 
#define PREV_BLKP(bp)  ((char *)(bp) - GET_SIZE((char *)bp - 6*WSIZE)) 

/* Manipulate free list */
#define PREV_PTR(bp)           ((void **) ((void *) FTRP(bp) + WSIZE))
#define NEXT_PTR(bp)           ((void **) ((void *) HDRP(bp) + WSIZE))
#define GET_NEXT_FREE(bp)      (*NEXT_PTR(bp))
#define GET_PREV_FREE(bp)      (*PREV_PTR(bp))
#define SET_NEXT_FREE(bp, val) (GET_NEXT_FREE(bp) = val)
#define SET_PREV_FREE(bp, val) (GET_PREV_FREE(bp) = val)

/* Find the amount of space left at the end of the heap */
#define SPACE_LEFT(eptr)  (GET_ALLOC(HDRP(eptr)) ? 0 : GET_SIZE(HDRP(eptr)))

/* Heap check helpers */
#define ALLOC_CHAR(p)     (GET_ALLOC(p) ? 'a' : 'f')  
/* Check if block satisfies alignment requirements */
#define ALIGNED(bp)       ((GET_SIZE(HDRP(bp))%DSIZE == 0) ? 1 : 0)
#define ALIGNED_CHAR(bp)  (ALIGNED(bp) ? 'Y' : 'N')
/* Check if header tag = footer tag */
#define HEF(bp)           ((GET(HDRP(bp)) == GET(FTRP(bp))) ? 'Y' : 'N') 

/* Global variables */
static char *heap_listp = 0;  /* Pointer to first block */  
static void *free_list; /* pointer to root of explicit free list */
static void *eptr; /* pointer to epilogue block */

/* Function prototypes for internal helper routines */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);

static void *search_list(size_t asize);
static void *split(void *block, size_t asize);
static void add_block(void *bp);
static void delete_block(void *bp);
static void set_size(void *block, size_t new_size);
static void set_alloc(void *block);
static void set_free(void *block);

/* 
 * mm_init - Initialize the memory manager 
 */
int mm_init(void) 
{
    /* Create the initial empty heap */
    if ((heap_listp = mem_sbrk(8*WSIZE)) == (void *)-1) 
        return -1;

    /* For ease of coalescing add prologue and epilogue blocks */
    /* Prologue does not need a footer, only a header to satisfy macros */
    PUT(heap_listp, 0); /* Alignment padding */
    PUT(heap_listp + (1*WSIZE), PACK(6*WSIZE, 1)); /* Prologue header tag */ 
    PUT(heap_listp + (2*WSIZE), 0); /* Prologue header prev ptr */ 
    PUT(heap_listp + (4*WSIZE), PACK(6*WSIZE, 1)); /* Prologue footer tag */ 
    PUT(heap_listp + (5*WSIZE), 0); /* Prologue footer next ptr */ 
    PUT(heap_listp + (7*WSIZE), PACK(0, 1)); /* Epilogue header */
    heap_listp += (4*WSIZE); /* Move heap ptr to prologue block pointer */
    eptr = heap_listp + (3*WSIZE);

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    //if ((free_list = extend_heap(CHUNKSIZE)) == NULL)  { 
    //    return -1;
    //}
    /* Initialize linked list pointers */
    //SET_PREV_FREE(free_list, LIST_END); 
    //SET_NEXT_FREE(free_list, LIST_END);
    free_list = NULL;
    return 0;
}

/* 
 * malloc - Allocate a block with at least size bytes of payload 
 */
void *malloc(size_t size) 
{
    size_t asize;      /* Adjusted block size */
    void *bp;      
    if (heap_listp == 0) {
        mm_init();
    }
    /* Ignore spurious requests */
    if (size == 0)
        return NULL;

    /* Adjust block size to include overhead and alignment reqs. */
    if (size <= DSIZE)                                          
        asize = MIN_SIZE;                                        
    else
        asize = DSIZE * ((size + (3*DSIZE) + (DSIZE-1)) / DSIZE); 

    /* Search the free list for a fit */
    bp = search_list(asize);
    /* No fit found. Get more memory and place the block */
    if (bp == NULL) {
        if ((bp = extend_heap(asize)) == NULL)  
            return NULL;                                  
    }

    /* set bp as allocated and delete from list */
    set_alloc(bp);
    return bp;
} 

/* 
 * free - Free a block 
 */
void free(void *bp)
{
    if (bp == 0) 
        return;
    if (heap_listp == 0){
        mm_init();
    }
    /* set block to free and coalesce immediately */
    set_free(bp);
    coalesce(bp);
}

void *calloc(size_t nmemb, size_t size) {
    size_t asize = nmemb*size; /* total number of bytes to allocate */
    int c = 0x0;

    void *bp = mm_malloc(asize); /* allocate the needed space */
    return memset(bp, c, asize); /* set all bytes to zero and return bp */ 
}

/*
 * realloc - Naive implementation of realloc
 */
void *realloc(void *ptr, size_t size)
{
    size_t oldsize;
    void *newptr;

    /* If size == 0 then this is just free, and we return NULL. */
    if(size == 0) {
        mm_free(ptr);
        return 0;
    }

    /* If oldptr is NULL, then this is just malloc. */
    if(ptr == NULL) {
        return mm_malloc(size);
    }

    newptr = mm_malloc(size);

    /* If realloc() fails the original block is left untouched  */
    if(!newptr) {
        return 0;
    }

    /* Copy the old data. */
    oldsize = GET_SIZE(HDRP(ptr));
    if(size < oldsize) oldsize = size;
    memcpy(newptr, ptr, oldsize);

    /* Free the old block. */
    mm_free(ptr);

    return newptr;
}

/*
 * search_list - search the free list for a block of at least asize
 */
static void *search_list(size_t asize) {
    /* root of free list */
    void* bp = free_list;

    /* traverse the free list */
    while (bp != NULL) {
        if (GET_SIZE(HDRP(bp)) >= asize)
            return split(bp, asize); 
        bp = GET_NEXT_FREE(bp); 
    }
    /* No free block found */
    return NULL;
}

/*
 * split - handle splitting of a free block during allocation
 */
static void *split(void *bp, size_t asize) {
    size_t original_size = GET_SIZE(HDRP(bp));
    void *split_block;
    /* Split the free block before allocation */
    if (original_size >= asize + MIN_SIZE) {

        /* Update size */
        delete_block(bp);
        set_size(bp, asize);

        /* Set split_block size and add to free list */
        split_block = NEXT_BLKP(bp);
        set_size(split_block, original_size - asize);
        add_block(split_block);
    } else {
        delete_block(bp);
    }
    /* Allocate the entire free block if not splitting */
    return bp;
}

/*
 * set_size - update the size tag in header/footer of a block. Allocation
 * 	      is automatically set to 0 (free)
 */
static void set_size(void *bp, size_t asize) {
    PUT(HDRP(bp),PACK(asize,0));
    PUT(FTRP(bp),PACK(asize,0)); 
}

/*
 * set_alloc - mark a block as allocated
 */
static void set_alloc(void *bp) {
    PUT(HDRP(bp), GET(HDRP(bp)) | 0x1);
    PUT(FTRP(bp), GET(FTRP(bp)) | 0x1);
} 

/*
 * set_free - mark a block as free
 */
static void set_free(void *bp) {
    PUT(HDRP(bp), GET(HDRP(bp)) & ~0x1);
    PUT(FTRP(bp), GET(FTRP(bp)) & ~0x1);
}

/* 
 * extend_heap - Extend heap with free block and return its block pointer
 */
static void *extend_heap(size_t asize) 
{
    void *bp;
    size_t space_left = SPACE_LEFT(eptr); /* Space left in heap */
    size_t size = asize - space_left; /* Amount to extend heap */

    if (size < MIN_SIZE)
        size = MIN_SIZE;

    if ((long)(bp = mem_sbrk(size)) == -1)  
        return NULL;                               
    bp = bp + 2*WSIZE; /* move bp to start of payload */

    /* Initialize new free space */
    set_size(bp, size);

    /* Build new epilogue header and update epilogue pointer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); 
    eptr += GET_SIZE(HDRP(bp)); 

    /* Coalesce if the previous block was free, and split before returning */
    bp = coalesce(bp);
    return split(bp, asize);                           
}

/*
 * delete_block - delete a block from the free list
 */
static void delete_block(void *bp) {
    void *prev = GET_PREV_FREE(bp);
    void *next = GET_NEXT_FREE(bp);
    /* Case 1: bp is the only block in list */
    if ((prev == LIST_END) && (next == LIST_END)) {
        free_list = NULL;
        return;
    /* Case 2: bp is the first block in the list */
    } else if (prev == LIST_END) {
        SET_PREV_FREE(next, LIST_END);
        free_list = next;
        return;
    /* Case 3: bp is the last block in the list */
    } else if (next == LIST_END) {
        SET_NEXT_FREE(prev, LIST_END);
        return;
    /* Case 4: bp is in the middle of the list */
    } else {
        SET_NEXT_FREE(prev, next);
        SET_PREV_FREE(next, prev);
        return;
    }
}

/*
 * add_block - add a block to the front of the free list
 */
static void add_block(void *bp) {
    /* Case 1: Add bp to front of list */
    if (free_list != NULL) {
        SET_PREV_FREE(free_list, bp);
        SET_NEXT_FREE(bp, free_list);
        SET_PREV_FREE(bp, LIST_END);
    /* Case 2: List is empty, make bp the first block */
    } else {
        SET_PREV_FREE(bp, LIST_END);
        SET_NEXT_FREE(bp, LIST_END);
    }
    free_list = bp; /* update list root */
}

/*
 * coalesce - Boundary tag coalescing. Return ptr to coalesced block
 */
static void *coalesce(void *bp) 
{
    void *prev = PREV_BLKP(bp);
    void *next = NEXT_BLKP(bp);

    size_t prev_alloc = GET_ALLOC(HDRP(prev));
    size_t next_alloc = GET_ALLOC(HDRP(next));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) {            /* Case 1 */
        add_block(bp);
        return bp;
    }
    else if (prev_alloc && !next_alloc) {      /* Case 2 */
        delete_block(next);
        size += GET_SIZE(HDRP(next));
        set_size(bp, size);
        add_block(bp);
        return bp;
    }
    else if (!prev_alloc && next_alloc) {      /* Case 3 */
        delete_block(prev);
        size += GET_SIZE(HDRP(prev));
        set_size(prev, size);
        add_block(prev);
        return prev;
    }
    else {                                     /* Case 4 */
        delete_block(next);
        delete_block(prev);
        size += GET_SIZE(HDRP(prev)) + 
            GET_SIZE(HDRP(next));
        set_size(prev, size);
        add_block(prev);
        return prev;
    }
}

/* 
 * mm_checkheap - Check the heap for correctness. It prints a nice grid
 *                representation of the heap at the moment in time it is
 *                called, as well as the block pointer and epilogue
 *                pointer. It takes __LINE__ as input, allowing it to print
 *                the number of the line it is called at. It also silently
 *                checks for errors and exits the program if any error
 *                is found. If silent is 0, it will only print the results
 *                upon finding an error.
 *
 *                NOTE: if mm_checkheap is called ANYWHERE besides malloc and
 *                free, it will find errors because of intermediate states
 *                that trigger error conditions. Therefore error_check should
 *                be 0 if you call it anywhere besides inside malloc and free.
 */
void mm_checkheap(int lineno)  
{ 
    char *bp;
    char alloc_char;
    char aligned;
    char hef;
    int payload;
    int size;
    int num_head = 0, num_tail = 0;
    int link_error_flag = 0;
    int alignment_flag = 0;
    int error_flag = 0;
    int error_check = 1; /* set to 0 if called outside malloc() and free() */
    int silent = 1;

    /* Silently check for errors */
    if (error_check) {
        for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
            if (!ALIGNED(bp))
                alignment_flag = 1;

            if (!GET_ALLOC(HDRP(bp))) {
                /* Check for linked list errors in free blocks */
                if (GET_NEXT_FREE(bp) == NULL)
                    num_tail++;
                else if (GET_PREV_FREE(GET_NEXT_FREE(bp)) != bp)
                    link_error_flag = 1;    
                if (GET_PREV_FREE(bp) == NULL)
                    num_head++;
                else if (GET_NEXT_FREE(GET_PREV_FREE(bp)) != bp)
                    link_error_flag = 1;
            }
            if (num_tail > 1||num_head > 1||link_error_flag||alignment_flag)
                error_flag = 1;
        }
    }

    if (error_flag || !silent) {
        printf("\n");
        printf("========================================"
            "==========================================\n");
        printf("                            HEAP CONSISTENCY CHECKER\n");
        printf("========================================"
            "==========================================\n");
        printf("Line number = %d\n", lineno);
        printf("Head of free list = %p\n",free_list);
        printf("Epilogue pointer  = %p\n", eptr);
        printf("----------------------------------------"
            "-----------------------------------------\n");
        printf("T |  Block pointer   | Size  |Payload|"
            "       Prev       |       Next       |A|HEF\n"); 
        printf("--|------------------|-------|-------|--"
            "----------------|------------------|-|---\n");

        for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
           payload = FTRP(bp)-bp;
           size = GET_SIZE(HDRP(bp));
           alloc_char = ALLOC_CHAR(HDRP(bp));
           aligned = ALIGNED_CHAR(bp); /* Check for alignment */
           hef = HEF(bp); /* Check that header = footer */
           if (GET_ALLOC(HDRP(bp))) {
               printf("%c |%18p|%7d|%7d|%18s|%18s|%c| %c \n",
                   alloc_char,
                   bp,
                   size,
                   payload,
                   "",
                   "",
                   aligned,
                   hef);
           } else {
               printf("%c |%18p|%7d|%7d|%18p|%18p|%c| %c \n",
                   alloc_char,
                   bp,
                   size,
                   payload,
                   GET_PREV_FREE(bp),
                   GET_NEXT_FREE(bp),
                   aligned,
                   hef);
          }
        }
        printf("----------------------------------------"
            "-----------------------------------------\n");
        printf("Key: T = (a)llocated or (f)ree. A = aligned to double word."
            " HEF = H/F tags match.\n");

        /* If there is a problem with the free list, print details and exit. */
        if (error_flag) {
            printf("----------------------------------------"
                "-----------------------------------------\n");
            printf("Heap integrity errors found:\n"); 
            if (num_tail > 1)
                printf("    [List error] More than one list tail.\n");
            if (num_head > 1)
                printf("    [List error] More than one list head.\n");
            if (link_error_flag)
                printf("    [List error] Links don't match up in "
                    "at least one block.\n"); 
            if (alignment_flag)
                printf("    [Alignment error] Unaligned block detected.\n");
            printf("----------------------------------------"
                "-----------------------------------------\n");
            printf("\n");
            exit(0);
        }  
    }
}
