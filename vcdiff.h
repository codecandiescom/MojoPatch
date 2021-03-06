#ifndef _INCL_VCDIFF_H_
#define _INCL_VCDIFF_H_

#if !defined(VCDIFF_NO_STDIO)
#include <stdio.h>
#endif

#if defined(_MSC_VER)
    #if !defined(inline)
    #define inline _inline
    #endif
    typedef __int64 int64;
    typedef unsigned __int64 uint64;
    typedef unsigned __int32 uint32;
    typedef unsigned __int8 uint8;
#else
    #include <stdint.h>
    typedef uint64_t uint64;
    typedef int64_t int64;
    typedef uint32_t uint32;
    typedef uint8_t uint8;
#endif

/*
 * These allocators work just like the C runtime's malloc() and free()
 *  (in fact, they probably use malloc() and free() internally if you don't
 *  specify your own allocator, but don't rely on that behaviour).
 * (data) is the pointer you supplied when specifying these allocator
 *  callbacks, in case you need instance-specific data...it is passed through
 *  to your allocator unmolested, and can be NULL if you like.
 */
typedef void *(*vcdiff_malloc)(int bytes, void *data);
typedef void (*vcdiff_free)(void *ptr, void *data);


/*
 * If you need more fined-grained control over i/o than you get from
 *  filenames, you can use these abstracted i/o intefaces with
 *  vcdiff() instead of vcdiff_fname().
 */
typedef struct
{
    int64 (*read)(void *ctx, void *buf, uint32 n);
    int64 (*write)(void *ctx, void *buf, uint32 n);
    int64 (*seek)(void *ctx, uint64 n);
    void *ctx;
} vcdiff_io;


/* !!! FIXME: documentation. */
/*
 *(iosrc) and (iodelta) need read access, (iodst) needs read
 *  AND write access. All three streams must be seekable!
 */
int vcdiff(vcdiff_io *iosrc, vcdiff_io *iodelta, vcdiff_io *iodst,
           vcdiff_malloc m, vcdiff_free f, void *d);


#if !defined(VCDIFF_NO_STDIO)
/* !!! FIXME: documentation. */
/*
 *(fiosrc) and (fiodelta) need read access, (fiodst) needs read
 *  AND write access. All three streams must be seekable!
 */
int vcdiff_stdio(FILE *fiosrc, FILE *fiodelta, FILE *fiodst,
                 vcdiff_malloc m, vcdiff_free f, void *d);
#endif


/* !!! FIXME: documentation. */
/*
 *(src) and (delta) need read access, (dst) needs read
 *  AND write access. All three streams must be seekable!
 */
int vcdiff_fname(const char *src, const char *delta, const char *dst,
                 vcdiff_malloc m, vcdiff_free f, void *d);


#endif  /* include-once blocker. */

/* end of vcdiff.h ... */

