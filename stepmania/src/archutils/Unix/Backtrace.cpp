#include "global.h"
#include "Backtrace.h"
#include "RageUtil.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#if defined(BACKTRACE_METHOD_X86_LINUX)
#include "LinuxThreadHelpers.h"

void GetCurrentBacktraceContext( BacktraceContext *ctx )
{
	register void *esp __asm__ ("esp");
	ctx->esp = (long) esp;
	ctx->eip = (long) GetThreadBacktraceContext;
	ctx->ebp = (long) __builtin_frame_address(0);
}

int GetThreadBacktraceContext( int ThreadID, BacktraceContext *ctx )
{
	ctx->pid = ThreadID;

	if( ThreadID != GetCurrentThreadId() )
		return GetThreadContext( ThreadID, ctx );

	GetCurrentBacktraceContext( ctx );

	return 0;
}

static const char *itoa(unsigned n)
{
	static char ret[32];
	char *p = ret;
	for( int div = 1000000000; div > 0; div /= 10 )
	{
		*p++ = (n / div) + '0';
		n %= div;
	}
	*p = 0;
	p = ret;
	while( p[0] == '0' && p[1] )
		++p;
	return p;
}

static int xtoi( const char *hex )
{
	int ret = 0;
	while( 1 )
	{
		int val = -1;
		if( *hex >= '0' && *hex <= '9' )
			val = *hex - '0';
		else if( *hex >= 'A' && *hex <= 'F' )
			val = *hex - 'A' + 10;
		else if( *hex >= 'a' && *hex <= 'f' )
			val = *hex - 'a' + 10;
		else
			break;
		hex++;

		ret *= 16;
		ret += val;
	}
	return ret;
}

enum { READABLE_ONLY=1, EXECUTABLE_ONLY=2 };
static int get_readable_ranges( const void **starts, const void **ends, int size, int type=READABLE_ONLY )
{
	char path[PATH_MAX] = "/proc/";
	strcat( path, itoa(getpid()) );
	strcat( path, "/maps" );

	int fd = open(path, O_RDONLY);
	if( fd == -1 )
		return false;

	/* Format:
	 *
	 * 402dd000-402de000 rw-p 00010000 03:03 16815669   /lib/libnsl-2.3.1.so
	 * or
	 * bfffb000-c0000000 rwxp ffffc000 00:00 0
	 *
	 * Look for the range that includes the stack pointer. */
	char file[1024];
	int file_used = 0;
	bool eof = false;
	int got = 0;
	while( !eof && got < size-1 )
	{
		int ret = read( fd, file+file_used, sizeof(file) - file_used);
		if( ret < int(sizeof(file)) - file_used)
			eof = true;

		file_used += ret;

		/* Parse lines. */
		while( got < size )
		{
			char *p = (char *) memchr( file, '\n', file_used );
			if( p == NULL )
				break;
			*p++ = 0;

			char line[1024];
			strcpy( line, file );
			memmove(file, p, file_used);
			file_used -= p-file;

			/* Search for the hyphen. */
			char *hyphen = strchr( line, '-' );
			if( hyphen == NULL )
				continue; /* Parse error. */


			/* Search for the space. */
			char *space = strchr( hyphen, ' ' );
			if( space == NULL )
				continue; /* Parse error. */

			/* " rwxp".  If space[1] isn't 'r', then the block isn't readable. */
			if( type & READABLE_ONLY )
				if( strlen(space) < 2 || space[1] != 'r' )
					continue;
			/* " rwxp".  If space[3] isn't 'x', then the block isn't readable. */
			if( type & EXECUTABLE_ONLY )
				if( strlen(space) < 4 || space[3] != 'x' )
					continue;

			*starts++ = (const void *) xtoi( line );
			*ends++ = (const void *) xtoi( hyphen+1 );
			++got;
		}

		if( file_used == sizeof(file) )
		{
			/* Line longer than the buffer.  Weird; bail. */
			break;
		}
	}

	close(fd);

	*starts++ = NULL;
	*ends++ = NULL;

	return got;
}

/* If the address is readable (eg. reading it won't cause a segfault), return
 * the block it's in.  Otherwise, return -1. */
static int find_address( const void *p, const void **starts, const void **ends )
{
	for( int i = 0; starts[i]; ++i )
	{
		/* Found it. */
		if( starts[i] <= p && p < ends[i] )
			return i;
	}

	return -1;
}

static void *SavedStackPointer = NULL;

void InitializeBacktrace()
{
	static bool bInitialized = false;
	if( bInitialized )
		return;
	bInitialized = true;

	/* We might have a different stack in the signal handler.  Record a pointer
	 * that lies in the real stack, so we can look it up later. */
	register void *esp __asm__ ("esp");
	SavedStackPointer = esp;
}

/* backtrace() for x86 Linux, tested with kernel 2.4.18, glibc 2.3.1. */
static void do_backtrace( const void **buf, size_t size, BacktraceContext *ctx )
{
	/* Read /proc/pid/maps to find the address range of the stack. */
	const void *readable_begin[1024], *readable_end[1024];
	get_readable_ranges( readable_begin, readable_end, 1024 );
		
	/* Find the stack memory blocks. */
	const int stack_block1 = find_address( (void *) ctx->esp, readable_begin, readable_end );
	const int stack_block2 = find_address( SavedStackPointer, readable_begin, readable_end );

	/* This matches the layout of the stack.  The frame pointer makes the
	 * stack a linked list. */
	struct StackFrame
	{
		StackFrame *link;
		char *return_address;

		/* These are only relevant if the frame is a signal trampoline. */
		int signal;
		sigcontext sig;
	};

	StackFrame *frame = (StackFrame *) ctx->ebp;

	unsigned i=0;
	if( i < size-1 ) // -1 for NULL
		buf[i++] = (void *) ctx->eip;

	while( i < size-1 ) // -1 for NULL
	{
		/* Make sure that this frame address is readable, and is on the stack. */
		int val = find_address(frame, readable_begin, readable_end);
		if( val == -1 )
			break;
		if( val != stack_block1 && val != stack_block2 )
			break;

		/* XXX */
		//if( frame->return_address == (void*) CrashSignalHandler )
		//	continue;

		/*
		 * The stack return stub is:
		 *
		 * 0x401139d8 <sigaction+280>:     pop    %eax			0x58
		 * 0x401139d9 <sigaction+281>:     mov    $0x77,%eax	0xb8 0x77 0x00 0x00 0x00
		 * 0x401139de <sigaction+286>:     int    $0x80			0xcd 0x80
		 *
		 * This will be different if using realtime signals, as will the stack layout.
		 *
		 * If we detect this, it means this is a stack frame returning from a signal.
		 * Ignore the return_address and use the sigcontext instead.
		 */
		const char comp[] = { 0x58, 0xb8, 0x77, 0x0, 0x0, 0x0, 0xcd, 0x80 };
		bool is_signal_return = true;

		/* Ugh.  Linux 2.6 is putting the return address in a place that isn't listed
		 * as readable in /proc/pid/maps.  This is probably brittle. */
		if( frame->return_address != (void*)0xffffe420 &&
			find_address(frame->return_address, readable_begin, readable_end) == -1)
			is_signal_return = false;

		for( unsigned pos = 0; is_signal_return && pos < sizeof(comp); ++pos )
			if(frame->return_address[pos] != comp[pos])
				is_signal_return = false;

		void *to_add = NULL;
		if( is_signal_return )
		{
			/* Mark the signal trampoline. */
			if( i < size-1 )
				buf[i++] = BACKTRACE_SIGNAL_TRAMPOLINE;

			to_add = (void *) frame->sig.eip;
		}
		else
			to_add = frame->return_address;

		if( i < size-1 && to_add )
			buf[i++] = to_add;

		/* frame always goes down.  Make sure it doesn't go up; that could
		 * cause an infinite loop. */
		if( frame->link <= frame )
			break;

		frame = frame->link;
	}

	buf[i] = NULL;
}

void GetBacktrace( const void **buf, size_t size, BacktraceContext *ctx )
{
	InitializeBacktrace();
	
	BacktraceContext CurrentCtx;
	if( ctx == NULL )
	{
		ctx = &CurrentCtx;
		GetCurrentBacktraceContext( &CurrentCtx );
	}


	do_backtrace( buf, size, ctx );
}
#elif defined(BACKTRACE_METHOD_BACKTRACE)
#include <execinfo.h>

void InitializeBacktrace() { }
	
void GetBacktrace( const void **buf, size_t size, BacktraceContext *ctx )
{
	InitializeBacktrace();

	int retsize = backtrace( buf, size-1 );

	/* Remove any NULL entries.  We want to null-terminate the list, and null entries are useless. */
	for( int i = retsize-1; i >= 0; --i )
	{
		if( buf[i] != NULL )
			continue;

		memmove( &buf[i], &buf[i]+1, retsize-i-1 );
	}

	buf[retsize] = NULL;
}
#elif defined(BACKTRACE_METHOD_POWERPC_DARWIN)

#include "archutils/Darwin/Crash.h"
typedef struct Frame {
    Frame *stackPointer;
    long conditionReg;
    void *linkReg;
} *FramePtr;

void InitializeBacktrace() { }

void GetBacktrace( const void **buf, size_t size, BacktraceContext *ctx )
{
    FramePtr frame = FramePtr(GetCrashedFramePtr());
    unsigned i;
    for (i=0; frame && frame->linkReg && i<size; ++i, frame=frame->stackPointer)
        buf[i] = frame->linkReg;
    i = (i == size ? size - 1 : i);
    buf[i] = NULL;
}

#else

#warning Undefined BACKTRACE_METHOD_*
void InitializeBacktrace() { }

void GetBacktrace( const void **buf, size_t size, BacktraceContext *ctx )
{
    buf[0] = BACKTRACE_METHOD_NOT_AVAILABLE;
    buf[1] = NULL;
}

#endif
