/* 
 * Malloclab Submission, CMU 15213/513 Fall 2015
 * Aleksander Bapst (abapst)
 *
 * This is a segregated free list memory allocator. Each block has a
 * header and footer. The header contains a 4-byte tag with the total size 
 * of the block and an 4-byte integer offset to the previous free block (if the
 * block is free). The allocation bit is the least significant bit of the tag.
 * The footer contains the same 4-byte tag and a 4-byte offset to the next free
 * block. A diagram of this block layout is shown below.
 *
 *                         Typical memory block in the heap
 *         ---------------------------------------------------------------
 *         | tag | previous pointer | <<<payload>>> | tag | next pointer |
 *         ---------------------------------------------------------------
 *            ^            ^        ^       ^          ^        ^ 
 *         4 bytes      4 bytes     |   Arbitrary    4 bytes    4 bytes
 *                                  |
 *                          block pointer (bp) 
 *
 * The min block size is thus 24 bytes, 16 for overhead and 8 for alignment.
 * The explicit lists are maintained as 20 linked lists of free blocks.
 * The blocks are sorted into lists based on the position of the most
 * significant bit. For example, list 4 contains block sizes ranging from
 * 2^3 - (2^4)-1. List 0 is a dummy list and doesn't contain any blocks
 * because malloc can't allocate 0 blocks.  
 * When blocks are freed they are added to the beginning of the list whose
 * size range they fall into, and the lists are searched for free blocks using 
 * the first-fit strategy. When searching, if the smallest size range list that
 * fits the query size does not contain a large enough block, then the next
 * largest list is searched and so on until a block is found.
 * Blocks are split and coalesced if necessary. When new memory is required, 
 * the heap is expanded to fit the necessary amount. A minimum chunk size is
 * used for extension so that many small allocation requests don't tie up
 * the processor. The best size to use was determined by trial and error.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

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
#define MIN_SIZE    24      /* Header + Footer + Payload Alignment */
#define LIST_END    0       /* Front and end of linked lists */
#define NUM_LISTS   20      /* Number of segregated free lists */
#define CHUNKSIZE   1<<8   /* Minimum chunk size to extend heap */

/* Heap checker options */

/* Check for errors when checker called, or just print the current state of the 
 * heap. If this is 1 and mm_checkheap is called outside malloc() and free()
 * there is a good chance it will fault and print errors because it isn't smart 
 * and doesn't know if an error (like temporary non-coalesced blocks) is normal 
 * or not.
 */
#define ERROR_CHECK   1
/* If 1, the checker will only print results upon finding an error. Otherwise 
 * it will print whenever mm_checkheap(__LINE__) is called.
 */
#define SILENT   0

/* Max of two numbers */
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
#define HDRP(bp)       ((char *)(bp) - 2*WSIZE)                      
#define FTRP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)) - 4*WSIZE) 

/* Given block ptr bp, get address of next bp and previous bp */
#define NEXT_BLKP(bp)  ((char *)(bp) + GET_SIZE(HDRP(bp))) 
#define PREV_BLKP(bp)  ((char *)(bp) - GET_SIZE((char *)bp - 4*WSIZE)) 

/* Manipulate free list by storing integer offsets to the next/prev pointers */
#define PREV_PTR(bp)            (int *) (FTRP(bp) + WSIZE)
#define NEXT_PTR(bp)            (int *) (HDRP(bp) + WSIZE)
#define GET_PREV_FREE(bp)       ((!*PREV_PTR(bp)) ? 0 : (bp + *PREV_PTR(bp)))
#define GET_NEXT_FREE(bp)       ((!*NEXT_PTR(bp)) ? 0 : (bp + *NEXT_PTR(bp)))
#define SET_PREV_FREE(bp, prev) (*PREV_PTR(bp) = prev - bp)
#define SET_NEXT_FREE(bp, next) (*NEXT_PTR(bp) = next - bp)

/* Find the amount of space left at the end of the heap */
#define SPACE_LEFT(eptr)  (GET_ALLOC(HDRP(eptr)) ? 0 : GET_SIZE(HDRP(eptr)))

/* Heap check helpers */
#define ALLOC_CHAR(p)     (GET_ALLOC(p) ? 'a' : 'f')  
/* Check if block satisfies alignment requirements */
#define ALIGNED(bp)       ((GET_SIZE(HDRP(bp))%DSIZE == 0) ? 1 : 0)
#define BPALIGNED(bp)     (((uintptr_t) bp % DSIZE == 0) ? 1 : 0)
#define ALIGNED_CHAR(bp)  (ALIGNED(bp) ? 'Y' : 'N')
/* Check if header tag = footer tag */
#define HEF(bp)           ((GET(HDRP(bp)) == GET(FTRP(bp))) ? 1 : 0) 
#define HEF_CHAR(bp)      (HEF(bp) ? 'Y' : 'N') 

/* Global variables */
static char *heap_listp = 0;  /* Pointer to first block */  
static void *free_lists[NUM_LISTS]; /* array of pointers to free lists */
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
static int get_list(size_t size);
static size_t align_size(size_t size);

/* 
 * mm_init - Initialize the memory manager. Creates the prologue and epilogue
 *           blocks and initializes the segregated free lists. 
 */
int mm_init(void) 
{
    /* Create the initial empty heap */
    if ((heap_listp = mem_sbrk(5*WSIZE)) == (void *)-1) 
        return -1;

    /* Initialize pointers to all free lists */
    for (int list = 0; list < NUM_LISTS; list++) {
        free_lists[list] = NULL;
    }

    /* For ease of coalescing add prologue and epilogue blocks */
    /* Prologue does not need a footer, only a header to satisfy macros */
    PUT(heap_listp, PACK(4*WSIZE, 1)); /* Prologue header tag */ 
    PUT(heap_listp + (1*WSIZE), 0); /* Prologue header prev ptr */ 
    PUT(heap_listp + (2*WSIZE), PACK(4*WSIZE, 1)); /* Prologue footer tag */ 
    PUT(heap_listp + (3*WSIZE), 0); /* Prologue footer next ptr */ 
    PUT(heap_listp + (4*WSIZE), PACK(0, 1)); /* Epilogue header */
    heap_listp += (2*WSIZE); /* Move heap ptr to prologue block pointer */
    eptr = heap_listp + (2*WSIZE);

    return 0;
}

/* 
 * malloc - Allocate a block with at least size bytes of payload. 
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
    asize = align_size(size);
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
 * free - Free a block. The requested block is set to free, then passed
 *        to the coalesce function to handle coalescing. 
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

/*
 * calloc - Simple implementation of calloc. It uses malloc to allocate
 *          a block and then sets every byte to 0.
 */
void *calloc(size_t nmemb, size_t size) {
    size_t asize = nmemb*size; /* total number of bytes to allocate */
    int c = 0x0;

    void *bp = mm_malloc(asize); /* allocate the needed space */
    return memset(bp, c, asize); /* set all bytes to zero and return bp */ 
}

/*
 * realloc - Implementation of realloc that is slightly better than textbook.
 *           If the requested size is smaller than the available size, the
 *           block is checked if it can be split or coalesced before malloc.
 *           Otherwise malloc is used to find a new block.
 */
void *realloc(void *ptr, size_t size)
{
    /* If size == 0 then this is just free, and we return NULL. */
    if(size == 0) {
        mm_free(ptr);
        return 0;
    }

    /* If oldptr is NULL, then this is just malloc. */
    if(ptr == NULL) {
        return mm_malloc(size);
    }

    size_t oldsize = GET_SIZE(HDRP(ptr));
    size_t newsize = align_size(size);
    void *newptr;

    /* Split block if newsize is less than or equal to oldsize */
    if (newsize <= oldsize) {
        set_free(ptr);
        add_block(ptr); /* add to free list */
        newptr = split(ptr, newsize);       
        set_alloc(newptr);
        return newptr;
    /* Else try to coalesce, then use malloc if needed */
    } else {
        void *prev = PREV_BLKP(ptr);
        void *next = NEXT_BLKP(ptr);
        size_t prev_alloc = GET_ALLOC(HDRP(prev));
        size_t next_alloc = GET_ALLOC(HDRP(next));
        size_t prev_size = GET_SIZE(HDRP(prev));
        size_t next_size = GET_SIZE(HDRP(next));
        size_t diff = newsize - oldsize; /* We know diff is positive here */
 
        /* Coalesce if surrounding blocks have needed space.
         * This could be done more thoroughly but I don't want to
         * bother with the implementation.
         */
        if ((!prev_alloc && prev_size >= diff) ||
            (!next_alloc && next_size >= diff) ||
            (!next_alloc && !prev_alloc && (prev_size+next_size) >= diff)) {

            newptr = coalesce(ptr);
            /* If pointers are different, we need to move the data back */
            if (newptr != ptr) {
                memcpy(newptr, ptr, oldsize);
            }
            newptr = split(newptr, newsize);
            set_alloc(newptr);
            return newptr;
        /* We need to use malloc to find space */
        } else {
            newptr = malloc(newsize);
            memcpy(newptr, ptr, oldsize); /* Copy the old data over */
            mm_free(ptr);
            return newptr;
        }
    }
}

/*
 * search_list - search the free list for a block of at least asize using
 *               a first-fit search. If the smallest size list that fits the
 *               requested size doesn't have an available block, it cycles
 *               through all the larger lists. When it finds a block it
 *               passes it to split() to determine if it needs to be split.
 */
static void *search_list(size_t asize) {
    /* root of smallest free list that fits asize */
    int list = get_list(asize);
    void *bp = free_lists[list];

    /* cycle through all lists with bucket size >= query size */
    while (list < NUM_LISTS+1) {
        while (bp != LIST_END) {
            if (GET_SIZE(HDRP(bp)) >= asize)
                return split(bp, asize); 
            bp = GET_NEXT_FREE(bp); 
        }
        bp = free_lists[list++];
    }
    /* No free block found */
    return NULL;
}

/*
 * split - handle splitting of a free block during allocation. If the
 *         block is the perfect size, or the amount of internal fragmentation
 *         is too low, it just returns the whole block without splitting. 
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
 *  align_size - If size is less than the minimum block size, set it to the
 *               minimum block size. Else round it up to the nearest multiple
 *               of eight (double word size).
 */
static size_t align_size(size_t size) {
    size_t asize;

    if (size <= DSIZE)                                          
        asize = MIN_SIZE;                                        
    else
        asize = DSIZE * ((size + (2*DSIZE) + (DSIZE-1)) / DSIZE); 
    return asize;
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
 * get_list - given an input size return the index of the free_list whose
 *            range the size falls into.
 */
static int get_list(size_t size) {
    int list = 0;
    while (list < NUM_LISTS-1 && size > 0) {
        size >>= 1;
        list++;
    }
    return list;
}

/* 
 * extend_heap - Extend heap with free block and return its block pointer.
 *               It extends the heap by just enough to fit the required size,
 *               then splits and coalesces the new block as necessary. A
 *               more efficient implementation would be to somehow predict
 *               the frequency of upcoming allocations using past history
 *               and extend larger chunks as necessary, but this was too
 *               complex for my time constraints.
 */
static void *extend_heap(size_t asize) 
{
    void *bp;
    size_t space_left = SPACE_LEFT(eptr); /* Space left in heap */
    size_t size = asize - space_left; /* Amount to extend heap */

    /* If block is small, allocated a larger chunk to help with throughput */
    if (size < CHUNKSIZE)
        size = CHUNKSIZE;

    if ((long)(bp = mem_sbrk(size)) == -1)  
        return NULL;                               
    bp = bp + WSIZE; /* move bp to start of payload */

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
 * delete_block - This function deletes a block from the free list whose
 *                size bucket it falls into by setting the neighboring pointers
 *                to jump over the deleted block. It also handles all corner
 *                cases that might come up. 
 */
static void delete_block(void *bp) {
    void *prev = GET_PREV_FREE(bp);
    void *next = GET_NEXT_FREE(bp);

    int list = get_list(GET_SIZE(HDRP(bp)));

    /* Case 1: bp is the only block in list */
    if ((prev == LIST_END) && (next == LIST_END)) {
        free_lists[list] = NULL;
        return;
    /* Case 2: bp is the first block in the list */
    } else if (prev == LIST_END) {
        SET_PREV_FREE(next, next);
        free_lists[list] = next;
        return;
    /* Case 3: bp is the last block in the list */
    } else if (next == LIST_END) {
        SET_NEXT_FREE(prev, prev);
        return;
    /* Case 4: bp is in the middle of the list */
    } else {
        SET_NEXT_FREE(prev, next);
        SET_PREV_FREE(next, prev);
        return;
    }
}

/*
 * add_block - This function adds a block to the front of the free list. I tried
 *             doing an in-address-order arrangement, but this involved
 *             searching the free lists every time which destroyed throughput.
 */
static void add_block(void *bp) {

    int list = get_list(GET_SIZE(HDRP(bp)));

    /* Case 1: Add bp to front of list */
    if (free_lists[list] != NULL) {
        SET_PREV_FREE(free_lists[list], bp);
        SET_NEXT_FREE(bp, free_lists[list]);
        SET_PREV_FREE(bp, bp);
    /* Case 2: List is empty, make bp the first block */
    } else {
        SET_PREV_FREE(bp, bp);
        SET_NEXT_FREE(bp, bp);
    }
    free_lists[list] = bp; /* update list root */
}

/*
 * coalesce - Boundary tag coalescing. Return ptr to coalesced block.
 *            This function does simple coalescing immediately after
 *            a block is freed, by checking if the blocks immediately
 *            before and after it in the heap are also free.
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
 *                free, it will probably find errors because of intermediate 
 *                states that trigger error conditions. Therefore error_check 
 *                should be 0 if you call it anywhere besides inside malloc 
 *                and free.
 */
void mm_checkheap(int lineno)  
{ 
    void *high = mem_heap_hi();
    void *low = mem_heap_lo();
    char *bp;
    char alloc_char;
    char aligned;
    char hef;
    size_t payload;
    size_t size;
    int list_num;
    int free_token = 0;
    int free_count = 0;
    int alloc_count = 0;
    int num_head[NUM_LISTS]={0};
    int num_tail[NUM_LISTS]={0};
    unsigned int error_flags = 0; /*The bits of this number encode error flags*/
    /*
     * SUMMARY OF ERROR FLAGS
     * Bit 1: detected block out of alignment.
     * Bit 2: more than one tail detected in a list.
     * Bit 3: detected linked list mismatch.
     * Bit 4: more than one head detected in a list.
     * Bit 5: detected a header/footer tag mismatch.
     * Bit 6: detected a payload that lies outside the heap.
     * Bit 7: detected a block pointer that doesn't align to a double word.
     * Bit 8: detected two consecutive free blocks (coalescing error).
     * Bit 9: prologue block is not right size or not set as allocated.
     * Bit 10: epilogue block is not size 0, or is not set as allocated, or
     *     is not at the location of eptr.
     */

    /* Silently check for errors */
    if (ERROR_CHECK) {

        /* Check the epilogue block for problems */
        if ((GET_SIZE((char *)high - 3) != 0) || (!GET_ALLOC((char *)high - 3))
            || ((void *)((char *)high - 3) != eptr))
            error_flags |= 512; 

        /* Loop through all the blocks in the heap and check for errors */
        for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
            if (!ALIGNED(bp))
                error_flags |= 1; /* Set 1st bit */
            if (!BPALIGNED(bp))
                error_flags |= 64; /* Set 7th bit */
            if (!HEF(bp))
                error_flags |= 16; /* Set 5th bit */
            if ((void *)bp < low||((void *)bp+GET_SIZE(HDRP(bp))-DSIZE) > high)
                error_flags |= 32; /* Set 6th bit */
            if (bp == heap_listp && 
                ((GET_SIZE(HDRP(bp)) != 2*DSIZE) || !GET_ALLOC(HDRP(bp))))
                error_flags |= 256; /* Set 9th bit */

            if (!GET_ALLOC(HDRP(bp))) {
                /* Detect two consecutive free blocks (coalescing error) */
                free_token++;
                if (free_token > 1)
                    error_flags |= 128; /* Set 8th bit */
                /* Check for linked list errors in free blocks */
                size = GET_SIZE(HDRP(bp));
                list_num = get_list(size);
                if (GET_NEXT_FREE(bp) == NULL) {
                    if (num_tail[list_num]++ > 1)
                        error_flags |= 2; /* Set 2th bit */
                }
                else if (GET_PREV_FREE(GET_NEXT_FREE(bp)) != bp)
                    error_flags |= 4; /* Set 3rd bit */    
                if (GET_PREV_FREE(bp) == NULL) {
                    if (num_head[list_num]++ > 1)
                        error_flags |= 8; /* Set 4th bit */
                }
                else if (GET_NEXT_FREE(GET_PREV_FREE(bp)) != bp)
                    error_flags |= 4; /* Set 3rd bit */
            } else {
                free_token = 0; /* Reset token since next block is allocated */
            }
        }
    }

    /* Count the number of free and allocated blocks for the printed report.
     * If the check above went smoothly, then the free lists are all okay.
     */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if (GET_ALLOC(HDRP(bp)))
            alloc_count++;
        else
            free_count++;
    }

    if (error_flags || !SILENT) {
        printf("\n");
        printf("====================================="
            "==========================================\n");
        printf("                           HEAP CONSISTENCY CHECKER\n");
        printf("====================================="
            "==========================================\n");
        if (!error_flags)
            printf("Integrity check: OK\n");
        else
            printf("Integrity check: Errors found, see below for details.\n"); 
        printf("Line number = %d\n", lineno);
        printf("Free blocks: %d Allocated blocks: %d\n",free_count,alloc_count);
        printf("Epilogue pointer = %p\n", eptr);
        printf("--------------------------------------"
            "-----------------------------------------\n");
        printf("   T|Block pointer|  Size   | Payload | L|"
            "     Prev     |     Next     |A|E\n"); 
        printf("----|-------------|---------|---------|--|"
            "--------------|--------------|-|--\n");

        for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
           payload = FTRP(bp)-bp;
           size = GET_SIZE(HDRP(bp));
           alloc_char = ALLOC_CHAR(HDRP(bp));
           aligned = ALIGNED_CHAR(bp); /* Check for alignment */
           hef = HEF_CHAR(bp); /* Check that header = footer */
           list_num = get_list(size);
           if (GET_ALLOC(HDRP(bp))) {
               printf("   %c|%13p|%9zu|%9zu|%2s|%14s|%14s|%c|%c\n",
                   alloc_char,
                   bp,
                   size,
                   payload,
                   "",
                   "",
                   "",
                   aligned,
                   hef);
           } else {
               printf("   %c|%13p|%9zu|%9zu|%2d|%14p|%14p|%c|%c\n",
                   alloc_char,
                   bp,
                   size,
                   payload,
                   list_num,
                   GET_PREV_FREE(bp),
                   GET_NEXT_FREE(bp),
                   aligned,
                   hef);
          }
        }
        printf("--------------------------------------"
            "-----------------------------------------\n");
        printf("Key: T = (a)llocated or (f)ree. A = aligned to double word."
            " E = H/F tags match.\n");
        printf("     L = list number (range = 2^(L-1) -> (2^L)-1).\n");
        printf("\n");

        /* If there is a problem with the free list, print details and exit. */
        if (error_flags) {
            printf("--------------------------------------"
                "-----------------------------------------\n");
            printf("Heap Integrity Error Report:\n"); 
            if (error_flags & 1)
                printf("    [Block error] Unaligned block detected.\n");
            if (error_flags & 2)
                printf("    [List error] More than one list tail.\n");
            if (error_flags & 4)
                printf("    [List error] Links don't match up in "
                    "at least one block.\n"); 
            if (error_flags & 8)
                printf("    [List error] More than one list head.\n");
            if (error_flags & 16)
                printf("    [Block error] Header/footer mismatch detected.\n");
            if (error_flags & 32)
                printf("    [Heap error] Block payload outside of heap.\n");
            if (error_flags & 64)
                printf("    [Block error] Block pointer doesn't " 
                    "align to a double word.\n");
            if (error_flags & 128)
                printf("    [Coalescing error] Found two consecutive " 
                    "free blocks.\n");
            if (error_flags & 256)
                printf("    [Block error] Prologue block size is not %d bytes, "
                    "or is not set as alloc.\n",2*DSIZE);
            if (error_flags & 512)
                printf("    [Block error] Epilogue block size is not 0 bytes, "
                    "or is not set as alloc,\n    or is not at eptr.\n");
            printf("--------------------------------------"
                "-----------------------------------------\n");
            printf("\n");
            exit(0);
        }  
    }
}
