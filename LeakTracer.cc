/* 
 * Homepage: <http://www.andreasen.org/LeakTracer/>
 *
 * Authors:
 *  Erwin S. Andreasen <erwin@andreasen.org>
 *  Henner Zeller <H.Zeller@acm.org>
 *
 * This program is Public Domain
 */

#define TRACE_MALLOC 			// enable tracing of malloc/free/calloc/realloc
#define TRACE_FREE
#define TRACE_CALLOC
#define TRACE_REALLOC

//#define TRACE(arg) fprintf arg
#define TRACE(arg)

#ifdef THREAD_SAVE
#define _THREAD_SAVE
#include <pthread.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <dlfcn.h>
#include <assert.h>

/**
 * dynamic call interfaces to memory allocation functions in libc.so
 */
static void* (*lt_malloc)(size_t size);
static void  (*lt_free)(void* ptr);
static void* (*lt_realloc)(void *ptr, size_t size);
static void* (*lt_calloc)(size_t nmemb, size_t size);

/*
 * underlying allocation, de-allocation used within 
 * this tool
 */
#define LT_MALLOC  (*lt_malloc)
#define LT_FREE    (*lt_free)
#define LT_REALLOC (*lt_realloc)
#define LT_CALLOC  (*lt_calloc)

/*
 * prime number for the address lookup hash table.
 * if you have _really_ many memory allocations, use a
 * higher value, like 343051 for instance.
 */
#define SOME_PRIME 35323
#define ADDR_HASH(addr) ((unsigned long) addr % SOME_PRIME)

/**
 * Filedescriptor to write to. This should not be a low number,
 * because these often have special meanings (stdin, out, err)
 * and may be closed by the program (daemons)
 * So choose an arbitrary higher FileDescriptor .. e.g. 42
 */
#define FILEDESC    42

/**
 * allocate a bit more memory in order to check if there is a memory
 * overwrite. Either 0 or more than sizeof(unsigned int). Note, you can
 * only detect memory over_write_, not _reading_ beyond the boundaries. Better
 * use electric fence for these kind of bugs 
 *   <ftp://ftp.perens.com/pub/ElectricFence/>
 */
#define MAGIC "\xAA\xBB\xCC\xDD"
#ifdef MAGIC
#define MAGIC_SIZE (sizeof(MAGIC)-1)
#else
#define MAGIC_SIZE 0
#endif

/**
 * on 'new', initialize the memory with this value.
 * if not defined - uninitialized. This is very helpful because
 * it detects if you initialize your classes correctly .. if not,
 * this helps you faster to get the segmentation fault you're 
 * implicitly asking for :-). 
 *
 * Set this to some value which is likely to produce a
 * segmentation fault on your platform.
 */
#define SAVEVALUE   0xAA

/**
 * on 'delete', clean memory with this value.
 * if not defined - no memory clean.
 *
 * Set this to some value which is likely to produce a
 * segmentation fault on your platform.
 */
#define MEMCLEAN    0xEE

/**
 * Initial Number of memory allocations in our list.
 * Doubles for each re-allocation.
 */
#define INITIALSIZE 32768

static class LeakTracer {
public:
	struct Leak {
		const void *addr;
		size_t      size;
		const void *allocAddr;
		bool        type;	// true == normal, false == []
		int         nextBucket;
	};
	
	int  newCount;      // how many memory blocks do we have
	int  leaksCount;    // amount of entries in the leaks array
	int  firstFreeSpot; // Where is the first free spot in the leaks array?
	int  currentAllocated; // currentAllocatedMemory
	int  maxAllocated;     // maximum Allocated
	unsigned long totalAllocations; // total number of allocations. stats.
	unsigned int  abortOn;  // resons to abort program (see abortReason_t)

public:
	/**
	 * Have we been initialized yet?  We depend on this being
	 * false before constructor has been called!  
	 */
	bool initialized;
	bool destroyed;		// Has our destructor been called?

private:
	FILE *report;       // filedescriptor to write to

	/**
	 * pre-allocated array of leak info structs.
	 */
	Leak *leaks;

	/**
	 * fast hash to lookup the spot where an allocation is 
	 * stored in case of an delete. map<void*,index-in-leaks-array>
	 */
	int  *leakHash;     // fast lookup

#ifdef THREAD_SAVE
	pthread_mutex_t mutex;
#endif

	enum abortReason_t {
		OVERWRITE_MEMORY    = 0x01,
		DELETE_NONEXISTENT  = 0x02,
		NEW_DELETE_MISMATCH = 0x04
	};

public:
	LeakTracer() {
		TRACE((stderr,"LeakTracer\n"));
		initialize();
	}
	
	void initialize() {
		// Unfortunately we might be called before our constructor has actualy fired
		if (initialized)
			return;

		TRACE((stderr, "LeakTracer::initialize()\n"));
		newCount = 0;
		leaksCount = 0;
		firstFreeSpot = 1; // index '0' is special
		currentAllocated = 0;
		maxAllocated = 0;
		totalAllocations = 0;
		abortOn =  OVERWRITE_MEMORY; // only _severe_ reason
		report = 0;
		leaks = 0;
		leakHash = 0;

		if (!lt_calloc)
		{
			TRACE((stderr, "initialize: dlsym(lt_calloc)\n"));
			lt_calloc = (void*(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
			TRACE((stderr, "initialize: lt_calloc=%p\n", lt_calloc));
			if (!lt_calloc) {
				fprintf(stderr, "LeakTracer: could not resolve 'calloc' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}

		if (!lt_malloc)
		{
			lt_malloc = (void*(*)(size_t))dlsym(RTLD_NEXT, "malloc");
			TRACE((stderr, "initialize: lt_malloc=%p\n", lt_malloc));
			if (!lt_malloc) {
				fprintf(stderr, "LeakTracer: could not resolve 'malloc' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}

		if (!lt_free)
		{
			lt_free = (void(*)(void*))dlsym(RTLD_NEXT, "free");
			TRACE((stderr, "initialize: lt_free=%p\n", lt_free));
			if (!lt_free) {
				fprintf(stderr, "LeakTracer: could not resolve 'free' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}

		if (!lt_realloc)
		{
			lt_realloc = (void*(*)(void*,size_t))dlsym(RTLD_NEXT, "realloc");
			TRACE((stderr, "initialize: lt_realloc=%p\n", lt_realloc));
			if (!lt_realloc) {
				fprintf(stderr, "LeakTracer: could not resolve 'realloc' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}

		// initialize trace file
		
		char uniqFilename[256];
		const char *filename = getenv("LEAKTRACE_FILE") ? : "leak.out";
		struct stat dummy;
		if (stat(filename, &dummy) == 0) {
			sprintf(uniqFilename, "%s.%d", filename, getpid());
			fprintf(stderr, 
				"LeakTracer: file exists; using %s instead\n",
				uniqFilename);
		}
		else {
			sprintf(uniqFilename, "%s", filename);
		}
		int reportfd = open(uniqFilename, 
				    O_WRONLY|O_CREAT|O_TRUNC,S_IREAD|S_IWRITE);
		if (reportfd < 0) {
			fprintf(stderr, "LeakTracer: cannot open %s: %m\n", 
				filename);
			report = stderr;
		}
		else {
			int dupfd = dup2(reportfd, FILEDESC);
			close(reportfd);
			report = fdopen(dupfd, "w");
			if (report == NULL) {
				report = stderr;
			}
		}
		
		time_t t = time(NULL);
		fprintf (report, "# starting %s", ctime(&t));
		leakHash = (int*) LT_MALLOC(SOME_PRIME * sizeof(int));
		memset ((void*) leakHash, 0x00, SOME_PRIME * sizeof(int));

#ifdef MAGIC
		fprintf (report, "# memory overrun protection of %d Bytes\n", MAGIC_SIZE);
#endif
		
#ifdef SAVEVALUE
		fprintf (report, "# initializing new memory with 0x%2X\n",
			 SAVEVALUE);
#endif

#ifdef MEMCLEAN
		fprintf (report, "# sweeping deleted memory with 0x%2X\n",
			 MEMCLEAN);
#endif
		if (getenv("LT_ABORTREASON")) {
			abortOn = atoi(getenv("LT_ABORTREASON"));
		}

#define PRINTREASON(x) if (abortOn & x) fprintf(report, "%s ", #x);
		fprintf (report, "# aborts on ");
		PRINTREASON( OVERWRITE_MEMORY );
		PRINTREASON( DELETE_NONEXISTENT );
		PRINTREASON( NEW_DELETE_MISMATCH );
		fprintf (report, "\n");
#undef PRINTREASON

#ifdef THREAD_SAVE
		fprintf (report, "# thread save\n");
		/*
		 * create default, non-recursive ('fast') mutex
		 * to lock our datastructure where we keep track of
		 * the user's new/deletes
		 */
		if (pthread_mutex_init(&mutex, NULL) < 0) {
			fprintf(report, "# couldn't init mutex ..\n");
			fclose(report);
			_exit(1);
		}
#else
		fprintf(report, "# not thread save; if you use threads, recompile with -DTHREAD_SAVE\n");
#endif
		fflush(report);

		TRACE((stderr, "LeakTracer::initialized\n"));
		initialized = true;
	}
	
	/*
	 * the workhorses:
	 */
	void *registerAlloc(size_t size, bool type, unsigned caller_stack);
	void  registerFree (void *p, bool type, unsigned caller_stack);
	void *registerRealloc (void *p, size_t size, bool type);

	/**
	 * write a hexdump of the given area.
	 */
	void  hexdump(const unsigned char* area, int size);
	
	/**
	 * Terminate current running progam.
	 */
	void progAbort(abortReason_t reason) {
		if (abortOn & reason) {
			fprintf(report, "# abort; DUMP of current state\n");
                        fprintf(stderr, "LeakTracer aborting program due to reason %d\n", reason);
			writeLeakReport();
			fclose(report);
			abort();
		}
		else
			fflush(report);
	}

	/**
	 * write a Report over leaks, e.g. still pending deletes
	 */
	void writeLeakReport();

	~LeakTracer() {
		TRACE((stderr, "~LeakTracer\n"));
		time_t t = time(NULL);
		fprintf (report, "# finished %s", ctime(&t));
		writeLeakReport();
		fclose(report);
		free(leaks);
#ifdef THREAD_SAVE
		pthread_mutex_destroy(&mutex);
#endif
		TRACE((stderr, "~LeakTracer: destroyed"));
		destroyed = true;
	}
} leakTracer;

void* LeakTracer::registerAlloc (size_t size, bool type, unsigned caller_stack) {
	TRACE((stderr, "LeakTracer::registerAlloc(%d, %d, %d)\n", size, type, caller_stack));

	initialize();

	if (destroyed) {
		fprintf(stderr, "Oops, registerAlloc called after destruction of LeakTracer (size=%d)\n", size);
		return LT_MALLOC(size);
	}

	void *p = LT_MALLOC(size + MAGIC_SIZE);

	// Need to call the new-handler
	if (!p) {
		fprintf(report, "LeakTracer malloc %m\n");
		_exit (1);
	}

#ifdef SAVEVALUE
	/* initialize with some defined pattern */
	memset(p, SAVEVALUE, size + MAGIC_SIZE);
#endif
	
#ifdef MAGIC
	/*
	 * the magic value is a special pattern which does not need
	 * to be uniform.
	 */
        memcpy((char*)p+size, MAGIC, MAGIC_SIZE);
#endif

#ifdef THREAD_SAVE
	pthread_mutex_lock(&mutex);
#endif

	++newCount;
	++totalAllocations;
	currentAllocated += size;
	if (currentAllocated > maxAllocated)
		maxAllocated = currentAllocated;
	
	for (;;) {
		for (int i = firstFreeSpot; i < leaksCount; i++)
			if (leaks[i].addr == NULL) {
				leaks[i].addr = p;
				leaks[i].size = size;
				leaks[i].type = type;
				leaks[i].allocAddr= (caller_stack == 1) ? __builtin_return_address(1) :  __builtin_return_address(2),
				firstFreeSpot = i+1;
				// allow to lookup our index fast.
				int *hashPos = &leakHash[ ADDR_HASH(p) ];
				leaks[i].nextBucket = *hashPos;
				*hashPos = i;
				TRACE((stderr, "registerAlloc returning %p\n", p));
#ifdef THREAD_SAVE
				pthread_mutex_unlock(&mutex);
#endif
				return p;
			}
		
		// Allocate a bigger array
		// Note that leaksCount starts out at 0.
		int new_leaksCount = (leaksCount == 0) ? INITIALSIZE 
			                               : leaksCount * 2;
		leaks = (Leak*)LT_REALLOC(leaks, 
					  sizeof(Leak) * new_leaksCount);
		if (!leaks) {
			fprintf(report, "# LeakTracer realloc failed: %m\n");
			_exit(1);
		}
		else {
			fprintf(report, "# internal buffer now %d\n", 
				new_leaksCount);
			fflush(report);
		}
		memset(leaks+leaksCount, 0x00,
		       sizeof(Leak) * (new_leaksCount-leaksCount));
		leaksCount = new_leaksCount;
	}
}

void LeakTracer::hexdump(const unsigned char* area, int size) {
	fprintf(report, "# ");
	for (int j=0; j < size ; ++j) {
		fprintf (report, "%02x ", *(area+j));
		if (j % 16 == 15) {
			fprintf(report, "  ");
			for (int k=-15; k < 0 ; k++) {
				char c = (char) *(area + j + k);
				fprintf (report, "%c", isprint(c) ? c : '.');
			}
			fprintf(report, "\n# ");
		}
	}
	fprintf(report, "\n");
}

void LeakTracer::registerFree (void *p, bool type, unsigned caller_stack) {
	TRACE((stderr, "LeakTracer::registerFree(%p, %d, %d)\n", p, type, caller_stack));

	initialize();

	if (p == NULL)
		return;

	if (destroyed) {
		fprintf(stderr, "Oops, registerFree called after destruction of LeakTracer (p=%p)\n", p);
		return LT_FREE(p);
	}

#ifdef THREAD_SAVE
	pthread_mutex_lock(&mutex);
#endif
	if (leaks) {

		int *lastPointer = &leakHash[ ADDR_HASH(p) ];
		int i = *lastPointer;
		
		while (i != 0 && leaks[i].addr != p) {
			lastPointer = &leaks[i].nextBucket;
			i = *lastPointer;
		}
		
		if (leaks[i].addr == p) {
			*lastPointer = leaks[i].nextBucket; // detach.
			newCount--;
			leaks[i].addr = NULL;
			currentAllocated -= leaks[i].size;
			if (i < firstFreeSpot)
				firstFreeSpot = i;
			
			if (leaks[i].type != type) {
				fprintf(report, 
					"S %10p %10p  # new%s but delete%s "
					"; size %d\n",
					leaks[i].allocAddr,
					(caller_stack == 1) ? __builtin_return_address(1) :  __builtin_return_address(2),
					((!type) ? "[]" : " normal"),
					((type) ? "[]" : " normal"),
					leaks[i].size);
				
				progAbort( NEW_DELETE_MISMATCH );
			}
#ifdef MAGIC
			if (memcmp((char*)p + leaks[i].size, MAGIC, MAGIC_SIZE)) {
				fprintf(report, "O memAddr: %10p memSize: %d allocAddr: %10p freeAddr: %10p "
					"# memory overwritten beyond allocated"
					" %d bytes\n",
					p, leaks[i].size,
					leaks[i].allocAddr,
					(caller_stack == 1) ? __builtin_return_address(1) :  __builtin_return_address(2),
					leaks[i].size);
				fprintf(report, "# %d byte beyond area:\n",
					MAGIC_SIZE);
				hexdump((unsigned char*)p+leaks[i].size,
					MAGIC_SIZE);
				progAbort( OVERWRITE_MEMORY );
			}
#endif
			
#ifdef THREAD_SAVE
#  ifdef MEMCLEAN
			int allocationSize = leaks[i].size;
#  endif
			pthread_mutex_unlock(&mutex);
#else
#define                 allocationSize leaks[i].size
#endif

#ifdef MEMCLEAN
			// set it to some garbage value.
			memset((unsigned char*)p, MEMCLEAN, allocationSize + MAGIC_SIZE);
#endif
			return LT_FREE(p);
		}
	}
#ifdef THREAD_SAVE
	pthread_mutex_unlock(&mutex);
#endif
	fprintf(report, "D %10p             # delete non alloc or twice pointer %10p\n", 
		(caller_stack == 1) ? __builtin_return_address(1) :  __builtin_return_address(2), p);
	progAbort( DELETE_NONEXISTENT );
}

void* LeakTracer::registerRealloc (void* p, size_t size, bool type) {
	TRACE((stderr, "LeakTracer::registerRealloc(%p, %d, %d)\n", p, size, type));

	initialize();

	if (destroyed) {
		fprintf(stderr, "Oops, registerRealloc called after destruction of LeakTracer (size=%d, p=%p)\n", size, p);
		return LT_REALLOC(p,size);
	}

	if (!p)
		return registerAlloc(size, false, 2);

	void* new_p = NULL;
	int  old_size = -1;
	
#ifdef THREAD_SAVE
	pthread_mutex_lock(&mutex);
#endif
	int *lastPointer = &leakHash[ ADDR_HASH(p) ];
	int i = *lastPointer;

	while (i != 0 && leaks[i].addr != p) {
		lastPointer = &leaks[i].nextBucket;
		i = *lastPointer;
	}
	if (leaks[i].addr == p)
		old_size = leaks[i].size;
#ifdef THREAD_SAVE
	pthread_mutex_unlock(&mutex);
#endif
	if (old_size >= 0) {
		if ((int) size > old_size) {
			new_p = registerAlloc(size, type, 2);
			if (new_p) {
				memcpy(new_p, p, old_size);
				registerFree(p, type, 2);
			}
		}
		else
			new_p = p;
	}
	else {
		fprintf(report, "D %10p             # realloc non alloc or twice pointer %10p\n", 
			__builtin_return_address(1), p);
		progAbort( DELETE_NONEXISTENT );
	}
	return new_p;
}

void LeakTracer::writeLeakReport() {
	initialize();

	if (newCount > 0) {
		fprintf(report, "# LeakReport\n");
		fprintf(report, "# %10s | %9s  # Pointer Addr\n",
			"from new @", "size");
	}
	for (int i = 0; i <  leaksCount; i++)
		if (leaks[i].addr != NULL) {
			// This ought to be 64-bit safe?
			fprintf(report, "L %10p   %9ld  # %p\n",
				leaks[i].allocAddr,
				(long) leaks[i].size,
				leaks[i].addr);
		}
	fprintf(report, "# total allocation requests: %6ld ; max. mem used"
		" %d kBytes\n", totalAllocations, maxAllocated / 1024);
	fprintf(report, "# leak %6d Bytes\t:-%c\n", currentAllocated,
		(currentAllocated == 0) ? ')' : '(');
	if (currentAllocated > 50 * 1024) {
		fprintf(report, "# .. that is %d kByte!! A lot ..\n", 
			currentAllocated / 1024);
	}
}

/** -- The actual new/delete operators -- **/

void* operator new(size_t size) {
	TRACE((stderr, "new\n"));
	return leakTracer.registerAlloc(size,false,1);
}


void* operator new[] (size_t size) {
	TRACE((stderr, "new[]\n"));
	return leakTracer.registerAlloc(size,true,1);
}


void operator delete (void *p) {
	TRACE((stderr, "delete(%p)\n",p));
	leakTracer.registerFree(p,false,1);
}


void operator delete[] (void *p) {
	TRACE((stderr, "delete[](%p)\n",p));
	leakTracer.registerFree(p,true,1);
}

/** -- libc memory operators -- **/
void *malloc(size_t size)
{
	TRACE((stderr, "malloc(%d)\n",size));
#ifdef TRACE_MALLOC	
	if (leakTracer.initialized)
		return leakTracer.registerAlloc(size,false,1);
	else
#endif
	{
		if (!lt_malloc)
		{
			lt_malloc = (void*(*)(size_t))dlsym(RTLD_NEXT, "malloc");
			TRACE((stderr, "malloc: lt_malloc=%p\n", lt_malloc));
			if (!lt_malloc) {
				fprintf(stderr, "LeakTracer: could not resolve 'malloc' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}
		if (lt_malloc)
			return LT_MALLOC(size);
		else
		{
			TRACE((stderr, "assert(lt_malloc) failed\n"));
			return NULL;
		}
	}
}

void  free(void* ptr)
{
	TRACE((stderr, "free(%p)\n", ptr));
#ifdef TRACE_FREE
	if (leakTracer.initialized)
		leakTracer.registerFree(ptr,false,1);
	else
#endif
	{
		if (!lt_free)
		{
			lt_free = (void(*)(void*))dlsym(RTLD_NEXT, "free");
			TRACE((stderr, "free: lt_free=%p\n", lt_free));
			if (!lt_free) {
				fprintf(stderr, "LeakTracer: could not resolve 'free' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}

		if (lt_free)
			return LT_FREE(ptr);
		else
		{
			TRACE((stderr, "assert(lt_free) failed\n"));
			return;
		}
		
	}

}
void* realloc(void *ptr, size_t size)
{
	TRACE((stderr, "realloc(%p,%d)\n", ptr, size));
#ifdef TRACE_REALLOC
	if (leakTracer.initialized)
		return leakTracer.registerRealloc(ptr,size,false);
	else
#endif
	{
		if (!lt_realloc)
		{
			lt_realloc = (void*(*)(void*,size_t))dlsym(RTLD_NEXT, "realloc");
			TRACE((stderr, "realloc: lt_realloc=%p\n", lt_realloc));
			if (!lt_realloc) {
				fprintf(stderr, "LeakTracer: could not resolve 'realloc' in 'libc.so': %s\n", dlerror());
				exit(1);
			}
		}
		if (lt_realloc)
			return LT_REALLOC(ptr, size);
		else
		{
			TRACE((stderr, "assert(lt_realloc) failed\n"));
			return NULL;
		}
	}
}

void* calloc(size_t nmemb, size_t size)
{
	TRACE((stderr, "calloc(%d,%d)\n",nmemb, size));
#ifdef TRACE_CALLOC
	if (leakTracer.initialized)
	{
		size_t total = nmemb * size;
		void * ptr = leakTracer.registerAlloc(total,false,1);
		if (ptr)
		{
			TRACE((stderr,"memset(%p, 0, %d)\n", ptr, total));
			memset (ptr, 0, total);
		}
		return ptr;
	}
	else
#endif
	{
		if (lt_calloc)
			return LT_CALLOC(nmemb, size);
		else
		{

			// The problem is that malloc or calloc call dlsym, which in turn
			// (through _dlerror_run) call calloc and thus end up in endless
			// recursion. Fortunately, in _dlerror_run nothing bad happens if
			// calloc returns NULL,	because dlsym(_dlerror_run() will use
			// a static buffer 
			TRACE((stderr, "assert(lt_calloc) failed\n"));
			return NULL;
		}
	}
}
/* Emacs: 
 * Local variables:
 * c-basic-offset: 8
 * End:
 * vi:set tabstop=8 shiftwidth=8 nowrap: 
 */