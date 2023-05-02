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

typedef struct Page
{
	unsigned long int address;
	int ref_count;
}Page;

typedef struct ThreadLocalStorage // TLS struct + linked list implementation 
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

ThreadLocalStorage * table[97] = { NULL }; // Table of threads using an arbitrary prime number for hashing
ThreadLocalStorage * head = NULL; // doubly linked list implementation
ThreadLocalStorage * tail = NULL;
pthread_mutex_t lock; // lock for threads
bool first_create = true;
unsigned long int page_size = 0; // keeping track of page size. assigned to getpagesize... could also just be defined as 4096
int count = 0;

/*
 * With global data declared, this is a good point to start defining your
 * static helper functions.
 */
void printList() // printing linked list to make sure structure is working correctly
{
	for(ThreadLocalStorage * node = head; node != NULL; node = node->next)
	{
		printf("%u %u %u\n", head->key, node->key, tail->key);
	}
}

/*
 * Lastly, here is a good place to add your externally-callable functions.
 */ 

void Insert(ThreadLocalStorage * new) // insert node into tail of the doubly linked list
{
	pthread_t tid = pthread_self();
	int key = tid % 97;
	new->tid = tid;
	if(head != NULL) // if there is a head we just find the tail and assign the new node as a tail
	{
		ThreadLocalStorage * ptr = tail;
		ptr->next = new;
		new->prev = ptr;
		new->next = NULL;
		tail = new;
	}
	else // otherwise we declare the new node the head
	{
		head = new;
		tail = new;
		new->prev = NULL;
		new->next = NULL;
	}

	if(table[key] != NULL) // quadratic probing for tls array
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

ThreadLocalStorage * Find(pthread_t tid) // Finding a TLS node. If we don't find it in the table for whatever reason we can search the linked list
{					// useful for functionality in case one data structure fails to store the node properly
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

static void handle_page_faults(int sig, siginfo_t *si, void * context) // get the address of where the page fault happened. we find the page and terminate the thread.
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
	if(first_create) // signal handling for page_faults
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

	ThreadLocalStorage * node = (ThreadLocalStorage*) malloc(sizeof(ThreadLocalStorage)); // malloc new TLS struct and malloc the page array and each page.
	node->size = size;
	node->page_num = size / page_size + 1;	
	node->pages = (Page**) malloc(node->page_num * sizeof(Page*));
	for(int i = 0 ; i < node->page_num; i++)
	{
		Page * page = (Page*) malloc(sizeof(Page));
		page->address = (unsigned long int) mmap(0, page_size, PROT_READ, (MAP_ANON | MAP_PRIVATE), 0, 0); // map page to virtual memory address
		if(page->address == (unsigned long int) MAP_FAILED) return -1; 
		page->ref_count = 1;
		node->pages[i] = page;
	}

	Insert(node); // Inser node to tls array and linked list
	
	return 0;
}

int tls_destroy() // we find the node and remove it from the linked list and free the allocated memory
{
	pthread_t tid = pthread_self();
	ThreadLocalStorage * node = Find(tid);	
	if(node == NULL) return -1;
	if(head->key == node->key) // Could be faulty in the sense that this function assumes that the node will always either be the head or tail. If there were 
	{ // An input tid or key, this could create problems in our linked list.
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
	pthread_t tid = pthread_self(); // We find the node that we want to read and check first if the input variables offset + length is greater than the size of the node
	ThreadLocalStorage * node = Find(tid);

	if(node == NULL) return -1;

	if(offset + length > node->size)
	{
		return -1;
	}

	pthread_mutex_lock(&lock); // lock so to prevent race conditions or unwanted behavior from other threads

	for(int i = 0; i < node->page_num; i++) // change the access permissions of the pages to allow for reads. In retrospect this probably is inefficent
	{ 				// If we are not reading the entire page array, it is probably better to only allow access to the pages to be read.
		if(mprotect((void*) node->pages[i]->address, page_size, PROT_READ) == -1) // but since we acquired a lock it probably is fine but in terms of runtime,
		{     								// it may be inefficient especially if we are doing alot of these system calls constantly
			exit(1);
		}
	}

	char * buffer_ptr = buffer; // create a buffer_ptr that we can increment as me memcpy to the buffer
	unsigned int start_page = offset / page_size; // we want to calculate which page we start and which page we end
	unsigned int end_page = (offset + length) / page_size;

	for(unsigned int i = start_page; i <= end_page; i++) // we loop through the total amount of pages we need
	{
		unsigned int start_offset = 0;
		if(i == start_page) start_offset = offset % page_size; // if we are on the first page, we want to calculate any offset we have on that page
		unsigned int end_offset = page_size; // Given the inputs, we may not want to always start on address 0 on our first page.
		if(i == end_page) end_offset = (offset + length) % page_size; // similarly if we arrive on the end page we want to also account for any offset we may have
									// as we may not want to read the entire page..
		char * page_ptr = (char*) node->pages[i]->address; // we create a ptr to the starting address of the page
		memcpy(buffer_ptr, page_ptr + start_offset, end_offset - start_offset); // increment the page_ptr to the offset we calculated before and 
		buffer_ptr += (end_offset - start_offset); // calculate the amount of bytes we want to cpy into buffer by taking the page_size - offset
	}	// Similarly if we are on the last page, we start at the first byte of that page and cpy the length of bytes calculated by the end offset of the page 

	for(int i = 0; i < node->page_num; i++) // reprotect
	{
		if(mprotect((void *) node->pages[i]->address, page_size, PROT_NONE) == -1)
		{
			exit(1);
		}
	}

	pthread_mutex_unlock(&lock); //release the lock 

	return 0;
}

Page * tls_private_copy(ThreadLocalStorage * node, Page * page, unsigned int page_num) // creates a copy of the page if the page has more than 1 reference.
{										// we do this because we dont want to change the contents of the page for other 
	Page * page_copy = (Page*) malloc(sizeof(Page));			// threads. Doing so could result in race conditions.
	page_copy->address = (unsigned long int) mmap(0, page_size, PROT_WRITE,(MAP_ANON | MAP_PRIVATE), 0, 0);
	memcpy((void*) page_copy->address, (void*) page->address, page_size);
	page_copy->ref_count = 1;

	return page_copy;
}

int tls_write(unsigned int offset, unsigned int length, const char *buffer) // same concept as tls_read except for creating a private page copy
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

	const char * buffer_ptr = buffer;
	unsigned int start_page = offset / page_size;
	unsigned int end_page = (offset + length) / page_size;

	for(unsigned int i = start_page; i <= end_page; i++)
	{
		Page * page = node->pages[i];

		if(page->ref_count >= 2)
		{
			Page * pg_cpy = tls_private_copy(node, page, i);
			node->pages[i] = pg_cpy; // now the page in our tls page table is replaced by the copy, and all other references still refer to the "clean" page
			page->ref_count--; // we want to decrement the ref_count of our clean page as this tls_node page table now contains the copied page.
			if(mprotect((void*) page->address, page_size, PROT_NONE)) // reprotect the old page as now our mprotect loop will be protecting the copy
			{
				exit(1);
			}
		}

		unsigned int start_offset = 0; // same concept as tls_read except we memcpy to the first page + (offset) to the last page size - (offset) from the buffer
		if(i == start_page) start_offset = offset % page_size; 
		unsigned int end_offset = page_size;
		if(i == end_page) end_offset = (offset + length) % page_size;
		char * page_ptr = (char*) node->pages[i]->address;
		memcpy(page_ptr + start_offset, buffer_ptr, end_offset - start_offset);
		buffer_ptr += (end_offset - start_offset);
	}

	for(int i = 0; i < node->page_num; i++) // reprotect pages
	{
		if(mprotect((void *) node->pages[i]->address, page_size, PROT_NONE))
		{
			exit(1);
		}
	}
	
	pthread_mutex_unlock(&lock); // release lock

	return 0;
}

int tls_clone(pthread_t tid) // find the tls node we need to clone using its tid
{

	pthread_t clone_tid = pthread_self(); 
	
	ThreadLocalStorage * node = Find(tid);

	if(node == NULL) return -1;
	
	ThreadLocalStorage * clone = (ThreadLocalStorage*) malloc(sizeof(ThreadLocalStorage)); // allocate memory for a clone tls that has the same variables as our target tls
	
	clone->page_num = node->page_num;
	clone->size = node->size;
	clone->tid = clone_tid;

	clone->pages = (Page**) malloc(clone->page_num * sizeof(Page*));

	for(int i = 0 ; i < clone->page_num; i++)
	{
		clone->pages[i] = (Page*) malloc(sizeof(Page));
		clone->pages[i] = node->pages[i];
		clone->pages[i]->ref_count++;
	}

	Insert(clone); // insert clone

	return 0;
}

