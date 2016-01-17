/* Cache for Proxylab, CMU 15-213/513, Fall 2015
 * Author: Aleksander Bapst (abapst)
 *
 * Implements a linked-list cache for the proxy server using a LRU
 * cache eviction policy. LRU is approximated by moving recently read objects
 * to the end of the list, and evicting objects from the front of list.
 * Thread safety is implemented using semaphores to block writing to the cache
 * until no readers are present.
 */

#include "cache.h"

/* init_cache - Initialize global cache list, shared by all threads, and
 *              return a pointer to the cache.
 */
cache_list *init_cache() {
    cache_list *cache = (cache_list *)Malloc(sizeof(cache_list));  

    Sem_init(&cache->r, 0, 1); // reader semaphore
    Sem_init(&cache->w, 0, 1); // writer semaphore

    cache->first = NULL;
    cache->last = NULL;
    cache->space_left = MAX_CACHE_SIZE;
    cache->readcnt = 0; // number of people trying to read the cache
    return cache;
}

/* init_object - Create a new cache object using malloc and return a pointer
 *               to the object.
 */
cache_object *init_object(char *id, unsigned int length) {

    cache_object *new_object = (cache_object *)Malloc(sizeof(cache_object));

    new_object->id = (char *)Malloc(strlen(id)+1);
    strcpy(new_object->id, id);

    new_object->length = length;
    new_object->data = Malloc(length);
    new_object->next = NULL;
    return new_object;
}

/* open_reader - Add a symbolic 'reader' using semaphores to block writing
 *               until close_reader is called.
 */
void open_reader(cache_list *cache) {
    P(&cache->r);
    cache->readcnt++;   
    if (cache->readcnt == 1)
        P(&cache->w); // Decrement the writing semaphore
    V(&cache->r);
}

/* close_reader - Signify that a reader has left by decrementing the reader
 *                count. If no readers are left, increment (unblock) the
 *                writer semaphore.
 */
void close_reader(cache_list *cache) {
    P(&cache->r);
    cache->readcnt--;
    if (cache->readcnt == 0)
        V(&cache->w); // Increment the writing semaphore
    V(&cache->r);
}

/* search_cache - Traverse the cache list of objects and search for a match.
 *                If a match is found, the object contents  and length are 
 *                read into the query_object buffer and query_length. The
 *                object is then moved to the end of the linked list to mark
 *                it as recently read, and 0 is returned. Otherwise -1 is
 *                returned. This function also uses a simple solution to the
 *                reader-writer problem for thread safety and efficiency.
 */
int search_cache(cache_list *cache, char *query_id, void *query_object,
                 unsigned int *query_length) {

    open_reader(cache);

    /* Traverse cache list to find object that matches query */
    cache_object *match = cache->first; 
    while (match != NULL) {
        if (!strcmp(match->id, query_id))
            break;            
        match = match->next;
    }

    /* Cache hit, read from the cache */
    if (match != NULL) {
        *query_length = match->length;
        memcpy(query_object, match->data, *query_length);
    /* Cache miss */
    } else {
        close_reader(cache);
        return -1;
    } 
    close_reader(cache);
    
    /* Move read node to end of list to enforce LRU policy */
    P(&cache->w);
    if ((match = delete_object(cache, query_id)) == NULL) {
        V(&cache->w); // Make sure to close the writer
        return -1;
    }
    add_to_end(cache, match);
    V(&cache->w); 
    return 0;
}

/* add_to_end - Add an object to the end of the linked list. This is the most
 *              recently used object.
 */
void add_to_end(cache_list *cache, cache_object *object) {
    if (cache->first == NULL) {
        cache->first = object;
        cache->last = object;
        cache->space_left -= object->length;
    } else {
        cache->last->next = object;
        cache->last = object;
        cache->space_left -= object->length;
    }
}

/* delete_object - Delete an object from the cache by rearranging the linked
 *                 list pointers, and return a pointer to the object.
 */
cache_object *delete_object(cache_list *cache, char *query_id) {
    cache_object *prev = NULL;
    cache_object *current = cache->first;

    /* Traverse list and delete requested object */
    while (current != NULL) {
        if (!strcmp(current->id, query_id)) {
            /* Case 1: deleted node is first node */
            if (current == cache->first)
                cache->first = current->next;
            /* Case 2: deleted node is last node */
            if (current == cache->last)
                cache->last = prev;
            /* Case 3: deleted node is not first */
            if (prev != NULL)
                prev->next = current->next;
            current->next = NULL;
            cache->space_left += current->length;
            return current;
        }
        prev = current;
        current = current->next;
    }
    return NULL;
}

/* evict_object - Remove the least recently used (LRU) object from the cache
 *                by removing the first object in the list, and freeing the
 *                object structure.
 */
int evict_object(cache_list *cache) {

    cache_object *object = cache->first;
    if (object == NULL)
        return -1;

    if (object == cache->last)
        cache->last = NULL;
    cache->first = object->next;
    cache->space_left += object->length;

    /* Free the cached object from memory */
    Free(object->id);
    Free(object->data);
    Free(object);
    return 0;
}

/* add_to_cache - Add an object to the cache, if the size is smaller than
 *                MAX_OBJECT_SIZE. If not enough space is available, objects
 *                are evicted from the front of the linked list until enough
 *                space is made.
 */
int add_to_cache(cache_list *cache, char *new_id, void *new_data,
                 unsigned int length) {

    cache_object *new_object = init_object(new_id, length);
    memcpy(new_object->data, new_data, length);

    P(&cache->w);
    while (cache->space_left < new_object->length) {
        if (evict_object(cache) == -1) {
            V(&cache->w); // Make sure to close the writer
            return -1;
        }
    }
    add_to_end(cache, new_object);
    V(&cache->w);

    return 0;
} 

/* destroy_cache - Free the cache from memory if a SIGINT is caught. This may
 *                 not be necessary if the kernel frees memory on exiting a
 *                 process, but it helps with portability.
 */
void destroy_cache(cache_list *cache) {

    printf("SIGINT caught, deleting cache...\n");
    cache_object *current = cache->first;
    cache_object *prev = NULL;

    /* Walk through list and free objects */
    while (current != NULL) {
        prev = current;
        Free(current->data);
        Free(current->id);
        current = current->next;    
        Free(prev);
    }
    Free(cache); /* Finally, delete the cache */
}

/* check_cache - check that there are no cycles in the cache linked
 *               list with tortoise and hare algorithm. Used for debugging.
 */
void check_cache(cache_list *cache) {
    cache_object *tortoise = cache->first; 
    cache_object *hare = cache->first;
    int cycle = 0;

    while (hare != NULL) {
        /* Move hare 1, tortoise 0 */
        if (cycle == 0) {
            hare = hare->next;
            if (tortoise == hare) {
                printf("Cycle detected in cache list!\n");            
                return; 
            }
            cycle = 1;
        /* Move hare 1, tortoise 1 */
        } else if (cycle == 1) {
            hare = hare->next;
            tortoise = tortoise->next;
            if (tortoise == hare) {
                printf("Cycle detected in cache list!\n");            
                return; 
            }
            cycle = 0;
        }
    }
    printf("Cache list check OK\n");
    return;
}
