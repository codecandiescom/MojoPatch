/**
 * MojoPatch; a tool for updating data in the field.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

/*
 *----------------------------------------------------------------------------
 *
 * (Insert documentation here.)
 *
 *----------------------------------------------------------------------------
 *
 *  The latest version of MojoPatch is available here:
 *    http://icculus.org/mojopatch/
 *
 *  This software was written quickly, is not well-engineered, and may have
 *   catastrophic bugs. Its method is brute-force, at best. Use at your
 *   own risk. Don't eat yellow snow.
 *
 *  Please see the file LICENSE.txt in the root of the source tree.
 *
 *   Send patches, improvements, suggestions, etc to Ryan:
 *    icculus@icculus.org.
 *
 *----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#include "platform.h"
#include "ui.h"
#include "md5.h"

#define VER_EXT_ZLIB

#if USE_ZLIB
#include "zlib.h"
#undef VER_EXT_ZLIB
#define VER_EXT_ZLIB " (w/zlib)"
#endif

/*
 * The version string is really file format version, not program version.
 *  This is to prevent incompatible builds of the program from (mis)processing
 *  a patchfile.
 */
#define VERSION "0.0.7" VER_EXT_ZLIB

#define DEFAULT_PATCHFILENAME "default.mojopatch"

#define MOJOPATCHSIG "mojopatch " VERSION ": http://icculus.org/mojopatch/\r\n"

#define STATIC_STRING_SIZE 1024

static void make_static_string(char *statstr, const char *str)
{
    size_t len = strlen(str);
    if (len >= STATIC_STRING_SIZE)
        _fatal("Unexpected data in make_static_string()");
    else
        strcpy(statstr, str);
} /* make_static_string */


typedef struct
{
    char signature[sizeof (MOJOPATCHSIG)];
    char product[STATIC_STRING_SIZE];
    char identifier[STATIC_STRING_SIZE];
    char version[STATIC_STRING_SIZE];
    char newversion[STATIC_STRING_SIZE];
    char readmefname[STATIC_STRING_SIZE];
    char *readmedata;
    char renamedir[STATIC_STRING_SIZE];
    char titlebar[STATIC_STRING_SIZE];
    char startupmsg[STATIC_STRING_SIZE];
} PatchHeader;

typedef enum
{
    OPERATION_DELETE = 0,
    OPERATION_DELETEDIRECTORY,
    OPERATION_ADD,
    OPERATION_ADDDIRECTORY,
    OPERATION_PATCH,
    OPERATION_REPLACE,
    OPERATION_DONE,
    OPERATION_TOTAL /* must be last! */
} OperationType;

typedef struct
{
    OperationType operation;
    char fname[STATIC_STRING_SIZE];
} DeleteOperation;

typedef struct
{
    OperationType operation;
    char fname[STATIC_STRING_SIZE];
} DeleteDirOperation;

typedef struct
{
    OperationType operation;
    char fname[STATIC_STRING_SIZE];
    unsigned int fsize;
    md5_byte_t md5[16];
    unsigned int mode;
} AddOperation;

typedef struct
{
    OperationType operation;
    char fname[STATIC_STRING_SIZE];
    unsigned int mode;
} AddDirOperation;

typedef struct
{
    OperationType operation;
    char fname[STATIC_STRING_SIZE];
    md5_byte_t md5_1[16];
    md5_byte_t md5_2[16];
    unsigned int fsize;
    unsigned int deltasize;
    unsigned int mode;
} PatchOperation;

typedef struct
{
    OperationType operation;
} DoneOperation;

typedef union
{
    OperationType operation;
    DeleteOperation del;
    DeleteDirOperation deldir;
    AddOperation add;
    AddDirOperation adddir;
    PatchOperation patch;
    AddOperation replace;
    DoneOperation done;
} Operations;

typedef struct
{
    FILE *io;
    int reading;
} SerialArchive;

typedef enum
{
    COMMAND_NONE = 0,
    COMMAND_CREATE,
    COMMAND_INFO,
    COMMAND_DOPATCHING,

    COMMAND_TOTAL
} PatchCommands;

typedef enum
{
    ZLIB_NONE = 0,
    ZLIB_COMPRESS,
    ZLIB_UNCOMPRESS
} ZlibOptions;

typedef enum
{
    ISPATCHABLE_ERROR,
    ISPATCHABLE_YES,
    ISPATCHABLE_NO,
    ISPATCHABLE_MATCHES,
} IsPatchable;

static int debug = 0;
static int interactive = 0;
static int replace = 0;
static int appending = 0;
static int alwaysadd = 0;
static int quietonsuccess = 0;
static int skip_patch = 0;  /* global flag to skip current patch. */
static int zliblevel = 9;
static PatchCommands command = COMMAND_NONE;

static const char *patchfile = NULL;
static const char *dir1 = NULL;
static const char *dir2 = NULL;

static char *patchtmpfile = NULL;
static char *patchtmpfile2 = NULL;

static PatchHeader header;

static char **ignorelist = NULL;
static int ignorecount = 0;

char *patchfiledir = NULL;

static unsigned int maxxdeltamem = 128;  /* in megabytes. */

static unsigned char iobuf[512 * 1024];

#if USE_ZLIB
static unsigned char compbuf[520 * 1024];
#endif


static int flush_archive(SerialArchive *ar)
{
    if ((ar->reading) || (ar->io == NULL))
        return(PATCHSUCCESS);

    return((fflush(ar->io) == 0) ? PATCHSUCCESS : PATCHERROR);
} /* flush_archive */


static int serialize(SerialArchive *ar, void *val, size_t size)
{
    int rc;

    if (size == 0)
        return(1);

    if (ar->reading)
        rc = fread(val, size, 1, ar->io);
    else
        rc = fwrite(val, size, 1, ar->io);

    if ( (rc != 1) && (ferror(ar->io)) )
        _fatal("%s error: %s.", ar->reading ? "Read":"Write", strerror(errno));

    return(rc == 1);  /* may fail without calling _fatal() on EOF! */
} /* serialize */


#define SERIALIZE(ar, x) serialize(ar, &x, sizeof (x))

static inline unsigned int swapui32(unsigned int x)
{
#if PLATFORM_BIGENDIAN
    return (((x)>>24) + (((x)>>8)&0xff00) + (((x)<<8)&0xff0000) + ((x)<<24));
#else
    return (x);
#endif
}

static int serialize_uint32(SerialArchive *ar, unsigned int *val)
{
    assert(sizeof (unsigned int) == 4);
    unsigned int x = *val;

    if (!ar->reading)
	    x = swapui32(x);

    if (!SERIALIZE(ar, x))
        return(0);

    *val = swapui32(x);
    return(1);
} /* serialize_uint32 */


static int serialize_static_string(SerialArchive *ar, char *val)
{
    unsigned int len = 0;

    if (!ar->reading)
        len = (unsigned int) strlen(val);

    if (!serialize_uint32(ar, &len))
        return(0);

    if (len >= STATIC_STRING_SIZE)
    {
        _fatal("Bogus string data in patchfile.");
        return(0);
    } /* if */

    val[len] = 0;
    return(serialize(ar, val, len));
} /* serialize_static_string */


/*
 * This will only overwrite (val)'s contents if (val) is empty ("!val[0]").
 *  Mostly, this lets us optionally override patchfiles from the command line.
 *  Writing is done, regardless of string's state.
 */
static int serialize_static_string_if_empty(SerialArchive *ar, char *val)
{
    if ((!ar->reading) || (val[0] == '\0'))
        return(serialize_static_string(ar, val));
    else
    {
        char buffer[STATIC_STRING_SIZE];
        return(serialize_static_string(ar, buffer));  /* throw away data */
    } /* else */
} /* serialize_static_string_if_empty */


static int serialize_asciz_string(SerialArchive *ar, char **_buffer)
{
    char *buffer = *_buffer;
    size_t i = 0;
    size_t allocated = 0;
    int ch;

    if (!ar->reading)
        return(serialize(ar, buffer, strlen(buffer) + 1));

    buffer = NULL;

    /* read an arbitrary amount, allocate storage... */
    do
    {
        if (i <= allocated)
        {
            allocated += 128;
            buffer = realloc(buffer, allocated);
            if (buffer == NULL)
            {
                _fatal("Out of memory.");
                return(0);
            } /* if */
        } /* if */

        ch = fgetc(ar->io);
        if (ch == EOF)
        {
            if (feof(ar->io))
                _fatal("Unexpected EOF during read.");
            else
                _fatal("Error during read: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */

        buffer[i] = (char) ch;
    } while (buffer[i++] != '\0');

    *_buffer = buffer;
    return(1);
} /* serialize_asciz_string */


static int serialize_header(SerialArchive *ar, PatchHeader *h, int *legitEOF)
{
    int rc;
    int dummy = 0;
    if (legitEOF == NULL)
        legitEOF = &dummy;

    assert(sizeof (h->signature) == sizeof (MOJOPATCHSIG));
    memcpy(h->signature, MOJOPATCHSIG, sizeof (h->signature));

    rc = SERIALIZE(ar, h->signature);
    *legitEOF = ( (feof(ar->io)) && (!ferror(ar->io)) );
    if (!rc)
        return(*legitEOF);

    if (memcmp(h->signature, MOJOPATCHSIG, sizeof (h->signature)) != 0)
    {
        h->signature[sizeof (h->signature) - 1] = '\0';  /* just in case. */
        _fatal("[%s] is not a compatible mojopatch file.", patchfile);
        _log("signature is: %s.", h->signature);
        _log("    expected: %s.", MOJOPATCHSIG);
        return(PATCHERROR);
    } /* if */

    if (serialize_static_string_if_empty(ar, h->product))
    if (serialize_static_string_if_empty(ar, h->identifier))
    if (serialize_static_string_if_empty(ar, h->version))
    if (serialize_static_string_if_empty(ar, h->newversion))
    if (serialize_static_string_if_empty(ar, h->readmefname))
    if (serialize_asciz_string(ar, &h->readmedata))
    if (serialize_static_string_if_empty(ar, h->renamedir))
    if (serialize_static_string_if_empty(ar, h->titlebar))
    if (serialize_static_string_if_empty(ar, h->startupmsg))
        return(flush_archive(ar));

    return(0);
} /* serialize_header */

static int serialize_delete_op(SerialArchive *ar, void *d)
{
    DeleteOperation *del = (DeleteOperation *) d;
    assert(del->operation == OPERATION_DELETE);
    if (serialize_static_string(ar, del->fname))
        return(1);

    return(0);
} /* serialize_delete_op */

static int serialize_deldir_op(SerialArchive *ar, void *d)
{
    DeleteDirOperation *deldir = (DeleteDirOperation *) d;
    assert(deldir->operation == OPERATION_DELETEDIRECTORY);
    if (serialize_static_string(ar, deldir->fname))
        return(1);

    return(0);
} /* serialize_deldir_op */

static int serialize_add_op(SerialArchive *ar, void *d)
{
    AddOperation *add = (AddOperation *) d;
    assert((add->operation == OPERATION_ADD) ||
           (add->operation == OPERATION_REPLACE));
    if (serialize_static_string(ar, add->fname))
    if (serialize_uint32(ar, &add->fsize))
    if (SERIALIZE(ar, add->md5))
    if (serialize_uint32(ar, &add->mode))
        return(1);

    return(0);
} /* serialize_add_op */

static int serialize_adddir_op(SerialArchive *ar, void *d)
{
    AddDirOperation *adddir = (AddDirOperation *) d;
    assert(adddir->operation == OPERATION_ADDDIRECTORY);
    if (serialize_static_string(ar, adddir->fname))
    if (serialize_uint32(ar, &adddir->mode))
        return(1);

    return(0);
} /* serialize_adddir_op */

static int serialize_patch_op(SerialArchive *ar, void *d)
{
    PatchOperation *patch = (PatchOperation *) d;
    assert(patch->operation == OPERATION_PATCH);
    if (serialize_static_string(ar, patch->fname))
    if (SERIALIZE(ar, patch->md5_1))
    if (SERIALIZE(ar, patch->md5_2))
    if (serialize_uint32(ar, &patch->fsize))
    if (serialize_uint32(ar, &patch->deltasize))
    if (serialize_uint32(ar, &patch->mode))
        return(1);

    return(0);
} /* serialize_patch_op */

static int serialize_replace_op(SerialArchive *ar, void *d)
{
    AddOperation *add = (AddOperation *) d;
    assert(add->operation == OPERATION_REPLACE);
    return(serialize_add_op(ar, d));
} /* serialize_replace_op */

static int serialize_done_op(SerialArchive *ar, void *d)
{
    DoneOperation *done = (DoneOperation *) d;
    assert(done->operation == OPERATION_DONE);
    return(1);
} /* serialize_done_op */


typedef int (*OpSerializers)(SerialArchive *ar, void *data);
static OpSerializers serializers[OPERATION_TOTAL] =
{
    /* Must match OperationType order! */
    serialize_delete_op,
    serialize_deldir_op,
    serialize_add_op,
    serialize_adddir_op,
    serialize_patch_op,
    serialize_replace_op,
    serialize_done_op,
};



static int handle_delete_op(SerialArchive *ar, OperationType op, void *data);
static int handle_deldir_op(SerialArchive *ar, OperationType op, void *data);
static int handle_add_op(SerialArchive *ar, OperationType op, void *data);
static int handle_adddir_op(SerialArchive *ar, OperationType op, void *data);
static int handle_patch_op(SerialArchive *ar, OperationType op, void *data);
static int handle_replace_op(SerialArchive *ar, OperationType op, void *data);
static int handle_done_op(SerialArchive *ar, OperationType op, void *data);

typedef int (*OpHandlers)(SerialArchive *ar, OperationType op, void *data);
static OpHandlers operation_handlers[OPERATION_TOTAL] =
{
    /* Must match OperationType order! */
    handle_delete_op,
    handle_deldir_op,
    handle_add_op,
    handle_adddir_op,
    handle_patch_op,
    handle_replace_op,
    handle_done_op,
};



static int serialize_operation(SerialArchive *ar, Operations *ops)
{
    unsigned char op = (unsigned char) ops->operation;
    if (!SERIALIZE(ar, op))
        return(0);

    ops->operation = (OperationType) op;
    if ((ops->operation < 0) || (ops->operation >= OPERATION_TOTAL))
    {
        _fatal("Invalid operation in patch file.");
        return(0);
    } /* if */

    if (!serializers[ops->operation](ar, ops))
        return(0);

    return(flush_archive(ar));
} /* serialize_operation */


static int open_serialized_archive(SerialArchive *ar,
                                   const char *fname,
                                   int is_reading,
                                   int *sizeok,
                                   unsigned int *file_size)
{
    memset(ar, '\0', sizeof (*ar));
    if (strcmp(fname, "-") == 0)  /* read from stdin? */
        ar->io = (is_reading) ? stdin : stdout;
    else
	{
        const char *fopenstr = "rb";
        if (!is_reading)
            fopenstr = ((appending) ? "ab" : "wb");

        if (file_size != NULL)
        {
            int tmp = get_file_size(patchfile, file_size);
            if (sizeok != NULL)
                *sizeok = tmp;
        } /* if */

        ar->io = fopen(patchfile, fopenstr);
        if (ar->io == NULL)
        {
            _fatal("Couldn't open [%s]: %s.", patchfile, strerror(errno));
            return(PATCHERROR);
        } /* if */
	} /* else */

    ar->reading = is_reading;
    return(PATCHSUCCESS);
} /* open_serialized_archive */


static inline int close_serialized_archive(SerialArchive *ar)
{
    if (ar->io != NULL)
    {
        if (fclose(ar->io) == EOF)
            return(PATCHERROR);

        ar->io = NULL;
    } /* if */

    return(PATCHSUCCESS);
} /* close_serialized_archive */


/* printf-style: makes string for UI to put in the log. */
void _fatal(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = '\0';
    ui_fatal(buf);
    ui_pump();
} /* _fatal */

/* printf-style: makes string for UI to put in the log. */
void _log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = '\0';
    ui_add_to_log(buf, 0);
    ui_pump();
} /* _log */


/* printf-style: makes string for UI to put in the log if debugging enabled. */
void _dlog(const char *fmt, ...)
{
    if (debug)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof (buf), fmt, ap);
        va_end(ap);
        buf[sizeof(buf)-1] = '\0';
        ui_add_to_log(buf, 1);
        ui_pump();
    } /* if */
} /* _dlog */

static void _current_operation(const char *fmt, ...)
{
    char buf[512];
    if (skip_patch)
        strcpy(buf, "Skipping ahead...");
    else
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof (buf), fmt, ap);
        va_end(ap);
        buf[sizeof(buf)-1] = '\0';
    } /* else */
    ui_status(buf);
    ui_pump();
} /* _current_operation */


/* !!! FIXME: don't taunt this function. */
/*
 * ver == installed app's current version.
 * allowed_ver == allowed versions ("123", "123 or 456", "less than 789").
 * newver == version that patched app will become.
 */
IsPatchable version_ok(const char *ver, const char *allowed_ver, const char *newver)
{
    char *ptr;
    char *buf;

    if (*allowed_ver == '\0')  /* No specified version? Anything is okay. */
        return ISPATCHABLE_YES;

    if (strcmp(ver, newver) == 0)  /* all patched up? */
        return ISPATCHABLE_MATCHES;

    buf = (char *) alloca(strlen(allowed_ver) + 1);
    strcpy(buf, allowed_ver);

    const char *lessThan = "less than ";
    const size_t lessThanLen = strlen(lessThan);
    if (strncmp(buf, lessThan, lessThanLen) == 0)
    {
        char *endptr = NULL;
        double dver;
        double dallow;
        ptr = buf + lessThanLen;

        dver = strtod(ver, &endptr);
        if (endptr == ver)
            return ISPATCHABLE_ERROR;

        dallow = strtod(ptr, &endptr);
        if (endptr == ptr)
            return ISPATCHABLE_ERROR;

        if (dver < dallow)
            return(ISPATCHABLE_YES);

        return(ISPATCHABLE_NO);
    } /* if */

    while ((ptr = strstr(buf, " or ")) != NULL)
    {
        *ptr = '\0';
        if (strcmp(ver, buf) == 0)
            return(ISPATCHABLE_YES);

        buf = ptr + 4;
    } /* while */

    return( (strcmp(ver, buf) == 0) ? ISPATCHABLE_YES : ISPATCHABLE_NO );
} /* version_ok */


static IsPatchable check_product_version(const char *ident,
                                         const char *version,
                                         const char *newversion)
{
    char buf[128];
    IsPatchable retval = ISPATCHABLE_ERROR;

    if (!get_product_version(ident, buf, sizeof (buf)))
        _fatal("Can't determine product's installed version.");
    else
    {
        retval = version_ok(buf, version, newversion);
        if (retval == ISPATCHABLE_MATCHES)
            _fatal("You seem to be all patched up already!");
        else if (retval == ISPATCHABLE_ERROR)
            _fatal("This patch is misbuilt! Contact your vendor!");
        else if (retval == ISPATCHABLE_NO)
        {
            _fatal("This patch applies to version '%s', but you have '%s'.",
                    version, buf);
        } /* else */
        else
        {
            assert(retval == ISPATCHABLE_YES);
        } /* else */
    } /* else */

    return(retval);
} /* check_product_version */


static int _do_xdelta(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = '\0';
	_dlog("(xdelta call: [%s].)", buf);
    return(spawn_xdelta(buf) == SPAWN_RETURNGOOD);
} /* _do_xdelta */


static int in_ignore_list(const char *fname)
{
    int i;
    for (i = 0; i < ignorecount; i++)
    {
        if (strcmp(fname, ignorelist[i]) == 0)
        {
            _log("Ignoring %s on user's instructions.", fname);
            return(1);
        } /* if */
    } /* for */

    return(0);
} /* in_ignore_list */


static inline int info_only(void)
{
    return((command == COMMAND_INFO) || (skip_patch));
} /* info_only */


static void free_filelist(file_list *list)
{
    file_list *next;
    while (list != NULL)
    {
        next = list->next;
        free(list->fname);
        free(list);
        list = next;
    } /* while */
} /* free_filelist */


#if USE_ZLIB
static int write_between_files_compress(FILE *in, FILE *out, long fsize)
{
    uLongf compsize;
    uLongf uncompsize;
    unsigned int uncompsizeui32;
    unsigned int compsizeui32;

    while (fsize > 0)
    {
        uncompsize = sizeof (iobuf);
        if (uncompsize > fsize)
            uncompsize = fsize;

        if (fread(iobuf, uncompsize, 1, in) != 1)
        {
            _fatal("read error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        ui_pump();

        fsize -= uncompsize;

        compsize = sizeof (compbuf);
        if (compress2(compbuf, &compsize, iobuf, uncompsize, zliblevel)!=Z_OK)
        {
            _fatal("zlib compression error.");
            return(PATCHERROR);
        } /* if */
        ui_pump();

        /* !!! FIXME: serialize? */
        uncompsizeui32 = swapui32(uncompsize);
        compsizeui32 = swapui32(compsize);
        if ( (fwrite(&uncompsizeui32, sizeof (uncompsizeui32), 1, out) != 1) ||
             (fwrite(&compsizeui32, sizeof (compsizeui32), 1, out) != 1) ||
             (fwrite(compbuf, compsize, 1, out) != 1) )
        {
            _fatal("write error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        ui_pump();
    } /* while */

    return(fflush(out) == 0 ? PATCHSUCCESS : PATCHERROR);
} /* write_between_files_compress */


static int write_between_files_uncompress(FILE *in, FILE *out,
                                          long fsize, int skip)
{
    uLongf compsize;
    uLongf uncompsize;
    unsigned int uncompsizeui32;
    unsigned int compsizeui32;

    while (fsize > 0)
    {
        if ( (fread(&uncompsizeui32, sizeof (uncompsizeui32), 1, in) != 1) ||
             (fread(&compsizeui32, sizeof (compsizeui32), 1, in) != 1) )
        {
            _fatal("read error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */

        /* !!! FIXME: serialize? */
        uncompsize = swapui32(uncompsizeui32);
        compsize = swapui32(compsizeui32);

        if ( (compsize > sizeof (compbuf)) || (uncompsize > sizeof (iobuf)) )
        {
            _fatal("bogus compression data.");
            return(PATCHERROR);
        } /* if */

        /* fsize is the uncompressed file size... */
        fsize -= uncompsize;

        if (skip)
        {
            if (fseek(in, compsize, SEEK_CUR) < 0)
            {
                _fatal("seek error: %s.", strerror(errno));
                return(PATCHERROR);
            } /* if */
        } /* if */

        else
        {
            if (fread(compbuf, compsize, 1, in) != 1)
            {
                _fatal("read error: %s.", strerror(errno));
                return(PATCHERROR);
            } /* if */
            ui_pump();

            if (uncompress(iobuf, &uncompsize, compbuf, compsize) != Z_OK)
            {
                _fatal("zlib decompression error.");
                return(PATCHERROR);
            } /* if */
            ui_pump();

            if (fwrite(iobuf, uncompsize, 1, out) != 1)
            {
                _fatal("write error: %s.", strerror(errno));
                return(PATCHERROR);
            } /* if */
        } /* else */

        ui_pump();
    } /* while */

    return(fflush(out) == 0 ? PATCHSUCCESS : PATCHERROR);
} /* write_between_files_uncompress */
#endif


static int write_between_files(FILE *in, FILE *out, long fsize, ZlibOptions z)
{
    #if USE_ZLIB
    if (z == ZLIB_COMPRESS)
        return(write_between_files_compress(in, out, fsize));
    else if (z == ZLIB_UNCOMPRESS)
        return(write_between_files_uncompress(in, out, fsize, 0));
    else
        assert(z == ZLIB_NONE);
    #endif

    while (fsize > 0)
    {
        int max = sizeof (iobuf);
        if (max > fsize)
            max = fsize;

        if (fread(iobuf, max, 1, in) != 1)
        {
            _fatal("read error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        ui_pump();

        fsize -= max;

        if (fwrite(iobuf, max, 1, out) != 1)
        {
            _fatal("write error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        ui_pump();
    } /* while */

    return(fflush(out) == 0 ? PATCHSUCCESS : PATCHERROR);
} /* write_between_files */


static int do_rename(const char *from, const char *to)
{
    FILE *in;
    FILE *out;
    unsigned int fsize;
    int rc;

    unlink(to);  /* just in case. */
    if (rename(from, to) != -1)
        return(PATCHSUCCESS);

    /* rename() might fail if from and to are on seperate filesystems. */

    rc = get_file_size(from, &fsize);
    in = fopen(from, "rb");
    out = fopen(to, "wb");
    if ((!rc) || (!in) || (!out))
    {
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        unlink(to);
        _fatal("File copy failed.");
        return(PATCHERROR);
    } /* if */

    rc = write_between_files(in, out, fsize, ZLIB_NONE);

    fclose(in);
    if ((fclose(out) == -1) && (rc != PATCHERROR))
    {
        _fatal("File copy failed.");
        return(PATCHERROR);
    } /* if */

    unlink(from);

    return(rc);
} /* do_rename */


static int md5sum(FILE *in, md5_byte_t *digest, int output)
{
    md5_state_t md5state;
    long br;

    _dlog("md5summing...");

    memset(digest, '\0', 16);
    md5_init(&md5state);

    if (fseek(in, 0, SEEK_SET) == -1)
    {
        _fatal("Couldn't seek in file: %s.", strerror(errno));
        return(PATCHERROR);
    } /* if */

    while (1)
    {
        ui_pump();

        br = fread(iobuf, 1, sizeof (iobuf), in);
        if (br == 0)
        {
            int err = errno;
            if (feof(in))
                break;
            else
            {
                _fatal("Read error: %s.", strerror(err));
                return(PATCHERROR);
            } /* else */
        } /* if */
        md5_append(&md5state, (const md5_byte_t *) iobuf, br);
    } /* while */

    md5_finish(&md5state, digest);

    if ((output) || (debug))
    {
      /* ugly, but want to print it all on one line... */
        _log("  (md5sum: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x)",
              digest[0],  digest[1],  digest[2],  digest[3],
              digest[4],  digest[5],  digest[6],  digest[7],
              digest[8],  digest[9],  digest[10], digest[11],
              digest[12], digest[13], digest[14], digest[15]);
    } /* if */

    if (fseek(in, 0, SEEK_SET) == -1)
    {
        _fatal("Couldn't seek in file: %s.", strerror(errno));
        return(PATCHERROR);
    } /* if */

    return(PATCHSUCCESS);
} /* md5sum */


static int verify_md5sum(md5_byte_t *md5, md5_byte_t *result, FILE *in, int isfatal)
{
    md5_byte_t thismd5[16];

    if (md5sum(in, thismd5, 0) == PATCHERROR)
        return(PATCHERROR);

    if (result != NULL)
		memcpy(result, thismd5, sizeof (thismd5));

    if (memcmp(thismd5, md5, sizeof (thismd5)) != 0)
    {
        if (isfatal)
            _fatal("md5sum doesn't match original!");
        return(PATCHERROR);
    } /* if */
    
    return(PATCHSUCCESS);
} /* verify_md5sum */


/* !!! FIXME: This should be in the UI abstraction. */
static int confirm(void)
{
    char buf[256];
    char *ptr;

    if (!interactive)
        return(1);

    while (1)
    {
        printf("Confirm [Y/n] : ");
        fgets(buf, sizeof (buf) - 1, stdin);
        if ( (ptr = strchr(buf, '\r')) != NULL )
            *ptr = '\0';
        if ( (ptr = strchr(buf, '\n')) != NULL )
            *ptr = '\0';

        if (strlen(buf) <= 1)
        {
            int ch = tolower((int) buf[0]);
            if ((ch == '\0') || (ch == 'y'))
            {
                printf("Answered YES\n");
                return(1);
            } /* if */
            else if (ch == 'n')
            {
                printf("Answered NO\n");
                return(0);
            } /* else if */
        } /* if */
    } /* while */
} /* confirm */


static const char *final_path_element(const char *fname)
{
    const char *ptr = (const char *) strrchr(fname, PATH_SEP[0]);
    assert( (sizeof (PATH_SEP)) == (sizeof (char) * 2) );
    return(ptr ? ptr + 1 : fname);
} /* final_path_element */


/* put a DELETE operation in the mojopatch file... */
static int put_delete(SerialArchive *ar, const char *fname)
{
    Operations ops;

    _current_operation("DELETE %s", final_path_element(fname));
    _log("DELETE %s", fname);

    if (in_ignore_list(fname))
        return(PATCHSUCCESS);

    if (!confirm())
        return(PATCHSUCCESS);

    ops.operation = OPERATION_DELETE;
    make_static_string(ops.del.fname, fname);
    return(serialize_operation(ar, &ops));
} /* put_delete */


/* get a DELETE operation from the mojopatch file... */
static int handle_delete_op(SerialArchive *ar, OperationType op, void *d)
{
    DeleteOperation *del = (DeleteOperation *) d;
    assert(op == OPERATION_DELETE);

    _current_operation("DELETE %s", final_path_element(del->fname));
    _log("DELETE %s", del->fname);

    if ( (info_only()) || (!confirm()) )
        return(PATCHSUCCESS);

    if (in_ignore_list(del->fname))
        return(PATCHSUCCESS);

    if (!file_exists(del->fname))
    {
        _log("file seems to be gone already.");
        return(PATCHSUCCESS);
    } /* if */

    if (file_is_directory(del->fname))
    {
        _fatal("Expected file, found directory!");
        return(PATCHERROR);
    } /* if */

    if (remove(del->fname) == -1)
    {
        _fatal("Error removing [%s]: %s.", del->fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    _log("done DELETE.");
    return(PATCHSUCCESS);
} /* handle_delete_op */


/* put a DELETEDIRECTORY operation in the mojopatch file... */
static int put_delete_dir(SerialArchive *ar, const char *fname)
{
    Operations ops;

    _current_operation("DELETEDIRECTORY %s", final_path_element(fname));
    _log("DELETEDIRECTORY %s", fname);

    if (!confirm())
        return(PATCHSUCCESS);

    if (in_ignore_list(fname))
        return(PATCHSUCCESS);

    ops.operation = OPERATION_DELETEDIRECTORY;
    make_static_string(ops.deldir.fname, fname);
    return(serialize_operation(ar, &ops));
} /* put_delete_dir */


static int delete_dir_tree(const char *fname)
{
    char filebuf[MAX_PATH];
    file_list *files = make_filelist(fname);
    file_list *i;
    int rc = 0;

    _log("Deleting directory tree %s", fname);

    for (i = files; i != NULL; i = i->next)
    {
        snprintf(filebuf, sizeof (filebuf), "%s%s%s", fname, PATH_SEP, i->fname);
        if (file_is_directory(filebuf))
            rc = delete_dir_tree(filebuf);
        else
        {
            _log("Deleting file %s from dir tree", filebuf);
            rc = (remove(filebuf) == -1) ? PATCHERROR : PATCHSUCCESS;
            if (rc == PATCHERROR)
                _fatal("failed to delete %s: %s.", filebuf, strerror(errno));
        } /* else */

        if (rc == PATCHERROR)
        {
            free_filelist(files);
            return(PATCHERROR);
        } /* if */
    } /* for */

    free_filelist(files);

    if (rmdir(fname) == -1)
    {
        _fatal("Error removing directory [%s]: %s.", fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    return(PATCHSUCCESS);
} /* delete_dir_tree */


/* get a DELETEDIRECTORY operation from the mojopatch file... */
static int handle_deldir_op(SerialArchive *ar, OperationType op, void *d)
{
    DeleteDirOperation *deldir = (DeleteDirOperation *) d;
    assert(op == OPERATION_DELETEDIRECTORY);

    _current_operation("DELETEDIRECTORY %s", final_path_element(deldir->fname));
    _log("DELETEDIRECTORY %s", deldir->fname);

    if ( (info_only()) || (!confirm()) )
        return(PATCHSUCCESS);

    if (in_ignore_list(deldir->fname))
        return(PATCHSUCCESS);

    if (!file_exists(deldir->fname))
    {
        _log("directory seems to be gone already.");
        return(PATCHSUCCESS);
    } /* if */

    if (!file_is_directory(deldir->fname))
    {
        _fatal("Expected directory, found file!");
        return(PATCHERROR);
    } /* if */

    if (!delete_dir_tree(deldir->fname))
        return(PATCHERROR);

    _log("done DELETEDIRECTORY.");
    return(PATCHSUCCESS);
} /* handle_deldir_op */


/* put an ADD operation in the mojopatch file... */
static int put_add(SerialArchive *ar, const char *fname)
{
    Operations ops;
    FILE *in = NULL;
    struct stat statbuf;
    int retval = PATCHERROR;

    _current_operation("%s %s", (replace) ? "ADDORREPLACE" : "ADD",
                        final_path_element(fname));
    _log("%s %s", (replace) ? "ADDORREPLACE" : "ADD", fname);

    if (!confirm())
        return(PATCHSUCCESS);

    if (in_ignore_list(fname))
        return(PATCHSUCCESS);

    if (stat(fname, &statbuf) == -1)
    {
        _fatal("Couldn't stat %s: %s.", fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    in = fopen(fname, "rb");
    if (in == NULL)
    {
        _fatal("failed to open [%s]: %s.", fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    if (md5sum(in, ops.add.md5, debug) == PATCHERROR)
        goto put_add_done;

    ops.operation = (replace) ? OPERATION_REPLACE : OPERATION_ADD;
    ops.add.fsize = statbuf.st_size;
    ops.add.mode = (unsigned int) statbuf.st_mode;
    make_static_string(ops.add.fname, fname);

    if (!serialize_operation(ar, &ops))
        goto put_add_done;

    if (!write_between_files(in, ar->io, ops.add.fsize, ZLIB_COMPRESS))
        goto put_add_done;

    assert(fgetc(in) == EOF);
    retval = PATCHSUCCESS;

put_add_done:
    if (in != NULL)
        fclose(in);
    return(retval);
} /* put_add */


/* get an ADD or REPLACE operation from the mojopatch file... */
static int handle_add_op(SerialArchive *ar, OperationType op, void *d)
{
    AddOperation *add = (AddOperation *) d;
    assert((op == OPERATION_ADD) || (op == OPERATION_REPLACE));
    int replace_ok = (op == OPERATION_REPLACE);
    int retval = PATCHERROR;
    FILE *io = NULL;
    int rc;

    _current_operation("%s %s", (replace_ok) ? "ADDORREPLACE" : "ADD",
                          final_path_element(add->fname));
    _log("%s %s", (replace_ok) ? "ADDORREPLACE" : "ADD", add->fname);

    if ( (info_only()) || (!confirm()) || (in_ignore_list(add->fname)) )
    {
        #if USE_ZLIB  /* skip through compressed file... */
            return(write_between_files_uncompress(ar->io, NULL, add->fsize, 1));
        #else
        if (fseek(ar->io, add->fsize, SEEK_CUR) < 0)
        {
            _fatal("Seek error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        #endif
        return(PATCHSUCCESS);
    } /* if */

    if (file_exists(add->fname))
    {
        if (replace_ok)
            unlink(add->fname);
        else
        {
            if (file_is_directory(add->fname))
            {
                _fatal("Error: [%s] already exists, but it's a directory!", add->fname);
                return(PATCHERROR);
            } /* if */

            _log("[%s] already exists...looking at md5sum...", add->fname);
            _current_operation("VERIFY %s", final_path_element(add->fname));
            io = fopen(add->fname, "rb");
            if (io == NULL)
            {
                _fatal("Failed to open added file for read: %s.", strerror(errno));
                goto handle_add_done;
            } /* if */
        
            if (verify_md5sum(add->md5, NULL, io, 1) == PATCHERROR)
                goto handle_add_done;

            _log("Okay; file matches what we expected.");
            fclose(io);

            if (fseek(ar->io, add->fsize, SEEK_CUR) < 0)
            {
                _fatal("Seek error: %s.", strerror(errno));
                return(PATCHERROR);
            } /* if */

            return(PATCHSUCCESS);
        } /* else */
    } /* if */

    io = fopen(add->fname, "wb");
    if (io == NULL)
    {
        _fatal("Error creating [%s]: %s.", add->fname, strerror(errno));
        goto handle_add_done;
    } /* if */

    rc = write_between_files(ar->io, io, add->fsize, ZLIB_UNCOMPRESS);
    if (rc == PATCHERROR)
        goto handle_add_done;

    if (fclose(io) == EOF)
    {
        _fatal("Error: Couldn't flush output: %s.", strerror(errno));
        goto handle_add_done;
    } /* if */

    chmod(add->fname, (mode_t) add->mode);  /* !!! FIXME: Should this be an error condition? */

    _current_operation("VERIFY %s", final_path_element(add->fname));
    io = fopen(add->fname, "rb");
    if (io == NULL)
    {
        _fatal("Failed to open added file for read: %s.", strerror(errno));
        goto handle_add_done;
    } /* if */
        
    if (verify_md5sum(add->md5, NULL, io, 1) == PATCHERROR)
        goto handle_add_done;

    retval = PATCHSUCCESS;
    _log("done %s.", (replace_ok) ? "ADDORREPLACE" : "ADD");

handle_add_done:
    if (io != NULL)
        fclose(io);

    return(retval);
} /* handle_add_op */


static int handle_replace_op(SerialArchive *ar, OperationType op, void *d)
{
    assert(op == OPERATION_REPLACE);
    return(handle_add_op(ar, op, d));
} /* handle_replace_op */


static int put_add_for_wholedir(SerialArchive *ar, const char *base);


/* put an ADDDIRECTORY operation in the mojopatch file... */
static int put_add_dir(SerialArchive *ar, const char *fname)
{
    Operations ops;
    struct stat statbuf;

    _current_operation("ADDDIRECTORY %s", final_path_element(fname));
    _log("ADDDIRECTORY %s", fname);

    if (!confirm())
        return(PATCHSUCCESS);

    if (in_ignore_list(fname))
        return(PATCHSUCCESS);

    if (stat(fname, &statbuf) == -1)
    {
        _fatal("Couldn't stat %s: %s.", fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    ops.operation = OPERATION_ADDDIRECTORY;
    ops.adddir.mode = (unsigned int) statbuf.st_mode;
    make_static_string(ops.adddir.fname, fname);

    if (!serialize_operation(ar, &ops))
        return(PATCHERROR);

    /* must add contents of dir after dir itself... */
    if (put_add_for_wholedir(ar, fname) == PATCHERROR)
        return(PATCHERROR);

    return(PATCHSUCCESS);
} /* put_add_dir */


/* get an ADDDIRECTORY operation from the mojopatch file... */
static int handle_adddir_op(SerialArchive *ar, OperationType op, void *d)
{
    AddDirOperation *adddir = (AddDirOperation *) d;
    assert(op == OPERATION_ADDDIRECTORY);
    
    _current_operation("ADDDIRECTORY %s", final_path_element(adddir->fname));
    _log("ADDDIRECTORY %s", adddir->fname);

    if ( (info_only()) || (!confirm()) || (in_ignore_list(adddir->fname)) )
        return(PATCHSUCCESS);

    if (file_exists(adddir->fname))
    {
        if (file_is_directory(adddir->fname))
        {
            _log("[%s] already exists.", adddir->fname);
            return(PATCHSUCCESS);
        } /* if */
        else
        {
            _fatal("[%s] already exists, but it's a file!", adddir->fname);
            return(PATCHERROR);
        } /* else */
    } /* if */

    if (mkdir(adddir->fname, S_IRWXU) == -1)
    {
        _fatal("Error making directory [%s]: %s.", adddir->fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    /* !!! FIXME: Pass this to mkdir? */
    chmod(adddir->fname, (mode_t) adddir->mode);  /* !!! FIXME: Should this be an error condition? */

    _log("done ADDDIRECTORY.");
    return(PATCHSUCCESS);
} /* handle_adddir_op */


/* put add operations for each file in (base). Recurses into subdirs. */
static int put_add_for_wholedir(SerialArchive *ar, const char *base)
{
    char filebuf[MAX_PATH];
    file_list *files = make_filelist(base);
    file_list *i;
    int rc = 0;

    for (i = files; i != NULL; i = i->next)
    {
        snprintf(filebuf, sizeof (filebuf), "%s%s%s", base, PATH_SEP, i->fname);

        /* put_add_dir recurses back into this function. */
        if (file_is_directory(filebuf))
            rc = put_add_dir(ar, filebuf);
        else
            rc = put_add(ar, filebuf);

        if (rc == PATCHERROR)
        {
            free_filelist(files);
            return(PATCHERROR);
        } /* if */
    } /* for */

    free_filelist(files);
    return(PATCHSUCCESS);
} /* put_add_for_wholedir */

/* put a DONE operation in the mojopatch file... */
static int put_done(SerialArchive *ar)
{
    Operations ops;

    _current_operation("DONE");
    _log("DONE");

    ops.operation = OPERATION_DONE;
    return(serialize_operation(ar, &ops));
} /* put_done */


static int md5sums_match(const char *fname1, const char *fname2,
                         md5_byte_t *md5_1, md5_byte_t *md5_2)
{
    FILE *in;

    in = fopen(fname1, "rb");
    if (in == NULL)
        return(0);

    if (md5sum(in, md5_1, 0) == PATCHERROR)
        return(0);

    fclose(in);

    in = fopen(fname2, "rb");
    if (in == NULL)
        return(0);

    if (md5sum(in, md5_2, 0) == PATCHERROR)
        return(0);

    fclose(in);

    return(memcmp(md5_1, md5_2, 16) == 0);
} /* md5sums_match */


/* put a PATCH operation in the mojopatch file... */
static int put_patch(SerialArchive *ar, const char *fname1, const char *fname2)
{
    Operations ops;
    FILE *deltaio = NULL;
    int retval = PATCHERROR;
    struct stat statbuf;

    _current_operation("VERIFY %s", final_path_element(fname2));
	if (md5sums_match(fname1, fname2, ops.patch.md5_1, ops.patch.md5_2))
        return(PATCHSUCCESS);

    if (alwaysadd)  /* add it instead of patch it... */
    {
        int origreplace = replace;
        replace = 1;  /* must ADDORREPLACE, as file will definitely exist. */
        retval = put_add(ar, fname2);
        replace = origreplace;  /* reset original value. */
        return(retval);
    } /* if */

    _current_operation("PATCH %s", final_path_element(fname2));
    _log("PATCH %s", fname2);

    if (!confirm())
        return(PATCHSUCCESS);

    if (in_ignore_list(fname2))
        return(PATCHSUCCESS);

    if (stat(fname2, &statbuf) == -1)
    {
        _fatal("Couldn't stat %s: %s.", fname2, strerror(errno));
        return(PATCHERROR);
    } /* if */

    if ( (!_do_xdelta("delta -n --maxmem=%dM \"%s\" \"%s\" \"%s\"", maxxdeltamem, fname1, fname2, patchtmpfile)) ||
         (!get_file_size(patchtmpfile, &ops.patch.deltasize)) )
    {
        /* !!! FIXME: Not necessarily true. */
        _fatal("there was a problem running xdelta.");
        return(PATCHERROR);
    } /* if */

    ops.operation = OPERATION_PATCH;
    ops.patch.mode = (unsigned int) statbuf.st_mode;
    ops.patch.fsize = statbuf.st_size;
    make_static_string(ops.patch.fname, fname2);
    if (!serialize_operation(ar, &ops))
        return(PATCHERROR);

    deltaio = fopen(patchtmpfile, "rb");
    if (deltaio == NULL)
    {
        _fatal("couldn't read %s: %s.", patchtmpfile, strerror(errno));
        return(PATCHERROR);
    } /* if */

    retval = write_between_files(deltaio, ar->io,
                                 ops.patch.deltasize,
                                 ZLIB_NONE);

    assert(fgetc(deltaio) == EOF);
    fclose(deltaio);
    unlink(patchtmpfile);
    return(retval);
} /* put_patch */


/* get a PATCH operation from the mojopatch file... */
static int handle_patch_op(SerialArchive *ar, OperationType op, void *d)
{
    PatchOperation *patch = (PatchOperation *) d;
	md5_byte_t md5result[16];
    FILE *f = NULL;
    FILE *deltaio = NULL;
    int rc;

    assert(op == OPERATION_PATCH);

    _log("PATCH %s", patch->fname);

    if ( (info_only()) || (!confirm()) || (in_ignore_list(patch->fname)) )
    {
        if (fseek(ar->io, patch->deltasize, SEEK_CUR) < 0)
        {
            _fatal("Seek error: %s.", strerror(errno));
            return(PATCHERROR);
        } /* if */
        return(PATCHSUCCESS);
    } /* if */

    f = fopen(patch->fname, "rb");
    if (f == NULL)
    {
        _fatal("Failed to open [%s]: %s.", patch->fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    _current_operation("VERIFY %s", final_path_element(patch->fname));
    rc = verify_md5sum(patch->md5_1, md5result, f, 0);
    fclose(f);
    if (rc == PATCHERROR)
    {
        if (memcmp(patch->md5_2, md5result, sizeof (patch->md5_2)) == 0)
        {
            _log("Okay; file matches patched md5sum. It's already patched.");
            if (fseek(ar->io, patch->deltasize, SEEK_CUR) < 0)
            {
                _fatal("Seek error: %s.", strerror(errno));
                return(PATCHERROR);
            } /* if */
            return(PATCHSUCCESS);
        } /* if */
        return(PATCHERROR);
    } /* if */

    unlink(patchtmpfile2); /* just in case... */

    _current_operation("PATCH %s", final_path_element(patch->fname));
    deltaio = fopen(patchtmpfile2, "wb");
    if (deltaio == NULL)
    {
        _fatal("Failed to open [%s]: %s.", patchtmpfile2, strerror(errno));
        return(PATCHERROR);
    } /* if */

    rc = write_between_files(ar->io, deltaio, patch->deltasize, ZLIB_NONE);
    fclose(deltaio);
    if (rc == PATCHERROR)
    {
        unlink(patchtmpfile2);
        return(PATCHERROR);
    } /* if */

    if (!_do_xdelta("patch --maxmem=%dM \"%s\" \"%s\" \"%s\"", maxxdeltamem, patchtmpfile2, patch->fname, patchtmpfile))
    {
        _fatal("xdelta failed.");
        return(PATCHERROR);
    } /* if */

    unlink(patchtmpfile2);  /* ditch temp delta file... */

    f = fopen(patchtmpfile, "rb");
    if (f == NULL)
    {
        _fatal("Failed to open [%s] for read: %s.", patchtmpfile, strerror(errno));
        return(PATCHERROR);
    } /* if */

    _current_operation("VERIFY %s", final_path_element(patch->fname));
    rc = verify_md5sum(patch->md5_2, NULL, f, 1);
    fclose(f);
    if (rc == PATCHERROR)
        return(PATCHERROR);

    if (do_rename(patchtmpfile, patch->fname) == -1)
    {
        _fatal("Error replacing [%s] with tempfile: %s.", patch->fname, strerror(errno));
        return(PATCHERROR);
    } /* if */

    chmod(patch->fname, (mode_t) patch->mode);  /* !!! FIXME: fatal error? */

    _log("done PATCH.");
    return(PATCHSUCCESS);
} /* handle_patch_op */

/* get a DONE operation from the mojopatch file... */
static int handle_done_op(SerialArchive *ar, OperationType op, void *d)
{
    /*DoneOperation *done = (DoneOperation *) d;*/
    assert(op == OPERATION_DONE);
    _log("DONE");
    /* This might set some global state at some point... */
    return(PATCHSUCCESS);
} /* handle_done_op */


static int compare_directories(SerialArchive *ar,
                               const char *base1,
                               const char *base2)
{
    int retval = PATCHERROR;
    char filebuf1[MAX_PATH];
    char filebuf2[MAX_PATH]; /* can you feel the stack screaming? */
    const char *base2checked = *base2 ? base2 : ".";
    file_list *files1 = make_filelist(base1);
    file_list *files2 = NULL;
    file_list *i;

    /* may be recursive compare on deleted dir. */
    if (file_exists(base2checked))
        files2 = make_filelist(base2checked);

    assert(*base1);

    _current_operation("Examining %s", final_path_element(base2checked));
    _dlog("Examining %s and %s", base1, base2checked);

    _dlog("(looking for files that need deletion...)");

    /* check for files removed in newer version... */
    for (i = files1; i != NULL; i = i->next)
    {
        _dlog("([%s]...)", i->fname);

        snprintf(filebuf2, sizeof (filebuf2), "%s%s%s", base2,
                    *base2 ? PATH_SEP : "", i->fname);

        if (!file_exists(filebuf2))
        {
            int rc = 0;

            snprintf(filebuf1, sizeof (filebuf1), "%s%s%s", base1, PATH_SEP, i->fname);
            if (!file_is_directory(filebuf1))
                rc = put_delete(ar, filebuf2);
            else
            {
                rc = compare_directories(ar, filebuf1, filebuf2);
                if (rc != PATCHERROR)
                    rc = put_delete_dir(ar, filebuf2);
            } /* else */

            if (rc == PATCHERROR)
                goto dircompare_done;
        } /* if */
    } /* for */

    _dlog("(looking for files that need addition...)");

	/* check for files added in newer version... */
    for (i = files2; i != NULL; i = i->next)
    {
        #if PLATFORM_MACOSX
        /* !!! FIXME: Make this an option. */
        if (strcmp(i->fname, ".DS_Store") == 0)
        {
            _dlog("...skipped .DS_Store file...");
            continue;
        } /* if */
        #endif

        _dlog("([%s]...)", i->fname);

        snprintf(filebuf1, sizeof (filebuf1), "%s%s%s", base1, PATH_SEP, i->fname);
        snprintf(filebuf2, sizeof (filebuf2), "%s%s%s", base2,
                    *base2 ? PATH_SEP : "", i->fname);

        if (file_exists(filebuf1))  /* exists in both dirs; do compare. */
        {
            if (file_is_directory(filebuf2))
            {
                    /* probably a bad sign ... */
                if (!file_is_directory(filebuf1))
                {
                    _log("%s is a directory, but %s is not!", filebuf2, filebuf1);
                    if (put_delete(ar, filebuf2) == PATCHERROR)
                        goto dircompare_done;

                    if (put_add_dir(ar, filebuf2) == PATCHERROR)
                        goto dircompare_done;
                } /* if */

                if (compare_directories(ar, filebuf1, filebuf2) == PATCHERROR)
                    goto dircompare_done;
            } /* if */

            else  /* new item is not a directory. */
            {
                    /* probably a bad sign ... */
                if (file_is_directory(filebuf1))
                {
                    _log("Warning: %s is a directory, but %s is not!", filebuf1, filebuf2);
                    if (put_delete_dir(ar, filebuf2) == PATCHERROR)
                        goto dircompare_done;

                    if (put_add(ar, filebuf2) == PATCHERROR)
                        goto dircompare_done;
                } /* if */

                else
                {
                    /* may not put anything if files match... */
                    if (put_patch(ar, filebuf1, filebuf2) == PATCHERROR)
                        goto dircompare_done;
                } /* else */
            } /* else */
        } /* if */

        else  /* doesn't exist in second dir; do add. */
        {
            if (file_is_directory(filebuf2))
            {
                if (put_add_dir(ar, filebuf2) == PATCHERROR)
                    goto dircompare_done;
            } /* if */

            else
            {
                if (put_add(ar, filebuf2) == PATCHERROR)
                    goto dircompare_done;
            } /* else */
        } /* else */
    } /* for */

    retval = PATCHSUCCESS;

dircompare_done:
    free_filelist(files1);
    free_filelist(files2);
    return(retval);
} /* compare_directories */


static char *read_whole_file(const char *fname)
{
    int i;
    int rc;
    FILE *io = NULL;
    unsigned int fsize = 0;
    char *retval = NULL;

    if (!get_file_size(fname, &fsize))
    {
        _fatal("Can't get filesize for [%s]...file missing?", fname);
        return(NULL);
    } /* if */

    if ( (retval = (char *) malloc(fsize + 1)) == NULL )
    {
        _fatal("Out of memory.");
        return(NULL);
    } /* if */

    if ( (io = fopen(fname, "r")) == NULL )
    {
        _fatal("Can't open [%s].", fname);
        free(retval);
        return(NULL);
    } /* if */

    rc = fread(retval, fsize, 1, io);
    fclose(io);

    if (rc != 1)
    {
        _fatal("Read on [%s] failed: %s", fname, strerror(errno));
        free(retval);
        return(NULL);
    } /* if */

    /* This considers it an error condition to have a null char in the file. */
    for (i = 0; i < fsize; i++)
    {
        if (retval[i] == '\0')
        {
            _fatal("null char in read_whole_file.");
            free(retval);
            return(NULL);
        } /* if */
    } /* for */
    retval[fsize] = '\0';

    return(retval);
} /* read_whole_file */


static int create_patchfile(void)
{
    SerialArchive ar;
    int retval = PATCHSUCCESS;
    unsigned int fsize;
    char *real1 = NULL;
    char *real2 = NULL;
    char *real3 = NULL;
    char *readmefull = NULL;

    if (header.readmefname[0])  /* user specified a README? */
    {
        const char *ptr = NULL;
        readmefull = alloca(strlen(header.readmefname) + 1);

        /* header stores filename, not path. Chop it out, but retain orig. */
        strcpy(readmefull, header.readmefname);
        ptr = final_path_element(readmefull);
        if (ptr != readmefull)
            make_static_string(header.readmefname, ptr);
    } /* if */

    if (*header.product == '\0')  /* specified on the commandline. */
    {
        _fatal("No product name specified, but it's required!");
        return(PATCHERROR);
    } /* if */

    if (*header.identifier == '\0')  /* specified on the commandline. */
    {
        if (!ui_prompt_ny("No identifier specified. Is this intentional?"))
            return(PATCHERROR);
    } /* if */

    if (*header.version == '\0')  /* specified on the commandline. */
    {
        if (!ui_prompt_ny("No version specified. Is this intentional?"))
            return(PATCHERROR);
    } /* if */

    if (*header.newversion == '\0')  /* specified on the commandline. */
    {
        if (!ui_prompt_ny("No newversion specified. Is this intentional?"))
            return(PATCHERROR);
    } /* if */

    real1 = get_realpath(dir1);
    if (real1 == NULL)
    {
        _fatal("Couldn't get realpath of [%s].", dir1);
        return(PATCHERROR);
    } /* if */

    real2 = get_realpath(dir2);
    if (real2 == NULL)
    {
        _fatal("Couldn't get realpath of [%s].", dir2);
        return(PATCHERROR);
    } /* if */

    real3 = get_realpath(patchfile);
    if (real3 == NULL)
    {
        _fatal("Couldn't get realpath of [%s].", patchfile);
        return(PATCHERROR);
    } /* if */

    if (!appending)
        unlink(patchfile);  /* just in case. */

    if (!open_serialized_archive(&ar, patchfile, 0, NULL, NULL))
    {
        free(real1);
        free(real2);
        free(real3);
        _fatal("Couldn't open [%s]: %s.", patchfile, strerror(errno));
        return(PATCHERROR);
    } /* if */

    if (chdir(real2) != 0)
    {
        close_serialized_archive(&ar);
        free(real1);
        free(real2);
        free(real3);
        _fatal("Couldn't chdir to [%s]: %s.", real2, strerror(errno));
        return(PATCHERROR);
    } /* if */
    free(real2);

    if (readmefull != NULL)
    {
        header.readmedata = read_whole_file(readmefull);
        if (!header.readmedata)
        {
            close_serialized_archive(&ar);
            free(real1);
            free(real3);
            return(PATCHERROR);
        } /* if */
    } /* if */
    else
    {
        header.readmedata = malloc(1);  /* bleh. */
        header.readmedata[0] = '\0';
    } /* else */

    if (!serialize_header(&ar, &header, NULL))
    {
        close_serialized_archive(&ar);
        free(real1);
        free(real3);
        free(header.readmedata);
        return(PATCHERROR);
    } /* if */

    free(header.readmedata);

    retval = compare_directories(&ar, real1, "");

    free(real1);

    if (retval != PATCHERROR)
        retval = put_done(&ar);

    if (!close_serialized_archive(&ar))
    {
        free(real3);
        _fatal("Couldn't close [%s]: %s.", patchfile, strerror(errno));
        retval = PATCHERROR;
    } /* if */

    if (!get_file_size(real3, &fsize))
    {
        _fatal("Couldn't get size of [%s]: %s.", patchfile, strerror(errno));
        retval = PATCHERROR;
    } /* if */
    free(real3);

    if (retval == PATCHERROR)
        _fatal("THE FILE [%s] IS LIKELY INCOMPLETE. DO NOT USE!", patchfile);
    else
    {
        if (!quietonsuccess)
            ui_success("Patchfile successfully created.");
        _log("%ld bytes in the file [%s].", fsize, patchfile);
    } /* else */

    return(retval);
} /* create_patchfile */


static int do_patch_operations(SerialArchive *ar,
                               int do_progress,
                               long patchfile_size)
{
    Operations ops;
    memset(&ops, '\0', sizeof (ops));

    if (info_only())
        _log("These are the operations we would perform if patching...");

    do
    {
        ui_pump();

        if (do_progress)
        {
            long pos = ftell(ar->io);
            int progress = (int) (((float)pos)/((float)patchfile_size)*100.0f);
            ui_total_progress((pos == -1) ? -1 : progress);
        } /* if */

        if (!serialize_operation(ar, &ops))
            return(PATCHERROR);

        assert((ops.operation >= 0) && (ops.operation < OPERATION_TOTAL));
        if (!operation_handlers[ops.operation](ar, ops.operation, &ops))
            return(PATCHERROR);
    } while (ops.operation != OPERATION_DONE);

    return(PATCHSUCCESS);
} /* do_patch_operations */


static inline void header_log(const char *str, const char *val)
{
    _log(str, *val ? val : "(blank)");
} /* header_log */


static int show_and_install_readme(const char *fname, const char *text)
{
    FILE *io = fopen(fname, "wb");
    if (io == NULL)
    {
        _fatal("Failed to open [%s] for writing.", fname);
        return(0);
    } /* if */

    /* !!! FIXME: "text" may be binary data, not an asciz string... */
    fputs(text, io);  /* !!! FIXME: error checking! */
    fclose(io);
    return(ui_show_readme(fname, text));
} /* show_and_install_readme */


static int manually_locate_product(const char *name, char *buf, size_t bufsize)
{
    const char *promptfmt = "We can't find your \"%s\" installation."
                            " Would you like to show us where it is?";
    char *promptstr = alloca(strlen(name) + strlen(promptfmt) + 1);

    if (promptstr == NULL)
    {
        _fatal("Out of memory.");
        return(0);
    } /* if */
    sprintf(promptstr, promptfmt, name);

    if (!ui_prompt_yn(promptstr))
    {
        _log("User chose not to manually locate installation");
        return(0);
    } /* if */

    return(ui_file_picker(buf, bufsize));
} /* manually_locate_product */


static int chdir_by_identifier(const char *name, const char *str)
{
    char buf[MAXPATHLEN];
    int hasident = ((str != NULL) && (*str));
    int found = 0;

    if (hasident)
    {
        found = locate_product_by_identifier(str, buf, sizeof (buf));

        /*
         * Platform-specific issue...it's there, but there's a problem, like
         *  MacOS found it, but it's in the trashcan, etc. The platform
         *  driver should call the UI routines to explain the issue.
         */
        if (found == -1)
            return(0);

        if (!found)
            _log("Couldn't find product. Perhaps it isn't installed?");
    } /* if */

    if (!found) /* No identifier, or platform layer couldn't find it. */
    {
        if (!manually_locate_product(name, buf, sizeof (buf)))
        {
            _fatal("We can't patch the product if we can't find it!");
            return(0);
        } /* if */
    } /* if */

    _log("I think the product is installed at [%s].", buf);

    if (chdir(buf) != 0)
    {
        _fatal("Failed to change to product's installation directory.");
        return(0);
    } /* if */

    return(1);
} /* chdir_by_identifier */


static int run_script(const char *name)
{
    SpawnResult rc;
    char cwd[MAX_PATH];

    if (info_only())  /* don't run scripts unless applying a patch... */
        return(1);

    if (getcwd(cwd, sizeof (cwd)) == NULL)
    {
        _fatal("Couldn't determine current working directory!");
        return(0);
    } /* if */

    if (patchfiledir != NULL)
    {
        if (chdir(patchfiledir) == -1)
        {
            _fatal("Failed to change directory to \"%s\".", patchfiledir);
            return(0);
        } /* if */
    } /* if */

    rc = spawn_script(name, cwd);

    if (patchfiledir != NULL)
    {
        if (chdir(cwd) == -1)
        {
            _fatal("Failed to change directory to \"%s\".", cwd);
            return(0);
        } /* if */
    } /* if */

    if (rc == SPAWN_FILENOTFOUND)
        return(1);  /* "success" */

    else if (rc == SPAWN_FAILED)
        _fatal("Failed to run %s script!");

    return(rc == SPAWN_RETURNGOOD);
} /* run_script */


static int process_patch_header(SerialArchive *ar, PatchHeader *h)
{
    int retval = PATCHSUCCESS;

    _log("============Starting a new patch!============");
    header_log("Product to patch: \"%s\".", h->product);
    header_log("Product identifier: \"%s\".", h->identifier);
    header_log("Patch from version: \"%s\".", h->version);
    header_log("Patch to version: \"%s\".", h->newversion);
    header_log("Readme: \"%s\".", h->readmefname);
    header_log("Renamedir: \"%s\".", h->renamedir);
    header_log("UI titlebar: \"%s\".", h->titlebar);
    header_log("Startup msg: \"%s\".", h->startupmsg);

    /* Fill in a default titlebar if needed. */
    if (*h->titlebar == '\0')
    {
        if (*h->product != '\0')
            make_static_string(h->titlebar, h->product);
        else
        {
            char defstr[128];
            snprintf(defstr, sizeof (defstr) - 1, "MojoPatch %s", VERSION);
            make_static_string(h->titlebar, defstr);
        } /* else */
        _dlog("Defaulted UI titlebar to [%s].", h->titlebar);
    } /* if */

    ui_title(h->titlebar);

    /* show a message box before starting... */
    if (*(h->startupmsg))
    {
        if (!info_only())
            ui_msgbox(h->startupmsg);
    } /* if */

    if (!info_only())
    {
        if (!chdir_by_identifier(h->product, h->identifier))
            retval = PATCHERROR;
        else
        {
            IsPatchable rc;
            rc = check_product_version(h->identifier, h->version, h->newversion);
            if (rc == ISPATCHABLE_ERROR)
                retval = PATCHERROR;
            else if ((rc == ISPATCHABLE_MATCHES) || (rc == ISPATCHABLE_NO))
                skip_patch = 1;
            else
            {
                assert(rc == ISPATCHABLE_YES);
                if (!run_script("prepatch"))
                    retval = PATCHERROR;
                else if (*h->readmefname)
                    retval = show_and_install_readme(h->readmefname, h->readmedata);
            } /* else */
        } /* else */
    } /* if */

    free(h->readmedata);
    h->readmedata = NULL;

    return(retval);
} /* process_patch_header */


static char *get_real_filedir(const char *fname)
{
    char *ptr;
    char *buf = (char *) alloca(strlen(fname) + 1);
    strcpy(buf, fname);

    assert(strlen(PATH_SEP) == 1);  /* need better code if this doesn't fly. */
    ptr = strrchr(buf, (PATH_SEP)[0]);
    if (!ptr)
        strcpy(buf, ".");  /* current dir */
    else
        *ptr = '\0';  /* chop filename. */

    return(get_realpath(buf));
} /* get_real_filedir */


static int do_patching(void)
{
    SerialArchive ar;
    int report_error = 0;
    int retval = PATCHERROR;
    int installed_patches = 0;
    int skipped_patches = 0;
    unsigned int file_size = 0;
    int do_progress = 0;

    ui_total_progress(do_progress ? 0 : -1);
    ui_pump();

    if (!open_serialized_archive(&ar, patchfile, 1, &do_progress, &file_size))
        return(PATCHERROR);

    if ((patchfiledir = get_real_filedir(patchfile)) == NULL)
    {
        _fatal("internal error!");  /* !!! FIXME: better error? */
        return(PATCHERROR);
    } /* if */

    if (file_size == 0)
        do_progress = 0;  /* prevent a division by zero. */

    while (1)
    {
        int legitEOF = 0;
        if (!serialize_header(&ar, &header, &legitEOF))
            goto do_patching_done;

        if (legitEOF)  /* actually end of file, so bail. */
            break;

        if (process_patch_header(&ar, &header) == PATCHERROR)
            goto do_patching_done;

        report_error = 1;
        if (do_patch_operations(&ar, do_progress, file_size) == PATCHERROR)
            goto do_patching_done;

        if ((!info_only()) && (!skip_patch))
        {
            _current_operation("Updating product version...");
            ui_total_progress(-1);
            if ( (*header.newversion) && (!update_version(header.newversion)) )
                goto do_patching_done;

            if (*header.renamedir)
            {
                char cwdbuf[MAX_PATH];
                if (get_current_dir(cwdbuf, sizeof (cwdbuf)) != NULL)
                {
                    chdir("..");
                    rename(cwdbuf, header.renamedir); /* !!! FIXME: retval? */
                    chdir(header.renamedir);  /* just in case */
                } /* if */
            } /* if */
        } /* if */

        if (skip_patch)
        {
            skipped_patches++;
            skip_patch = 0;  /* reset for next patch... */
        } /* if */
        else
        {
            installed_patches++;
            if (!run_script("postpatch"))
                goto do_patching_done;
        } /* else */

        /* !!! FIXME: This loses command line overrides! */
        memset(&header, '\0', sizeof (header));

        report_error = 0;
    } /* while */

    retval = PATCHSUCCESS;
    ui_total_progress(100);
    if (!info_only())
    {
        if (installed_patches == 0)
            _fatal("No patches were applied to your installation.");
        else if (!quietonsuccess)
            ui_success("Patching successful!");
    } /* if */

do_patching_done:
    close_serialized_archive(&ar);

    if (retval == PATCHERROR)
    {
        ui_total_progress(-1);
        if (report_error)
        {
            _fatal("There were problems, so I'm aborting.");
            if (!info_only())
                _fatal("The product is possibly damaged and requires a fresh installation.");
        } /* if */
    } /* if */
    else
    {
        run_script("patchingdone");
    } /* else */

    free(patchfiledir);
    patchfiledir = NULL;

    return(retval);
} /* do_patching */


static int do_usage(const char *argv0)
{
    _log("");
    _log("USAGE: %s --create <file.mojopatch> <dir1> <dir2>", argv0);
    _log("   or: %s --info <file.mojopatch>", argv0);
    _log("   or: %s <file.mojopatch>", argv0);
    _log("");
    _log("  You may also specify:");
    _log("    --product (Product name)");
    _log("    --identifier (Product identifier for locating installation)");
    _log("    --version (Product version to patch against)");
    _log("    --newversion (Product version to patch up to)");
    _log("    --replace (specify ADDs overwrite, at create time or override)");
    _log("    --append (creation appends to existing patchfile)");
    _log("    --alwaysadd (put ADDs instead of PATCHs into the patchfile)");
    _log("    --quietonsuccess (Don't do msgbox on successful finish)");
    _log("    --startupmsg (msgbox text to show at startup)");
    _log("    --ui (UI driver to use for this run)");
    _log("    --readme (README filename to display/install)");
    _log("    --renamedir (What patched dir should be called)");
    _log("    --zliblevel (compression, 0-9: 0 == fastest, 9 == best)");
    _log("    --titlebar (What UI's window's titlebar should say)");
    _log("    --ignore (Ignore specific files/dirs)");
    _log("    --confirm (Make process confirm each step)");
    _log("    --debug (spew debugging output)");
    _log("");
    return(0);
} /* do_usage */


static int set_command_or_abort(PatchCommands cmd)
{
    if (command != COMMAND_NONE)
    {
        _fatal("You've specified more than one command!");
        return(0);
    } /* if */

    command = cmd;
    return(1);
} /* set_command_or_abort */


static int parse_cmdline(int argc, char **argv)
{
    int i;
    int nonoptcount = 0;
    char **nonoptions = (char **) alloca(sizeof (char *) * argc);

    if (nonoptions == NULL)
    {
        _fatal("Out of memory!");
        return(0);
    } /* if */

    if (argc <= 1)
    {
        if (file_exists(DEFAULT_PATCHFILENAME))
            nonoptions[nonoptcount++] = DEFAULT_PATCHFILENAME;
        else
            return(do_usage(argv[0]));
    } /* if */

    for (i = 1; i < argc; i++)
    {
        int okay = 1;

        if (strncmp(argv[i], "--", 2) != 0)
        {
            nonoptions[nonoptcount++] = argv[i];
            continue;
        } /* if */

        if (strcmp(argv[i], "--create") == 0)
            okay = set_command_or_abort(COMMAND_CREATE);
        else if (strcmp(argv[i], "--info") == 0)
            okay = set_command_or_abort(COMMAND_INFO);
        else if (strcmp(argv[i], "--confirm") == 0)
            interactive = 1;
        else if (strcmp(argv[i], "--debug") == 0)
            debug = 1;
        else if (strcmp(argv[i], "--replace") == 0)
            replace = 1;
        else if (strcmp(argv[i], "--append") == 0)
            appending = 1;
        else if (strcmp(argv[i], "--alwaysadd") == 0)
            alwaysadd = 1;
        else if (strcmp(argv[i], "--quietonsuccess") == 0)
            quietonsuccess = 1;
        else if (strcmp(argv[i], "--product") == 0)
            make_static_string(header.product, argv[++i]);
        else if (strcmp(argv[i], "--identifier") == 0)
            make_static_string(header.identifier, argv[++i]);
        else if (strcmp(argv[i], "--version") == 0)
            make_static_string(header.version, argv[++i]);
        else if (strcmp(argv[i], "--newversion") == 0)
            make_static_string(header.newversion, argv[++i]);
        else if (strcmp(argv[i], "--readme") == 0)
            make_static_string(header.readmefname, argv[++i]);
        else if (strcmp(argv[i], "--renamedir") == 0)
            make_static_string(header.renamedir, argv[++i]);
        else if (strcmp(argv[i], "--titlebar") == 0)
            make_static_string(header.titlebar, argv[++i]);
        else if (strcmp(argv[i], "--startupmsg") == 0)
            make_static_string(header.startupmsg, argv[++i]);
        else if (strcmp(argv[i], "--ui") == 0)
            i++;  /* (really handled elsewhere.) Just skip ui driver name. */
        else if (strcmp(argv[i], "--zliblevel") == 0)
        {
            zliblevel = atoi(argv[++i]);
            if ((zliblevel < 0) || (zliblevel > 9))
            {
                _fatal("zliblevel must be between 0 and 9");
                return(do_usage(argv[0]));
            } /* if */
        } /* else if */
        else if (strcmp(argv[i], "--ignore") == 0)
        {
            ignorecount++;
            ignorelist = (char **) realloc(ignorelist, sizeof (char *) * ignorecount);
            /* !!! FIXME: Check retval. */
            ignorelist[ignorecount-1] = argv[++i];
        } /* else if */
        else
        {
            _fatal("Error: Unknown option [%s].", argv[i]);
            return(do_usage(argv[0]));
        } /* else */

        if (!okay)
            return(0);
    } /* for */

    if (command == COMMAND_NONE)
        command = COMMAND_DOPATCHING;

    switch (command)
    {
        case COMMAND_INFO:
        case COMMAND_DOPATCHING:
            if (nonoptcount != 1)
            {
                _fatal("Error: Wrong arguments.");
                return(do_usage(argv[0]));
            } /* if */
            patchfile = nonoptions[0];
            break;

        case COMMAND_CREATE:
            if (nonoptcount != 3)
            {
                _fatal("Error: Wrong arguments.");
                return(do_usage(argv[0]));
            } /* if */
                
            patchfile = nonoptions[0];
            dir1 = nonoptions[1];
            dir2 = nonoptions[2];
            break;

        default:
            assert(0);
            break;
    } /* switch */

    if (debug)
    {
        _dlog("debugging enabled.");
        _dlog("Interactive mode %senabled.", (interactive) ? "" : "NOT ");
        _dlog("ADDs are %spermitted to REPLACE.", (replace) ? "" : "NOT ");
        _dlog("Created patch will %sbe appended.", (appending) ? "" : "NOT ");
        _dlog("%sse ADDs instead of PATCHs.", (alwaysadd) ? "U" : "Do NOT u");
        _dlog("%seport success in UI", (quietonsuccess) ? "Don't r" : "R");
        _dlog("zliblevel == (%d).", (int) zliblevel);
        _dlog("command == (%d).", (int) command);
        _dlog("(%d) nonoptions:", nonoptcount);
        for (i = 0; i < nonoptcount; i++)
            _dlog(" [%s]", nonoptions[i]);
        _dlog("patchfile == [%s].", (patchfile) ? patchfile : "(null)");
        _dlog("dir1 == [%s].", (dir1) ? dir1 : "(null)");
        _dlog("dir2 == [%s].", (dir2) ? dir2 : "(null)");
        for (i = 0; i < ignorecount; i++)
            _dlog("ignoring [%s].", ignorelist[i]);
    } /* if */

    return(1);
} /* parse_cmdline */


/* !!! FIXME: signal_cleanup */

static int kickoff_ui(int argc, char **argv)
{
    int seen_ui = 0;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ui") == 0)
        {
            seen_ui = 1;
            if (ui_init(argv[++i]))
                return(1);
        } /* if */
    } /* for */

    if (!seen_ui)
    {
        if (ui_init(NULL))
            return(1);
    } /* if */

    fprintf(stderr, "MojoPatch: ui_init() failed!");  /* oh well. */
    return(0);
} /* kickoff_ui */


int mojopatch_main(int argc, char **argv)
{
	time_t starttime = time(NULL);
    int retval = PATCHSUCCESS;

    /* !!! FIXME: We need to serialize this, so we serialize it as uint32. */
    assert(sizeof (mode_t) <= sizeof (unsigned int));

    memset(&header, '\0', sizeof (header));

    if (!kickoff_ui(argc, argv))
        return(PATCHERROR);  /* oh well. */

    _log("MojoPatch %s starting up.", VERSION);

    if (!parse_cmdline(argc, argv))
    {
        ui_deinit();
        return(PATCHERROR);
    } /* if */

    if (!calc_tmp_filenames(&patchtmpfile, &patchtmpfile2))
    {
        _fatal("Internal error: Couldn't find scratch filenames.");
        ui_deinit();
        return(PATCHERROR);
    } /* if */
    _dlog("Temp filenames are [%s] and [%s].", patchtmpfile, patchtmpfile2);

    if (command == COMMAND_CREATE)
        retval = create_patchfile();
    else
        retval = do_patching();

    unlink(patchtmpfile);  /* just in case. */
    unlink(patchtmpfile2); /* just in case. */

    _log("MojoPatch %s shutting down.", VERSION);
    _dlog("(Total running time: %ld seconds.)", time(NULL) - starttime);

    ui_deinit();
    return(retval);
} /* mojopatch_main */

/* end of mojopatch.c ... */

