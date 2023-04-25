#include "tls.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <stdbool.h>
#include <math.h>
#include <signal.h>


/*
 * This is a good place to define any data structures you will use in this file.
 * For example:
 *  - struct TLS: may indicate information about a thread's local storage
 *    (which thread, how much storage, where is the storage in memory)
 *  - struct page: May indicate a shareable unit of memory (we specified in
 *    homework prompt that you don't need to offer fine-grain cloning and CoW,
 *    and that page granularity is sufficient). Relevant information for sharing
 *    could be: where is the shared page's data, and how many threads are sharing it
 *  - Some kind of data structure to help find a TLS, searching by thread ID.
 *    E.g., a list of thread IDs and their related TLS structs, or a hash table.
 */

typedef struct Page
{
	unsigned long int address;
	int ref_count;
}Page;

typedef struct ThreadLocalStorage
{
	pthread_t tid;
	unsigned int size;
	unsigned int page_num;
	struct Page ** pages;
	struct ThreadLocalStorage * next;
	struct ThreadLocalStorage * prev;
	unsigned int key;
}ThreadLocalStorage;
/*
 * Now that data structures are defined, here's a good place to declare any
 * global variables.
 */

ThreadLocalStorage * table[97] = { NULL };
ThreadLocalStorage * head = NULL;
ThreadLocalStorage * tail = NULL;
pthread_mutex_t lock;
bool first_create = true;
unsigned long int page_size = 0;
int count = 0;

/*
 * With global data declared, this is a good point to start defining your
 * static helper functions.
 */
void printList()
{
	for(ThreadLocalStorage * node = head; node != NULL; node = node->next)
	{
		printf("%u %u %u\n", head->key, node->key, tail->key);
	}
}

/*
 * Lastly, here is a good place to add your externally-callable functions.
 */ 

void Insert(ThreadLocalStorage * new)
{
	pthread_t tid = pthread_self();
	int key = tid % 97;
	new->tid = tid;
	if(head != NULL)
	{
		ThreadLocalStorage * ptr = tail;
		ptr->next = new;
		new->prev = ptr;
		new->next = NULL;
		tail = new;
	}
	else
	{
		head = new;
		tail = new;
		new->prev = NULL;
		new->next = NULL;
	}

	if(table[key] != NULL)
	{
		int i = 0;
		while(table[key] != NULL)
		{
			i += 1;
			key = (i * i + key) % 97;
		}
	}
	table[key] = new;
	new->key = key;	
	count++;
}

ThreadLocalStorage * Find(pthread_t tid)
{
	if(head == NULL)
		return NULL;
	if(table[tid % 97]) return table[tid % 97];
	if(tail->tid == tid) return tail;
	if(head->tid == tid)
	{
		return head;
	}
	else
	{
		for(ThreadLocalStorage * node = head; node->next != NULL; node = node->next)
		{
			if(node->tid == tid)
			{
				return node;
			}
		}
	}
	return NULL;
}

static void handle_page_faults(int sig, siginfo_t *si, void * context)
{
	unsigned long int page_fault = (unsigned long int) si->si_addr;
	
	ThreadLocalStorage * node = head;
	while(node != NULL)
	{
		for(int i = 0; i < node->page_num; i++)
		{
			Page * page = node->pages[i];
			if(page->address == page_fault) pthread_exit(NULL);
		}
		node = node->next;
	}
	
	signal(SIGSEGV, SIG_DFL);
	raise(sig);
}

int tls_create(unsigned int size)
{
	if(first_create)
	{
		first_create = false;
		pthread_mutex_init(&lock, NULL);
		struct sigaction action;
		memset(&action, 0, sizeof(struct sigaction));
		action.sa_flags = SA_SIGINFO;
		action.sa_sigaction = handle_page_faults;
		sigaction(SIGSEGV, &action, NULL);
		sigaction(SIGBUS, &action, NULL);
		page_size = getpagesize();
	}


	pthread_t tid = pthread_self();

	if(size < 1 || Find(tid) != NULL) return -1;

	ThreadLocalStorage * node = (ThreadLocalStorage*) malloc(sizeof(ThreadLocalStorage));
	node->size = size;
	node->page_num = size / page_size + 1;	
	node->pages = (Page**) malloc(node->page_num * sizeof(Page*));
	for(int i = 0 ; i < node->page_num; i++)
	{
		Page * page = (Page*) malloc(sizeof(Page));
		page->address = (unsigned long int) mmap(0, page_size, PROT_READ, (MAP_ANON | MAP_PRIVATE), 0, 0);
		if(page->address == (unsigned long int) MAP_FAILED) return -1;
		page->ref_count = 1;
		node->pages[i] = page;
	}

	Insert(node);
	
	return 0;
}

int tls_destroy()
{
	pthread_t tid = pthread_self();
	ThreadLocalStorage * node = Find(tid);	
	if(node == NULL) return -1;
	if(head->key == node->key)
	{
		ThreadLocalStorage * temp;
		if(node->next != NULL)
		{
			temp = node->next;
			temp->prev = NULL;
			head = temp;
		}
		else
		{
			head = NULL;
		}	
	}
	
	if(tail->key == node->key)
	{
		ThreadLocalStorage * temp;
		if(node->prev != NULL)
		{
			temp = node->prev;
			temp->next = NULL;
			tail = temp;
		}
		else
		{
			tail = NULL;
		}
	}
	
	for(int i = 0; i < node->page_num; i++)
	{
		Page * page = node->pages[i];
		if(page->ref_count <= 1)
		{
			if((munmap((void *) page->address, page_size) == 0))
			{
				free(page);
			}
			else exit(1);
		}
		else
		{
			page->ref_count--;
		}
	}
	free(node->pages);
	free(node);
	count--;

	return 0;
}

int tls_read(unsigned int offset, unsigned int length, char *buffer)
{
	pthread_t tid = pthread_self();
	ThreadLocalStorage * node = Find(tid);

	if(node == NULL) return -1;

	if(offset + length > node->size)
	{
		return -1;
	}

	pthread_mutex_lock(&lock);

	for(int i = 0; i < node->page_num; i++)
	{
		if(mprotect((void*) node->pages[i]->address, page_size, PROT_READ) == -1)
		{
			exit(1);
		}
	}
	
	for(unsigned int i = offset; i < offset + length; i++)
	{
		unsigned int pg_num = i / page_size;
		unsigned int pg_offset = i % page_size;
		Page * page = node->pages[pg_num];

		char * first_byte = (char *) (page->address + pg_offset);
		buffer[i - offset] = *first_byte;

	}
	for(int i = 0; i < node->page_num; i++)
	{
		if(mprotect((void *) node->pages[i]->address, page_size, PROT_NONE) == -1)
		{
			exit(1);
		}
	}

	pthread_mutex_unlock(&lock);

	return 0;
}

Page * tls_private_copy(ThreadLocalStorage * node, Page * page, unsigned int page_num)
{
	Page * page_copy = (Page*) malloc(sizeof(Page));
	page_copy->address = (unsigned long int) mmap(0, page_size, PROT_WRITE,(MAP_ANON | MAP_PRIVATE), 0, 0);
	memcpy((void*) page_copy->address, (void*) page->address, page_size);
	page_copy->ref_count = 1;

	return page_copy;
}

int tls_write(unsigned int offset, unsigned int length, const char *buffer)
{
	pthread_t tid = pthread_self();
	ThreadLocalStorage * node = Find(tid);

	if(node == NULL)
	{
		return -1;
	}

	if(offset + length > node->size)
	{
		return -1;
	}

	pthread_mutex_lock(&lock);

	for(int i = 0 ; i < node->page_num; i++)
	{
		if(mprotect((void *) node->pages[i]->address, page_size, PROT_WRITE))
		{
			exit(0);
		}
	}
	for(unsigned int i = offset;  i < offset + length; i++)
	{

		unsigned int pg_num = i / page_size;
		unsigned int pg_offset = i % page_size;
		Page * page = node->pages[pg_num];

		if(page->ref_count >= 2)
		{
			Page * pg_cpy = tls_private_copy(node, page, pg_num);
			node->pages[pg_num] = pg_cpy;
			page->ref_count--;
			if(mprotect((void*) page->address, page_size, PROT_NONE))
			{
				exit(1);
			}
			page = pg_cpy;
		}
		char * first_byte = (char*) (page->address + pg_offset);
		*first_byte = buffer[i - offset];	
	}

	for(int i = 0; i < node->page_num; i++)
	{
		if(mprotect((void *) node->pages[i]->address, page_size, PROT_NONE))
		{
			exit(1);
		}
	}
	
	pthread_mutex_unlock(&lock);

	return 0;
}

int tls_clone(pthread_t tid)
{

	pthread_t clone_tid = pthread_self();
	ThreadLocalStorage * clone = Find(clone_tid);
	
	if(clone != NULL) return -1;
	
	ThreadLocalStorage * node = Find(tid);

	if(node == NULL) return -1;
	
	clone = (ThreadLocalStorage*) malloc(sizeof(ThreadLocalStorage));
	
	clone->page_num = node->page_num;
	clone->size = node->size;

	clone->pages = (Page**) malloc(clone->page_num * sizeof(Page*));

	for(int i = 0 ; i < clone->page_num; i++)
	{
		clone->pages[i] = node->pages[i];
		clone->pages[i]->ref_count++;
	}
	Insert(clone);

	return 0;
}

