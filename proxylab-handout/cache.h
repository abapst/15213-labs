/* Cache header file for cache.c
 * Author: Aleksander Bapst (abapst)
 */

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct cache_object {

    struct cache_object *next;
    char *id;
    void *data;
    unsigned int length;

} cache_object;

typedef struct cache_list {

    cache_object *first;
    cache_object *last;
    unsigned int space_left;
    unsigned int readcnt; // number of current readers
    sem_t r,w; // reader-writer semaphores

} cache_list;

cache_list *init_cache();
cache_object *init_object(char *id, unsigned int length);
void open_reader(cache_list *cache);
void close_reader(cache_list *cache);
cache_object *delete_object(cache_list *cache, char *query_id);
void add_to_end(cache_list *cache, cache_object *object);
int evict_object(cache_list *cache);
int search_cache(cache_list *cache, char *query_id, void *cache_object,
                 unsigned int *cache_length);
int add_to_cache(cache_list *cache, char *new_id, void *new_data,
                 unsigned int length);
void destroy_cache(cache_list *cache);
void check_cache(cache_list *cache);
